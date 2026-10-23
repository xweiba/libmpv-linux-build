// Regression test for nested HLS/DASH I/O through libmpv stream callbacks.
//
// Registers a "mediaio" protocol with mpv_stream_cb_add_ro() and plays HLS and
// DASH fixtures whose manifests, variant playlists and segments all use
// mediaio:// URIs. It checks that:
//   - every manifest/segment is read through the callback (FFmpeg patch
//     ffmpeg-segmented-custom-io + mpv patch mpv-nested-stream-callback),
//   - without allow_custom_io the segments are rejected (opt-in gate),
//   - absolute seeks re-open later segments through the callback,
//   - stop/destroy while a segment open or read is still pending neither
//     crashes nor hangs (ffmpeg-segmented-io-cancel + cancel detach),
//   - repeated load/stop cycles release every callback stream.
//
// Build: cc -O1 -g -Iinclude tests/mediaio_nested.c -o mediaio_nested -ldl -lpthread
// Run:   ./mediaio_nested /path/to/libmpv.so.2 /path/to/fixtures
// The fixtures are produced by tests/mediaio_fixtures.sh.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mpv/client.h>
#include <mpv/stream_cb.h>

#define PROTO "mediaio"
// Mirrors media-kit's demuxer-lavf-o for a player with a native media I/O
// provider (media_kit/lib/src/player/native/player/real.dart).
#define LAVF_COMMON "seg_max_retry=5,strict=experimental,allowed_extensions=ALL," \
    "hls_ad_filter=1,"
#define LAVF_WHITELIST "protocol_whitelist=[" PROTO ",file,crypto,data]"
#define LAVF_OPTS_ON LAVF_COMMON "allow_custom_io=1," LAVF_WHITELIST
#define LAVF_OPTS_OFF LAVF_COMMON LAVF_WHITELIST

static struct {
    mpv_handle *(*create)(void);
    int (*initialize)(mpv_handle *);
    void (*terminate_destroy)(mpv_handle *);
    int (*set_option_string)(mpv_handle *, const char *, const char *);
    int (*command)(mpv_handle *, const char **);
    char *(*get_property_string)(mpv_handle *, const char *);
    void (*free)(void *);
    mpv_event *(*wait_event)(mpv_handle *, double);
    int (*request_log_messages)(mpv_handle *, const char *);
    const char *(*error_string)(int);
    int (*stream_cb_add_ro)(mpv_handle *, const char *, void *,
                            mpv_stream_cb_open_ro_fn);
} mpv;

static const char *fixtures;
static int failures;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    failures++;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---------------------------------------------------------------- provider

enum block_mode { BLOCK_NONE, BLOCK_SEGMENT_OPEN, BLOCK_SEGMENT_READ };

#define MAX_OPENED 256

struct provider {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    enum block_mode block;
    bool blocked_now;     // a callback is currently blocked
    char opened[MAX_OPENED][128];
    int num_opened;
    atomic_int live;      // open cookies not yet closed
    atomic_llong bytes;   // bytes served through read_fn
    atomic_int cancels;   // cancel_fn invocations
};

struct cookie {
    struct provider *p;
    int fd;
    bool segment;
    atomic_bool cancelled;
};

static bool is_segment(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext && (!strcmp(ext, ".ts") || !strcmp(ext, ".m4s"));
}

// Blocks until the cookie is cancelled or 20 s pass (the watchdog catches a
// real hang; the timeout only keeps a broken cancel path from deadlocking).
static void block_until_cancel(struct provider *p, struct cookie *c)
{
    double deadline = now() + 20;
    pthread_mutex_lock(&p->lock);
    p->blocked_now = true;
    pthread_cond_broadcast(&p->cond);
    while (!(c && atomic_load(&c->cancelled)) && now() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&p->cond, &p->lock, &ts);
    }
    pthread_mutex_unlock(&p->lock);
}

static int64_t cb_read(void *cookie, char *buf, uint64_t nbytes)
{
    struct cookie *c = cookie;
    if (c->segment && c->p->block == BLOCK_SEGMENT_READ) {
        block_until_cancel(c->p, c);
        return -1;
    }
    if (atomic_load(&c->cancelled))
        return -1;
    ssize_t r = read(c->fd, buf, nbytes);
    if (r > 0)
        atomic_fetch_add(&c->p->bytes, r);
    return r < 0 ? -1 : r;
}

static int64_t cb_seek(void *cookie, int64_t offset)
{
    struct cookie *c = cookie;
    off_t r = lseek(c->fd, offset, SEEK_SET);
    return r < 0 ? MPV_ERROR_GENERIC : r;
}

static int64_t cb_size(void *cookie)
{
    struct cookie *c = cookie;
    struct stat st;
    return fstat(c->fd, &st) ? MPV_ERROR_UNSUPPORTED : st.st_size;
}

static void cb_close(void *cookie)
{
    struct cookie *c = cookie;
    atomic_fetch_sub(&c->p->live, 1);
    close(c->fd);
    free(c);
}

static void cb_cancel(void *cookie)
{
    struct cookie *c = cookie;
    atomic_store(&c->cancelled, true);
    atomic_fetch_add(&c->p->cancels, 1);
    pthread_mutex_lock(&c->p->lock);
    pthread_cond_broadcast(&c->p->cond);
    pthread_mutex_unlock(&c->p->lock);
}

static int cb_open(void *user_data, char *uri, mpv_stream_cb_info *info)
{
    struct provider *p = user_data;
    const char *prefix = PROTO "://";
    if (strncmp(uri, prefix, strlen(prefix)))
        return MPV_ERROR_LOADING_FAILED;
    const char *name = uri + strlen(prefix);
    if (strstr(name, ".."))
        return MPV_ERROR_LOADING_FAILED;

    pthread_mutex_lock(&p->lock);
    if (p->num_opened < MAX_OPENED)
        snprintf(p->opened[p->num_opened++], sizeof(p->opened[0]), "%s", name);
    pthread_mutex_unlock(&p->lock);

    if (is_segment(name) && p->block == BLOCK_SEGMENT_OPEN) {
        // Opening is not cancellable through the stream_cb API; emulate a
        // slow network open that finishes after the player was stopped.
        pthread_mutex_lock(&p->lock);
        p->blocked_now = true;
        pthread_cond_broadcast(&p->cond);
        pthread_mutex_unlock(&p->lock);
        usleep(800 * 1000);
    }

    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", fixtures, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return MPV_ERROR_LOADING_FAILED;
    struct cookie *c = calloc(1, sizeof(*c));
    c->p = p;
    c->fd = fd;
    c->segment = is_segment(name);
    atomic_fetch_add(&p->live, 1);
    info->cookie = c;
    info->read_fn = cb_read;
    info->seek_fn = cb_seek;
    info->size_fn = cb_size;
    info->close_fn = cb_close;
    info->cancel_fn = cb_cancel;
    return 0;
}

static void provider_reset(struct provider *p, enum block_mode block)
{
    pthread_mutex_lock(&p->lock);
    p->block = block;
    p->blocked_now = false;
    p->num_opened = 0;
    pthread_mutex_unlock(&p->lock);
    atomic_store(&p->bytes, 0);
    atomic_store(&p->cancels, 0);
}

static int count_opened(struct provider *p, const char *suffix)
{
    int n = 0;
    pthread_mutex_lock(&p->lock);
    for (int i = 0; i < p->num_opened; i++) {
        size_t a = strlen(p->opened[i]), b = strlen(suffix);
        if (a >= b && !strcmp(p->opened[i] + a - b, suffix))
            n++;
    }
    pthread_mutex_unlock(&p->lock);
    return n;
}

static bool was_opened(struct provider *p, const char *name)
{
    bool r = false;
    pthread_mutex_lock(&p->lock);
    for (int i = 0; i < p->num_opened; i++)
        r |= !strcmp(p->opened[i], name);
    pthread_mutex_unlock(&p->lock);
    return r;
}

static bool wait_blocked(struct provider *p, double timeout)
{
    double deadline = now() + timeout;
    bool r;
    pthread_mutex_lock(&p->lock);
    while (!(r = p->blocked_now) && now() < deadline) {
        pthread_mutex_unlock(&p->lock);
        usleep(10 * 1000);
        pthread_mutex_lock(&p->lock);
    }
    pthread_mutex_unlock(&p->lock);
    return r;
}

// ------------------------------------------------------------------ player

static struct provider provider = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static mpv_handle *player_new(const char *lavf_opts, bool lavf_only)
{
    mpv_handle *h = mpv.create();
    if (!h) {
        fail("mpv_create");
        exit(1);
    }
    const char *opts[][2] = {
        {"vo", "null"}, {"ao", "null"}, {"idle", "yes"},
        {"terminal", "no"},
        {"load-unsafe-playlists", "yes"},
        // lavf_only: no fallback to mpv's playlist demuxer, which would list
        // the segment URIs and play them as standalone files.
        {"demuxer", lavf_only ? "lavf" : NULL},
        {"cache", "no"}, {"demuxer-lavf-o", lavf_opts},
    };
    for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        if (!opts[i][1])
            continue;
        int r = mpv.set_option_string(h, opts[i][0], opts[i][1]);
        if (r < 0) {
            fail("option %s=%s: %s", opts[i][0], opts[i][1], mpv.error_string(r));
            exit(1);
        }
    }
    if (mpv.initialize(h) < 0) {
        fail("mpv_initialize");
        exit(1);
    }
    // FFmpeg's AV_LOG_INFO maps to mpv's "v" level.
    mpv.request_log_messages(h, "v");
    int r = mpv.stream_cb_add_ro(h, PROTO, &provider, cb_open);
    if (r < 0) {
        fail("mpv_stream_cb_add_ro: %s", mpv.error_string(r));
        exit(1);
    }
    return h;
}

static void cmd(mpv_handle *h, const char *a, const char *b, const char *c)
{
    const char *args[] = {a, b, c, NULL};
    int r = mpv.command(h, args);
    if (r < 0)
        fail("command %s %s: %s", a, b ? b : "", mpv.error_string(r));
}

static double get_double(mpv_handle *h, const char *name)
{
    char *s = mpv.get_property_string(h, name);
    if (!s)
        return -1;
    double v = atof(s);
    mpv.free(s);
    return v;
}

struct wait_state {
    bool ended;
    int end_error;
};

static bool verbose;
static atomic_int ad_detected, ad_skipped;

static void handle_event(mpv_event *ev, struct wait_state *st)
{
    if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
        mpv_event_log_message *m = ev->data;
        if (strstr(m->text, "Kazumi: Ad detected"))
            atomic_fetch_add(&ad_detected, 1);
        if (strstr(m->text, "Kazumi: Ad skipped"))
            atomic_fetch_add(&ad_skipped, 1);
        if (verbose)
            fprintf(stderr, "[%s] %s", m->prefix, m->text);
    } else if (ev->event_id == MPV_EVENT_END_FILE) {
        mpv_event_end_file *e = ev->data;
        st->ended = true;
        st->end_error = e->reason == MPV_END_FILE_REASON_ERROR ? e->error : 0;
    }
}

// Processes events for up to `wait` seconds (at least one wait_event call).
static void pump(mpv_handle *h, double wait, struct wait_state *st)
{
    mpv_event *ev = mpv.wait_event(h, wait);
    while (ev->event_id != MPV_EVENT_NONE) {
        handle_event(ev, st);
        ev = mpv.wait_event(h, 0);
    }
}

// Waits until time-pos >= target. Returns false on timeout or failed load.
static bool wait_time_pos(mpv_handle *h, double target, double timeout)
{
    struct wait_state st = {0};
    double deadline = now() + timeout;
    while (now() < deadline) {
        pump(h, 0.05, &st);
        if (st.ended && st.end_error)
            return false;
        if (get_double(h, "time-pos") >= target)
            return true;
    }
    return false;
}

static bool wait_end_file(mpv_handle *h, double timeout, int *err)
{
    struct wait_state st = {0};
    double deadline = now() + timeout;
    while (now() < deadline && !st.ended)
        pump(h, 0.05, &st);
    *err = st.end_error;
    return st.ended;
}

// Waits until every callback stream was closed by mpv.
static bool wait_released(double timeout)
{
    double deadline = now() + timeout;
    while (atomic_load(&provider.live) && now() < deadline)
        usleep(10 * 1000);
    return atomic_load(&provider.live) == 0;
}

// ------------------------------------------------------------------- cases

struct format_case {
    const char *name;
    const char *url;
    const char *manifest;     // must be opened through the callback
    const char *nested;       // nested manifest, NULL if none
    const char *seg_suffix;   // segment suffix
    const char *late_segment; // segment that only a seek to SEEK_TARGET needs
};

#define SEEK_TARGET 9.0

static void test_playback(const struct format_case *fc)
{
    printf("== %s: playback + seek through %s callbacks\n", fc->name, PROTO);
    provider_reset(&provider, BLOCK_NONE);
    mpv_handle *h = player_new(LAVF_OPTS_ON, false);
    cmd(h, "loadfile", fc->url, NULL);
    if (!wait_time_pos(h, 1.0, 20))
        fail("%s: playback did not reach 1s", fc->name);
    // Must be FFmpeg's HLS/DASH demuxer, not mpv's playlist fallback playing
    // the segment URIs one by one.
    char *format = mpv.get_property_string(h, "file-format");
    if (!format || strcmp(format, fc->name))
        fail("%s: file-format is %s", fc->name, format ? format : "(none)");
    mpv.free(format);
    if (get_double(h, "playlist-count") != 1)
        fail("%s: manifest was expanded into a playlist", fc->name);
    if (!was_opened(&provider, fc->manifest))
        fail("%s: manifest %s not opened through callback", fc->name, fc->manifest);
    if (fc->nested && !was_opened(&provider, fc->nested))
        fail("%s: nested manifest %s not opened through callback", fc->name,
             fc->nested);
    int segs = count_opened(&provider, fc->seg_suffix);
    if (segs < 1)
        fail("%s: no segment opened through callback", fc->name);
    if (was_opened(&provider, fc->late_segment))
        fail("%s: %s opened before seek", fc->name, fc->late_segment);
    printf("   started: %d segment opens, %lld bytes via callback\n", segs,
           (long long)atomic_load(&provider.bytes));

    char target[32];
    snprintf(target, sizeof(target), "%.1f", SEEK_TARGET);
    cmd(h, "seek", target, "absolute");
    if (!wait_time_pos(h, SEEK_TARGET + 0.5, 20))
        fail("%s: seek to %s did not complete", fc->name, target);
    if (!was_opened(&provider, fc->late_segment))
        fail("%s: seek did not open %s through callback", fc->name,
             fc->late_segment);
    double pos = get_double(h, "time-pos");
    if (pos > SEEK_TARGET + 5)
        fail("%s: time-pos %.2f after seek is wrong", fc->name, pos);
    printf("   seek ok: time-pos %.2f, %d segment opens\n", pos,
           count_opened(&provider, fc->seg_suffix));

    cmd(h, "stop", NULL, NULL);
    int err = 0;
    wait_end_file(h, 10, &err);
    mpv.terminate_destroy(h);
    if (!wait_released(5))
        fail("%s: %d callback streams leaked after destroy", fc->name,
             atomic_load(&provider.live));
}

static void test_gate(const struct format_case *fc)
{
    printf("== %s: segments refused without allow_custom_io\n", fc->name);
    provider_reset(&provider, BLOCK_NONE);
    mpv_handle *h = player_new(LAVF_OPTS_OFF, true);
    cmd(h, "loadfile", fc->url, NULL);
    int err = 0;
    // FFmpeg must refuse the segments, so opening fails.
    if (!wait_end_file(h, 20, &err))
        fail("%s: loading without allow_custom_io did not end", fc->name);
    if (!err)
        fail("%s: loading without allow_custom_io did not fail", fc->name);
    if (count_opened(&provider, fc->seg_suffix))
        fail("%s: segment opened without allow_custom_io", fc->name);
    mpv.terminate_destroy(h);
    if (!wait_released(5))
        fail("%s: callback streams leaked (gate)", fc->name);
}

// Stops (or destroys) the player while a segment open/read is pending.
static void test_cancel(const struct format_case *fc, enum block_mode mode,
                        bool destroy)
{
    printf("== %s: %s while segment %s is pending\n", fc->name,
           destroy ? "terminate_destroy" : "stop",
           mode == BLOCK_SEGMENT_OPEN ? "open" : "read");
    provider_reset(&provider, mode);
    mpv_handle *h = player_new(LAVF_OPTS_ON, false);
    cmd(h, "loadfile", fc->url, NULL);
    if (!wait_blocked(&provider, 20))
        fail("%s: segment callback never reached", fc->name);
    double t0 = now();
    if (destroy) {
        mpv.terminate_destroy(h);
    } else {
        cmd(h, "stop", NULL, NULL);
        int err = 0;
        if (!wait_end_file(h, 15, &err))
            fail("%s: stop during open did not end the file", fc->name);
        // The player must stay usable: play the same source normally.
        provider_reset(&provider, BLOCK_NONE);
        cmd(h, "loadfile", fc->url, NULL);
        if (!wait_time_pos(h, 1.0, 20))
            fail("%s: player unusable after cancelled open", fc->name);
        cmd(h, "stop", NULL, NULL);
        wait_end_file(h, 10, &err);
        mpv.terminate_destroy(h);
    }
    double dt = now() - t0;
    if (mode == BLOCK_SEGMENT_READ && atomic_load(&provider.cancels) < 1 &&
        destroy)
        fail("%s: blocked read was not cancelled", fc->name);
    if (dt > 15)
        fail("%s: cancellation took %.1fs", fc->name, dt);
    if (!wait_released(5))
        fail("%s: %d callback streams leaked after cancel", fc->name,
             atomic_load(&provider.live));
    printf("   cancelled in %.2fs\n", dt);
}

static void test_cycles(const struct format_case *fc)
{
    printf("== %s: repeated load/stop cycles\n", fc->name);
    provider_reset(&provider, BLOCK_NONE);
    mpv_handle *h = player_new(LAVF_OPTS_ON, false);
    for (int i = 0; i < 5; i++) {
        cmd(h, "loadfile", fc->url, NULL);
        // Alternate between stopping mid-open and after playback started.
        if (i % 2)
            wait_time_pos(h, 0.5, 20);
        else
            usleep(30 * 1000 * (i + 1));
        cmd(h, "stop", NULL, NULL);
        int err = 0;
        wait_end_file(h, 15, &err);
    }
    mpv.terminate_destroy(h);
    if (!wait_released(5))
        fail("%s: callback streams leaked after cycles", fc->name);
}

// HLS with a timestamp-restarting ad between DISCONTINUITY tags: the
// PTS-offset ad filter (ffmpeg-hls-kazumi-combined) must drop the ad and
// resume the main timeline; playback has to run through to EOF.
static void test_ad_filter(void)
{
    printf("== hls: hls_ad_filter skips a discontinuity ad\n");
    provider_reset(&provider, BLOCK_NONE);
    atomic_store(&ad_detected, 0);
    atomic_store(&ad_skipped, 0);
    mpv_handle *h = player_new(LAVF_OPTS_ON, false);
    cmd(h, "set", "speed", "8");
    cmd(h, "loadfile", PROTO "://hls/ads.m3u8", NULL);
    double last = -1, max_pos = -1, deadline = now() + 30;
    struct wait_state st = {0};
    while (!st.ended && now() < deadline) {
        pump(h, 0.02, &st);
        double pos = get_double(h, "time-pos");
        if (pos > max_pos)
            max_pos = pos;
        if (pos >= 0)
            last = pos;
    }
    if (!st.ended || st.end_error)
        fail("hls-ad: playback did not reach EOF (%d)", st.end_error);
    if (!was_opened(&provider, "hls/ad_000.ts"))
        fail("hls-ad: ad segment not read");
    if (!atomic_load(&ad_detected) || !atomic_load(&ad_skipped))
        fail("hls-ad: ad not detected/skipped (%d/%d)",
             atomic_load(&ad_detected), atomic_load(&ad_skipped));
    // Main content is 12 s; the 4 s ad must not extend the timeline.
    if (max_pos < 10 || max_pos > 13)
        fail("hls-ad: final position %.2f (expected ~12)", max_pos);
    printf("   ad skipped, played to %.2f (last %.2f)\n", max_pos, last);
    mpv.terminate_destroy(h);
    if (!wait_released(5))
        fail("hls-ad: callback streams leaked");
}

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL: watchdog timeout (hang)\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(3);
}

#define LOAD(field, sym)                                        \
    do {                                                        \
        *(void **)&mpv.field = dlsym(lib, sym);                 \
        if (!mpv.field) {                                       \
            fprintf(stderr, "missing symbol %s\n", sym);        \
            return 2;                                           \
        }                                                       \
    } while (0)

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s libmpv.so fixtures-dir [hls|dash]...\n",
                argv[0]);
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    LOAD(create, "mpv_create");
    LOAD(initialize, "mpv_initialize");
    LOAD(terminate_destroy, "mpv_terminate_destroy");
    LOAD(set_option_string, "mpv_set_option_string");
    LOAD(command, "mpv_command");
    LOAD(get_property_string, "mpv_get_property_string");
    LOAD(free, "mpv_free");
    LOAD(wait_event, "mpv_wait_event");
    LOAD(request_log_messages, "mpv_request_log_messages");
    LOAD(error_string, "mpv_error_string");
    LOAD(stream_cb_add_ro, "mpv_stream_cb_add_ro");
    fixtures = argv[2];
    setvbuf(stdout, NULL, _IOLBF, 0);
    verbose = getenv("MEDIAIO_TEST_VERBOSE") != NULL;

    signal(SIGALRM, on_alarm);
    alarm(600);

    static const struct format_case cases[] = {
        {"hls", PROTO "://hls/master.m3u8", "hls/master.m3u8",
         "hls/index.m3u8", ".ts", "hls/seg_004.ts"},
        {"dash", PROTO "://dash/manifest.mpd", "dash/manifest.mpd", NULL,
         ".m4s", "dash/chunk-stream0-00005.m4s"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct format_case *fc = &cases[i];
        bool selected = argc == 3;
        for (int a = 3; a < argc; a++)
            selected |= !strcmp(argv[a], fc->name);
        if (!selected)
            continue;
        test_playback(fc);
        test_gate(fc);
        test_cancel(fc, BLOCK_SEGMENT_OPEN, false);
        test_cancel(fc, BLOCK_SEGMENT_OPEN, true);
        test_cancel(fc, BLOCK_SEGMENT_READ, false);
        test_cancel(fc, BLOCK_SEGMENT_READ, true);
        test_cycles(fc);
        if (!strcmp(fc->name, "hls"))
            test_ad_filter();
    }

    if (failures) {
        fprintf(stderr, "mediaio nested I/O: %d failure(s)\n", failures);
        return 1;
    }
    printf("mediaio nested HLS/DASH callback I/O: PASS\n");
    return 0;
}

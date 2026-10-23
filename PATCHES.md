# PaoPao kernel patches (Linux)

Baseline: Predidit/libmpv-linux-build `ca3a20e` (no newer upstream commits as
of 2026-10-22), FFmpeg `n7.1.5`, mpv `f011d73` (~0.40), libplacebo `4c426e4`,
dav1d 1.5.4. Patches live in `patch/ffmpeg` and `patch/mpv` and are applied
in file-name order with `git apply` by `.github/workflows/build.yaml`.
File names and semantics follow the shared four-platform spec (Android,
Darwin, Linux, Windows).

## FFmpeg (`patch/ffmpeg`)

| Patch | What | Source | Verified by |
| --- | --- | --- | --- |
| `ffmpeg-hls-kazumi-combined.patch` | PTS-offset ad filter (`hls_ad_filter`; `pts_offset`, `ad_start_*`/`ad_resume_*`, both tracks must resume, window -2 s/+10 s, reset on seek), `seg_allow_img` PNG/GIF disguise probing, `hls_probe` returns `AVPROBE_SCORE_MAX/2` for non-standard extensions | Ported from Android `libmpv-android-video-build` (FFmpeg 7.1.3); replaces the earlier drop-only version | `tests/mediaio_nested.c` ad case (`Kazumi: Ad skipped`, timeline stays 12 s) |
| `ffmpeg-segmented-custom-io.patch` | HLS + DASH `allow_custom_io` (default 0). Unknown protocols with a non-empty `scheme:` are opened through `AVFormatContext.io_open` when enabled; DASH `proto_name` NULL fix for `proto_name_len`/`is_http` | Android version, reduced for 7.1.5: DASH `open_url`/`parse_manifest` already use `io_open`/`ff_format_io_close` in 7.1.5 | `tests/mediaio_nested.c` (segments via callback; refused without the option) |
| `ffmpeg-segmented-io-cancel.patch` | `open_url` in hls.c/dashdec.c returns `AVERROR_EXIT` when the interrupt callback fires | Darwin `libmpv-darwin-build` | `tests/mediaio_nested.c` stop/destroy during pending segment open/read |

## mpv (`patch/mpv`)

| Patch | What | Source |
| --- | --- | --- |
| `mpv-fix-gles-sampler-precision.patch` | upstream (Predidit) | upstream |
| `mpv-fix-libmpv-fd-leak.patch` | upstream (Predidit) | upstream |
| `mpv-nested-stream-callback.patch` | Union of the Android and Darwin versions: nested FFmpeg opens of a protocol that is both registered with `mpv_stream_cb_add_ro` and listed in `protocol_whitelist` go through the mpv stream callback (`nested_stream_read`/`nested_stream_seek`); cancel checked before/after `stream_create`; top-level manifest opens set `AVFMT_FLAG_CUSTOM_IO`; `io_close2` ignores `priv->pb` and keeps closed callback entries as tombstones so repeated closes never reach `avio_close`; `demux_close_lavf` marks a leaked callback `pb` as custom IO, detaches callback-stream cancels (`stream_cb_detach_cancel`, avoids the `cancel_destroy` `!c->slaves.head` assertion) and frees all callback entries | Android `nested-stream-callback.patch` (mpv 0.41) + Darwin `mpv-nested-stream-callback.patch` (mpv 0.36) |

Not added:

- `mpv-filter-metadata-key-type.patch`: mpv `f011d73` already contains the
  equivalent fix (`remaining || ka->action != M_PROPERTY_GET_TYPE` in
  `mp_property_filter_metadata`).
- `mpv_restore_tls_verify_default`: mpv `f011d73` still defaults
  `tls-verify` to no; only needed for mpv master.

## FFmpeg configure capabilities

Added on top of the upstream allowlist (`build.yaml`):

- filters `overlay equalizer astats aresample aformat anull asplit pan bandpass amerge`
- muxers `mp4 mov`, bsf `aac_adtstoasc` (original-audio cache export)
- `--enable-libxml2`: the upstream build never had the DASH demuxer because
  libxml2 is not auto-detected. libxml2 v2.14.6 is built static/PIC
  (`external/scripts/libxml2-*`) and hidden with `--exclude-libs`, so
  `libmpv.so.2` does not depend on the host libxml2 soname.

## Tests

Run after every CI build on the packaged `libmpv.so.2` (x86_64 and aarch64):

- `tests/check_library.py`: filters/demuxers/muxers/bsf present, patch
  strings (`allow_custom_io`, `seg_allow_img`, `hls_ad_filter`, ...) present,
  libxml2 not in `NEEDED`.
- `tests/audio_filters.py`: 32-band `asplit/bandpass/amerge/astats` graph,
  reads `af-metadata/ppviz/lavfi.astats.N.RMS_level` (see AUDIO_FILTERS.md).
- `tests/mediaio_nested.c` + `tests/mediaio_fixtures.sh`: registers
  `mediaio` with `mpv_stream_cb_add_ro`, uses media-kit's
  `demuxer-lavf-o` (`allow_custom_io=1,protocol_whitelist=[mediaio,file,crypto,data]`)
  and checks HLS (master -> variant -> segments) and DASH all read through
  the callback, `file-format` is hls/dash (no playlist fallback), absolute
  seek opens later segments through the callback, the opt-in gate, stop and
  `mpv_terminate_destroy` while a segment open or read is pending, repeated
  load/stop cycles release every callback stream, and the ad filter. Also
  passes against an ASan build of mpv.

Local run:

```sh
sh tests/mediaio_fixtures.sh /tmp/mediaio
cc -O1 -g -Iinclude tests/mediaio_nested.c -o /tmp/mediaio_nested -ldl -lpthread
/tmp/mediaio_nested path/to/libmpv.so.2 /tmp/mediaio   # optional: hls|dash
```

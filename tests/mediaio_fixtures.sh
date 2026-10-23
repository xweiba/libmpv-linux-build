#!/bin/sh
# Generates the HLS and DASH fixtures for tests/mediaio_nested.c.
#   hls/master.m3u8  -> relative "index.m3u8"
#   hls/index.m3u8   -> absolute mediaio://hls/seg_NNN.ts segment URIs
#   dash/manifest.mpd with relative SegmentTemplate media (resolved against
#                     the mediaio:// manifest URL)
# 12 s of H.264 + AAC, 1 s GOP, 2 s segments.
set -eu
out=${1:?usage: mediaio_fixtures.sh OUTPUT_DIR}
ffmpeg=${FFMPEG:-ffmpeg}
rm -rf "$out"
mkdir -p "$out/hls" "$out/dash"
src="-f lavfi -i testsrc=size=320x240:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000"
enc="-t 12 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 25 -keyint_min 25 -sc_threshold 0 -c:a aac -b:a 64k"
# shellcheck disable=SC2086
"$ffmpeg" -hide_banner -loglevel error -y $src $enc -f hls -hls_time 2 \
    -hls_list_size 0 -hls_playlist_type vod \
    -hls_segment_filename "$out/hls/seg_%03d.ts" \
    -hls_base_url "mediaio://hls/" "$out/hls/index.m3u8"
cat > "$out/hls/master.m3u8" <<'M3U'
#EXTM3U
#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=320x240,CODECS="avc1.42c00d,mp4a.40.2"
index.m3u8
M3U
# Ad-insertion variant: a 4 s "ad" with restarted timestamps between
# DISCONTINUITY tags after the 3rd main segment (exercises hls_ad_filter).
ad="-f lavfi -i smptebars=size=320x240:rate=25 -f lavfi -i sine=frequency=880:sample_rate=48000"
# shellcheck disable=SC2086
"$ffmpeg" -hide_banner -loglevel error -y $ad $(echo "$enc" | sed 's/-t 12/-t 4/') \
    -f hls -hls_time 2 -hls_list_size 0 -hls_playlist_type vod \
    -hls_segment_filename "$out/hls/ad_%03d.ts" \
    -hls_base_url "mediaio://hls/" "$out/hls/ad.m3u8"
{
    sed -n '1,/seg_002.ts/p' "$out/hls/index.m3u8"
    echo "#EXT-X-DISCONTINUITY"
    grep -B1 '^mediaio://' "$out/hls/ad.m3u8" | grep -v '^--'
    echo "#EXT-X-DISCONTINUITY"
    sed -n '/seg_002.ts/,$p' "$out/hls/index.m3u8" | sed 1d
} > "$out/hls/ads.m3u8"
# shellcheck disable=SC2086
"$ffmpeg" -hide_banner -loglevel error -y $src $enc -f dash -seg_duration 2 \
    -use_template 1 -use_timeline 0 "$out/dash/manifest.mpd"
ls "$out/hls" "$out/dash"

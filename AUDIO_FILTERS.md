# Audio analysis release

Upstream: Predidit/libmpv-linux-build, baseline
ca3a20edc729a22d57ed3ff5cb557e16205744f9. The 20260810 release disables
FFmpeg filters except an explicit allowlist. It lacks astats, asplit,
bandpass and amerge, so the music analysis graph cannot initialize.

Release 20261007 added those filters. Release 20261023 makes the filter set
explicit in the workflow (overlay, equalizer, astats, aresample, aformat,
anull, asplit, pan, bandpass, amerge) and adds the mp4/mov muxers and the
aac_adtstoasc bsf for original-audio cache export. It also carries the
unified kernel patches described in PATCHES.md. Decoding and hardware
acceleration options are unchanged.

Both x86_64 and aarch64 jobs exercise the actual packaged shared library
before publishing (`tests/audio_filters.py`, `tests/check_library.py`,
`tests/mediaio_nested.c`). The audio regression creates a stereo 440 Hz
tone, reads all 32 real RMS bands and checks the peak band. Checking a
system mpv or a filter-chain label alone is not sufficient.

Release 20261023 assets (sha256):

| Asset | sha256 |
| --- | --- |
| libmpv_x86_64.zip | 7b36d9f731eb50fd6fe37e4b65d7e9fb487ecb75c96245208f114594667c3150 |
| libmpv_aarch64.zip | 27608bf5ce88626f74518c31075cd1bd9cc573bd92fb3d6aeb2006a0931f2f1e |
| libmpv_x86_64_header.zip | 219addddeff09072995303b3a6c14081f052a07919d660ac1259b6b85df806d8 |
| libmpv_aarch64_header.zip | b92e49acd6070e44d6c948e9c2c27ccf6dac28c30579b6281ad8ddae300779a9 |

Exit condition: upstream ships all required filters and passes this packaged
library regression. Consumers pin the checked release archives and SHA256.

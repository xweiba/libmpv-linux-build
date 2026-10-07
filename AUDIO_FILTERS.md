# Audio analysis release

Upstream: Predidit/libmpv-linux-build, baseline
ca3a20edc729a22d57ed3ff5cb557e16205744f9. The 20260810 release disables
FFmpeg filters except an explicit allowlist. It lacks astats, asplit,
bandpass and amerge, so the music analysis graph cannot initialize.

Release 20261007 adds those filters without changing decoding or
hardware acceleration options. Both x86_64 and aarch64 jobs exercise the
actual packaged shared library before publishing. The regression creates
a stereo 440 Hz tone, reads all 32 real RMS bands and checks the peak band.
Checking a system mpv or a filter-chain label alone is not sufficient.

Exit condition: upstream ships all required filters and passes this packaged
library regression. Consumers pin the checked release archives and SHA256.

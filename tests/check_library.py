"""Static capability checks for the packaged libmpv.so.2.

Verifies the PaoPao kernel patch set and FFmpeg configure capabilities are
present in the release library (see PATCHES.md), and that the statically
linked libxml2 does not leak into the dynamic dependencies.
"""

import ctypes as c
from pathlib import Path
import subprocess
import sys

FILTERS = ("overlay", "equalizer", "astats", "aresample", "aformat", "anull",
           "asplit", "pan", "bandpass", "amerge")
DEMUXERS = ("hls", "dash", "mpegts", "mov")
MUXERS = ("mp4", "mov")
BSFS = ("aac_adtstoasc",)
# AVOption names / log strings introduced by the patches.
STRINGS = (b"allow_custom_io", b"seg_allow_img", b"hls_ad_filter",
           b"Kazumi: Ad skipped! Offset adjusted", b"aac_adtstoasc", b"astats")


def verify(library):
    path = Path(library).resolve()
    lib = c.CDLL(str(path))
    for fn, restype in (("avfilter_get_by_name", c.c_void_p),
                        ("av_find_input_format", c.c_void_p),
                        ("av_guess_format", c.c_void_p),
                        ("av_bsf_get_by_name", c.c_void_p)):
        getattr(lib, fn).restype = restype
    lib.av_guess_format.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p]
    missing = []
    missing += [f"filter {n}" for n in FILTERS
                if not lib.avfilter_get_by_name(n.encode())]
    missing += [f"demuxer {n}" for n in DEMUXERS
                if not lib.av_find_input_format(n.encode())]
    missing += [f"muxer {n}" for n in MUXERS
                if not lib.av_guess_format(n.encode(), None, None)]
    missing += [f"bsf {n}" for n in BSFS
                if not lib.av_bsf_get_by_name(n.encode())]
    data = path.read_bytes()
    missing += [f"string {s.decode()}" for s in STRINGS if s not in data]
    for symbol in ("mpv_stream_cb_add_ro", "mpv_create"):
        if not hasattr(lib, symbol):
            missing.append(f"symbol {symbol}")
    needed = subprocess.run(["readelf", "-d", str(path)], check=True,
                            capture_output=True, text=True).stdout
    if "libxml2" in needed:
        missing.append("libxml2 must be linked statically (found in NEEDED)")
    assert not missing, "Missing: " + ", ".join(missing)
    print("Packaged library: patch strings and FFmpeg capabilities PASS")


if __name__ == "__main__":
    verify(sys.argv[1])

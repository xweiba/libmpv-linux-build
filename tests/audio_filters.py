"""Exercise required filters and real spectrum metadata in the release library."""

import ctypes as c
import math
from pathlib import Path
import struct
import sys
import tempfile
import time
import wave


def verify(library):
    mpv = c.CDLL(str(Path(library).resolve()))
    mpv.avfilter_get_by_name.argtypes = [c.c_char_p]
    mpv.avfilter_get_by_name.restype = c.c_void_p
    for name in ("astats", "asplit", "bandpass", "amerge", "aformat", "pan"):
        assert mpv.avfilter_get_by_name(name.encode()), f"Missing filter: {name}"
    mpv.mpv_create.restype = c.c_void_p
    mpv.mpv_initialize.argtypes = [c.c_void_p]
    mpv.mpv_set_option_string.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p]
    mpv.mpv_command.argtypes = [c.c_void_p, c.POINTER(c.c_char_p)]
    mpv.mpv_get_property_string.argtypes = [c.c_void_p, c.c_char_p]
    mpv.mpv_get_property_string.restype = c.c_void_p
    mpv.mpv_free.argtypes = [c.c_void_p]
    mpv.mpv_terminate_destroy.argtypes = [c.c_void_p]
    player = mpv.mpv_create()

    def command(*args):
        values = (c.c_char_p * (len(args) + 1))(
            *[a.encode() for a in args], None
        )
        assert mpv.mpv_command(player, values) >= 0, args[0]

    def number(property_name):
        pointer = mpv.mpv_get_property_string(player, property_name.encode())
        if not pointer:
            return None
        try:
            return float(c.string_at(pointer))
        finally:
            mpv.mpv_free(pointer)

    try:
        for key, value in (("ao", "null"), ("vo", "null")):
            assert mpv.mpv_set_option_string(player, key.encode(), value.encode()) >= 0
        assert mpv.mpv_initialize(player) >= 0
        count = 32
        step = math.log2(14000 / 50) / (count - 1)
        graph = ";".join([
            "aformat=channel_layouts=stereo,asplit=2[a][b]",
            "[b]pan=mono|c0=0.5*c0+0.5*c1,asplit=32"
            + "".join(f"[b{i}]" for i in range(count)),
            *[
                f"[b{i}]bandpass=f={round(50 * 2 ** (step * i))}"
                f":width_type=o:w={step * 1.2:.3f}[c{i}]"
                for i in range(count)
            ],
            "[a]" + "".join(f"[c{i}]" for i in range(count))
            + "amerge=inputs=33,astats=metadata=1:reset=1"
            ":measure_perchannel=RMS_level:measure_overall=none,pan=stereo|c0=c0|c1=c1",
        ])
        command("af", "add", f"@ppviz:lavfi=graph=%{len(graph.encode())}%{graph}")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tone.wav"
            with wave.open(str(path), "wb") as output:
                output.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
                output.writeframes(b"".join(
                    struct.pack("<hh", *([int(12000 * math.sin(i * 2 * math.pi * 440 / 48000))] * 2))
                    for i in range(48000 * 4)
                ))
            command("loadfile", str(path))
            deadline = time.monotonic() + 3
            bands = None
            while time.monotonic() < deadline:
                bands = [number(f"af-metadata/ppviz/lavfi.astats.{i + 3}.RMS_level")
                         for i in range(count)]
                if all(value is not None for value in bands):
                    break
                time.sleep(.05)
            assert all(value is not None for value in bands), "No spectrum metadata"
            assert max(range(count), key=lambda i: bands[i]) == 12, "440 Hz peak is wrong"
            assert number("af-metadata/ppviz/lavfi.astats.1.RMS_level") is not None
            command("af", "remove", "@ppviz")
        print("Packaged library: required filters and 32-band real audio analysis PASS")
    finally:
        mpv.mpv_terminate_destroy(player)


if __name__ == "__main__":
    verify(sys.argv[1])

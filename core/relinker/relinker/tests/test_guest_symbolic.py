from pathlib import Path
import ctypes
import platform
import struct
import subprocess
import sys
import tempfile

from guest_import_fixtures import dynamic
from test_guest_intel_trampolines import main_fixture
from test_guest_module_directories import dynamic_flags


def module(tags, value=42):
    image = bytearray(0x3000)
    image[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     3, 62, 1, 0x1000, 64, 0, 0, 64, 56, 3, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", image, 64,
                     1, 5, 0x1000, 0x1000, 0x1000, 0x100, 0x100, 0x1000)
    struct.pack_into("<IIQQQQQQ", image, 120,
                     1, 6, 0x2000, 0x2000, 0x2000, 0x1000, 0x1000, 0x1000)
    struct.pack_into("<IIQQQQQQ", image, 176,
                     2, 6, 0x2000, 0x2000, 0x2000, 0, 0, 8)
    image[0x1000:0x1100] = b"\x90" * 0x100
    image[0x1000:0x1006] = b"\xb8" + struct.pack("<I", value) + b"\xc3"
    image[0x1010:0x1016] = b"\xff\x25" + struct.pack("<i", 0x2320 - 0x1016)
    strings = b"\0shared#A#B\0probe#A#B\0"
    image[0x2200:0x2200 + len(strings)] = strings
    struct.pack_into("<IIIIII", image, 0x2240, 1, 3, 1, 0, 2, 0)
    struct.pack_into("<IBBHQQ", image, 0x2298, 1, 0x12, 0, 1, 0x1000, 6)
    struct.pack_into("<IBBHQQ", image, 0x22B0, 12, 0x12, 0, 1, 0x1010, 6)
    struct.pack_into("<QQq", image, 0x2300, 0x2320, (1 << 32) | 6, 0)
    dynamic(image, 0x2000, 176, [(5, 0x2200), (10, len(strings)), (6, 0x2280),
                                (11, 24), (4, 0x2240), (7, 0x2300), (8, 24), (9, 24),
                                *tags])
    return image


def convert(relinker, case, windows, tags, value=42):
    modules = case / "sce_module"
    modules.mkdir(parents=True)
    name = case.name + ".prx"
    (modules / name).write_bytes(module(tags, value))
    source = case / "input.elf"
    source.write_bytes(main_fixture())
    output = case / ("output.exe" if windows else "output.elf")
    result = subprocess.run([str(relinker), *(["--windows"] if windows else []),
                             str(source), str(output)],
                            capture_output=True, text=True, timeout=20)
    return result, output, case / "app0" / "sce_module" / (name + ".guest.prx")


def main():
    relinker = Path(sys.argv[1]).resolve()
    linux = sys.platform == "linux" and platform.machine().lower() in ("x86_64", "amd64")
    cases = (
        ("absent", [], False),
        ("empty-flags", [(30, 0)], False),
        ("bind-now", [(30, 8)], False),
        ("flag", [(30, 2)], True),
        ("flag-bind-now", [(30, 10)], True),
        ("tag", [(16, 0)], True),
        ("tag-ignored-value", [(16, 0xFFFFFFFFFFFFFFFF)], True),
        ("tag-empty-flags", [(16, 0), (30, 0)], True),
        ("tag-bind-now", [(30, 8), (16, 0)], True),
        ("both", [(16, 0), (30, 2)], True),
        ("both-reversed", [(30, 10), (16, 0)], True),
    )
    with tempfile.TemporaryDirectory(prefix="anyps5-guest-symbolic-") as directory:
        work = Path(directory)
        if linux:
            result, _, global_module = convert(relinker, work / "global", False, [], value=7)
            assert result.returncode == 0, (result.stdout, result.stderr)
        for windows in (False, True):
            for label, tags, symbolic in cases:
                result, output, artifact = convert(relinker, work / f"{windows}-{label}", windows, tags)
                if windows and symbolic:
                    assert result.returncode == 2, (label, result.stdout, result.stderr)
                    assert "DF_SYMBOLIC guest modules are not supported by the Windows writer" in result.stderr, result.stderr
                    assert not output.exists() and not artifact.exists()
                    continue
                assert result.returncode == 0, (label, result.stdout, result.stderr)
                assert output.is_file() and artifact.is_file()
                if not windows:
                    assert dynamic_flags(artifact.read_bytes()) == (10 if symbolic else 8), label
                    if linux:
                        run = subprocess.run([sys.executable, __file__, "--load", str(global_module),
                                              str(artifact), str(42 if symbolic else 7)],
                                             capture_output=True, text=True, timeout=10)
                        assert run.returncode == 0, (label, run.stdout, run.stderr)
            for flags in (1, 4, 16):
                result, output, artifact = convert(relinker, work / f"{windows}-invalid-{flags}",
                                                   windows, [(16, 0), (30, flags)])
                assert result.returncode == 2, (result.stdout, result.stderr)
                assert "Unsupported guest dynamic flags" in result.stderr, result.stderr
                assert not output.exists() and not artifact.exists()
    print("Guest symbolic module tests passed" + (" with native Linux symbol lookup" if linux else ""))


if __name__ == "__main__":
    if sys.argv[1] == "--load":
        global_module = ctypes.CDLL(sys.argv[2], mode=ctypes.RTLD_GLOBAL)
        assert getattr(global_module, "shared#guest")() == 7
        local_module = ctypes.CDLL(sys.argv[3])
        assert getattr(local_module, "shared#guest")() == 42
        actual = getattr(local_module, "probe#guest")()
        assert actual == int(sys.argv[4]), actual
    else:
        main()

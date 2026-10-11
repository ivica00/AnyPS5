from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from test_guest_module_directories import module_with_symbol
from test_guest_intel_trampolines import main_fixture


def main():
    relinker = Path(sys.argv[1]).resolve()
    harness = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-deferred-module-") as directory:
        work = Path(directory)
        plugins = work / "Engine" / "Plugins"
        plugins.mkdir(parents=True)
        module = module_with_symbol(True)
        module[0x400:0x406] = bytes.fromhex("b8 2a 00 00 00 c3")
        module[0x430:0x435] = bytes.fromhex("89 f8 33 06 c3")
        module[0x440:0x445] = bytes.fromhex("89 f8 33 06 c3")
        struct.pack_into("<qQqQqQ", module, 0x680, 12, 0x1030, 13, 0x1040, 0, 0)
        struct.pack_into("<QQ", module, 176 + 32, 11 * 16, 11 * 16)
        (plugins / "sample.prx").write_bytes(module)
        source = work / "input.elf"
        source.write_bytes(main_fixture())
        output = work / "output.exe"
        converted = subprocess.run([str(relinker), "--windows", "--guest-module-directory",
                                    "Engine/Plugins", str(source), str(output)],
                                   capture_output=True, text=True, timeout=15)
        assert converted.returncode == 0, (converted.stdout, converted.stderr)
        startup = subprocess.run([str(output)], capture_output=True, text=True, timeout=15)
        assert startup.returncode == 42, (startup.returncode, startup.stdout, startup.stderr)
        artifact = work / "app0" / "Engine" / "Plugins" / "sample.prx.guest.prx"
        run = subprocess.run([str(harness), str(artifact)], capture_output=True, text=True, timeout=15)
        assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
    print("Deferred module arguments, repeated load and finalization passed")


if __name__ == "__main__":
    main()

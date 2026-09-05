"""Run every check that does not need real hardware.

  1. build_rom.py's encoder against emulate_rom.py's decoder, byte for byte
  2. the gateway firmware's decoder and link layer (compiled for the PC)
  3. the assembled ROM executed on a 6502 core, driving the real firmware

Needs python and g++ on PATH. Build the ROM first:

  python scripts/build_rom.py
  python tests/run_tests.py
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "tests", "host")
BUILD = os.path.join(ROOT, "build")
ROM = os.path.join(BUILD, "nes_web_server.nes")

sys.path.insert(0, os.path.join(ROOT, "scripts"))
import build_rom
import emulate_rom


def find_packets(rom_data):
    offsets, start = [], 0
    while True:
        pos = rom_data.find(emulate_rom.MAGIC, start)
        if pos == -1:
            return offsets
        offsets.append(pos)
        start = pos + 4


def test_roundtrip():
    """Every packet in the ROM must decode back to exactly what went in."""
    print("encoder -> decoder round trip")
    rom_data = open(ROM, "rb").read()
    offsets = find_packets(rom_data)

    expected = build_rom.load_site()

    if len(offsets) != len(expected):
        print(f"  found {len(offsets)} packets, expected {len(expected)}   FAIL")
        return False

    ok = True
    for (name, want), off in zip(expected, offsets):
        got, consumed = emulate_rom.decompress_packet(rom_data[off:])
        declared = int.from_bytes(rom_data[off - 2:off], "little")
        good = got == want and declared == consumed
        ok &= good
        print(f"  {name:12} {len(got):5} bytes, packet {declared:5} "
              f"{'ok' if good else 'FAIL'}")
    return ok


def run_cpp(name):
    """Compile one host test against the real .ino and run it."""
    exe = os.path.join(BUILD, name + (".exe" if os.name == "nt" else ""))
    compile_cmd = [
        "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-I", HOST, "-o", exe, os.path.join(HOST, name + ".cpp"),
    ]
    print(f"\ncompiling {name}")
    sys.stdout.flush()
    result = subprocess.run(compile_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout + result.stderr)
        return False
    sys.stdout.flush()
    return subprocess.run([exe, ROM]).returncode == 0


def main():
    if not os.path.exists(ROM):
        print("build/nes_web_server.nes missing - run: python scripts/build_rom.py")
        return 1
    if not shutil.which("g++"):
        print("g++ not found on PATH - skipping the firmware tests")
        return 0 if test_roundtrip() else 1

    ok = test_roundtrip()
    ok &= run_cpp("test_gateway")
    ok &= run_cpp("test_rom_link")

    print("\n" + ("=" * 40))
    print("ALL TESTS PASSED" if ok else "TESTS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

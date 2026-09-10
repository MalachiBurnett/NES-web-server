"""Run every check that does not need real hardware.

  1. build_rom.py's encoder against emulate_rom.py's decoder, byte for byte
  2. both gateways' firmware, compiled for the PC - the ESP32 original
     and the Arduino Mega port - through the same link layer tests
  3. the assembled ROM executed on a 6502 core, driving each firmware
  4. serial_bridge.py decompressing what the Mega gateway really sends
  5. the Mega firmware compiled for real, run cycle for cycle on an
     ATmega2560 simulator against the ROM on a cycle-counted 6502

Needs python and g++ on PATH, pyserial for 4, and arduino-cli with the
arduino:avr core for 5. Build the ROM first:

  python scripts/build_rom.py
  python tests/run_tests.py
"""
import contextlib
import io
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "tests", "host")
BUILD = os.path.join(ROOT, "build")
ROM = os.path.join(BUILD, "nes_web_server.nes")
MEGA_HTTP = os.path.join(BUILD, "mega_http")
MEGA_FIRMWARE = os.path.join(BUILD, "mega_fw")

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


def result(good, what):
    print(f"  {what:60} {'ok' if good else 'FAIL'}")
    return good


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


def run_cpp(name, *args):
    """Compile one host test against the real .ino and run it."""
    exe = os.path.join(BUILD, name + (".exe" if os.name == "nt" else ""))
    compile_cmd = [
        "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-I", HOST, "-o", exe, os.path.join(HOST, name + ".cpp"),
    ]
    print(f"\ncompiling {name}")
    sys.stdout.flush()
    compiled = subprocess.run(compile_cmd, capture_output=True, text=True)
    if compiled.returncode != 0:
        print(compiled.stdout + compiled.stderr)
        return False
    sys.stdout.flush()
    return subprocess.run([exe, ROM, *args]).returncode == 0


class FakeSerial:
    """Stands in for the serial port, replaying what a gateway wrote."""

    def __init__(self, data):
        self._data = io.BytesIO(data)
        self.timeout = None

    def reset_input_buffer(self):
        pass

    def write(self, data):
        return len(data)

    def flush(self):
        pass

    def readline(self):
        return self._data.readline()

    def read(self, n):
        return self._data.read(n)


def test_bridge():
    """serial_bridge.py against the Arduino Mega gateway's real output.

    test_mega_link saves every response the firmware wrote, byte for byte.
    Each goes through the bridge's own fetch() here, which must turn the
    raw packet back into exactly the page that went into the ROM."""
    print("\nserial_bridge.py against the Mega gateway's output")
    try:
        import serial_bridge
    except ImportError as e:
        print(f"  cannot import serial_bridge ({e}) - skipping: pip install pyserial")
        return True

    def replay(data, path):
        serial_bridge.ser = FakeSerial(data)
        with contextlib.redirect_stdout(io.StringIO()):   # the firmware's log lines
            return serial_bridge.fetch(path)

    def saved(name):
        return open(os.path.join(MEGA_HTTP, name), "rb").read()

    pages = [text.encode("ascii") for _, text in build_rom.load_site()]
    ok = True
    cases = [
        ("index.http", "/", 200, "text/html", pages[0]),
        ("index_bare.http", "/index.html", 200, "text/html", pages[0]),
        ("style.http", "/style.css?v=2", 200, "text/css", pages[1]),
        ("missing.http", "/missing", 404, "text/html", pages[2]),
    ]
    for name, path, want_status, want_type, want_body in cases:
        status, _, headers, body = replay(saved(name), path)
        ok &= result(status == want_status and headers.get("Content-Type") == want_type
                     and body == want_body and "X-NES-Packet" not in headers
                     and int(headers.get("Content-Length")) == len(saved(name)) - saved(name).index(b"\r\n\r\n") - 4,
                     f"{path} decompressed to the page in the ROM")

    status, _, headers, body = replay(saved("link.http"), "/_link")
    ok &= result(status == 200 and body.startswith(b"NES link probe"), "/_link passes through untouched")

    page = b"<html>already decompressed</html>"
    esp32 = (b"# page 0: an ESP32 log line\n"
             b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n" % len(page) + page)
    status, _, headers, body = replay(esp32, "/")
    ok &= result(status == 200 and body == page, "an ESP32 response passes through untouched")

    def refused(data):
        try:
            replay(data, "/")
        except ValueError:
            return True
        return False

    raw = saved("index.http")
    ok &= result(refused(raw[:-200]), "a packet cut short is refused, not served")
    ok &= result(refused(raw.replace(b"NHF2", b"NHF9")), "a packet with a bad magic is refused")

    serial_bridge.ser = FakeSerial(b"# Arduino Mega gateway online. Send a path\r\nHTTP/1.1")
    with contextlib.redirect_stdout(io.StringIO()):
        announced = serial_bridge.wait_for_gateway(timeout=1.0)
    ok &= result(announced and serial_bridge.ser.read(9) == b"HTTP/1.1",
                 "start-up waits for 'online', and no further")
    return ok


def find_arduino_cli():
    found = shutil.which("arduino-cli")
    if found:
        return found
    installed = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "arduino-cli", "arduino-cli.exe")
    return installed if os.path.exists(installed) else None


def build_mega_firmware():
    """Compile NES_router_mega exactly as it would be flashed, for the
    cycle-accurate simulation. Returns the .elf, None when arduino-cli or
    the AVR core is not installed, or False when the build fails."""
    cli = find_arduino_cli()
    if not cli:
        print("\narduino-cli not found - skipping the cycle-accurate Mega simulation")
        return None
    cores = subprocess.run([cli, "core", "list"], capture_output=True, text=True).stdout
    if "arduino:avr" not in cores:
        print("\nno arduino:avr core - skipping the cycle-accurate Mega simulation"
              " (arduino-cli core install arduino:avr)")
        return None
    print("\ncompiling NES_router_mega for the ATmega2560")
    sys.stdout.flush()
    built = subprocess.run(
        [cli, "compile", "--fqbn", "arduino:avr:mega:cpu=atmega2560", "--build-path", MEGA_FIRMWARE,
         os.path.join(ROOT, "src", "firmware", "NES_router_mega")],
        capture_output=True, text=True)
    if built.returncode != 0:
        print(built.stdout + built.stderr)
        return False
    return os.path.join(MEGA_FIRMWARE, "NES_router_mega.ino.elf")


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
    ok &= run_cpp("test_mega_gateway")

    os.makedirs(MEGA_HTTP, exist_ok=True)
    mega_link = run_cpp("test_mega_link", MEGA_HTTP)
    ok &= mega_link
    if mega_link:
        ok &= test_bridge()
    else:
        print("\nskipping the serial_bridge.py test: test_mega_link failed")

    elf = build_mega_firmware()
    if elf is False:
        ok = False
    elif elf:
        ok &= run_cpp("test_mega_avr", elf)

    print("\n" + ("=" * 40))
    print("ALL TESTS PASSED" if ok else "TESTS FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

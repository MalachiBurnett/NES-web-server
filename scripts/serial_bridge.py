"""Bridge real HTTP requests to the NES gateway over USB serial.

Point a Cloudflare Tunnel (or any reverse proxy) at this server's port.
Each request is forwarded to the gateway as a single line (its own
line-based protocol, see loop()/serve() in the firmware), and the HTTP
response the gateway writes back is relayed to the client.

Two gateways speak that protocol:

- NES_router (ESP32-C3) decompresses pages itself, so its responses are
  relayed unchanged.
- NES_router_mega (Arduino Mega 2560) has too little RAM for that. It
  sends each page exactly as the cartridge stores it, marked with an
  "X-NES-Packet: nhf2" header, and this bridge decompresses it.

The gateway handles one request at a time - fetching a page from the NES
takes ~0.7s, longer if the link has to retry, and the gateway gives up
after 5s (see docs/protocol.md); the ESP32 serves later hits from its RAM
cache - so requests to the serial port are serialised behind a lock
rather than sent concurrently.

    python scripts/serial_bridge.py --serial-port COM3
    python scripts/serial_bridge.py --serial-port /dev/ttyACM0 --port 8080
"""
import argparse
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial
import serial.tools.list_ports

import emulate_rom

RESPONSE_TIMEOUT = 8.0   # covers the gateway's 5s fetch deadline with margin
LINE_TIMEOUT = 2.0       # max time to wait for any single line while within budget
BOOT_TIMEOUT = 4.0       # how long to wait for the gateway's "online" line
PACKET_HEADER = "X-NES-Packet"

SERIAL_LOCK = threading.Lock()
ser = None  # opened in main()


def autodetect_port():
    markers = ("USB", "CDC", "Arduino", "CH340")
    candidates = [p for p in serial.tools.list_ports.comports()
                  if any(m in (p.description or "") for m in markers)]
    if len(candidates) == 1:
        return candidates[0].device
    return None


def _read_line(deadline):
    """Read one line from the gateway, honouring both the per-line and
    overall deadlines. Returns the decoded, stripped line, or None on
    timeout."""
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        return None
    ser.timeout = min(LINE_TIMEOUT, remaining)
    raw = ser.readline()
    if not raw:
        return None
    return raw.decode("latin-1").rstrip("\r\n")


def _read_exact(n, deadline):
    data = bytearray()
    while len(data) < n:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        ser.timeout = min(LINE_TIMEOUT, remaining)
        chunk = ser.read(n - len(data))
        if not chunk:
            break
        data += chunk
    return bytes(data)


def wait_for_gateway(timeout=BOOT_TIMEOUT):
    """An Arduino Mega resets when its serial port is opened and spends a
    moment in its bootloader, so a request sent straight away is lost.
    Wait for the firmware's "online" line. The ESP32's native USB does not
    reset, so it says nothing and this just runs out the timeout."""
    deadline = time.monotonic() + timeout
    while True:
        line = _read_line(deadline)
        if line is None:
            print("Gateway did not announce itself (normal for the ESP32, which does not reset); carrying on.")
            return False
        if line.startswith("#"):
            print(line, flush=True)
            if "online" in line:
                return True


def decode_packet_body(headers, body):
    """Decompress a page the gateway sent as a raw cartridge packet.

    Only responses marked X-NES-Packet are touched; everything else (the
    ESP32 gateway, /_link) passes through as it is. Returns the headers
    and body to relay. Raises ValueError for a packet that is cut short or
    does not decode cleanly, so a damaged page is refused, not served."""
    kind = next((v for k, v in headers.items() if k.lower() == PACKET_HEADER.lower()), None)
    if kind is None:
        return headers, body
    if kind.lower() != "nhf2":
        raise ValueError(f"unknown packet format {kind!r}")
    declared = int(headers.get("Content-Length", "-1"))
    if len(body) != declared:
        raise ValueError(f"packet cut short: {len(body)} of {declared} bytes")
    try:
        text, consumed = emulate_rom.decompress_packet(body, strict=True)
    except Exception as e:
        raise ValueError(f"packet does not decode: {e}") from e
    if consumed != len(body):
        raise ValueError(f"packet is {len(body)} bytes but decodes from {consumed}")
    headers = {k: v for k, v in headers.items() if k.lower() != PACKET_HEADER.lower()}
    return headers, text.encode("latin-1")


def fetch(path):
    """Send one request to the gateway and return (status, reason,
    headers, body), with any raw packet already decompressed. Raises
    TimeoutError if no HTTP status line ever arrives, and ValueError for
    a packet that does not decode."""
    deadline = time.monotonic() + RESPONSE_TIMEOUT

    ser.reset_input_buffer()
    ser.write(f"GET {path} HTTP/1.1\n".encode("ascii", errors="ignore"))
    ser.flush()

    status_line = None
    while True:
        line = _read_line(deadline)
        if line is None:
            raise TimeoutError("no response from the gateway")
        if line.startswith("#"):
            print(line, flush=True)   # firmware debug/log line
            continue
        if line.startswith("HTTP/"):
            status_line = line
            break
        # blank line or anything unexpected before the status line: ignore

    # "HTTP/1.1 200 OK" -> (200, "OK")
    _, rest = status_line.split(" ", 1)
    code_str, _, reason = rest.partition(" ")
    status = int(code_str)

    headers = {}
    while True:
        line = _read_line(deadline)
        if line is None:
            raise TimeoutError("connection to the gateway dropped mid-response")
        if line == "":
            break
        key, _, value = line.partition(":")
        headers[key.strip()] = value.strip()

    content_length = int(headers.get("Content-Length", "0"))
    body = _read_exact(content_length, deadline)
    headers, body = decode_packet_body(headers, body)
    return status, reason, headers, body


class BridgeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        with SERIAL_LOCK:
            try:
                status, reason, headers, body = fetch(self.path)
            except (TimeoutError, serial.SerialException) as e:
                self._error(504, "Gateway Timeout", e)
                return
            except ValueError as e:
                self._error(502, "Bad Gateway", e)
                return

        self.send_response(status, reason)
        for key, value in headers.items():
            if key.lower() == "content-length":
                continue
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status, reason, error):
        print(f"bridge: {error}", flush=True)
        self.send_response(status, reason)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(f"bridge: {error}\n".encode("utf-8"))

    def log_message(self, format, *args):
        print(f"{self.address_string()} - {format % args}", flush=True)


def main():
    global ser

    # CLI flags win; falling back to env vars lets a systemd EnvironmentFile
    # configure this without editing the unit file.
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial-port", default=os.environ.get("NES_BRIDGE_SERIAL_PORT"),
                         help="e.g. COM3 or /dev/ttyACM0 (auto-detected if omitted and only one candidate is present)")
    parser.add_argument("--baud", type=int, default=int(os.environ.get("NES_BRIDGE_BAUD", "115200")),
                         help="115200 for the Arduino Mega; ignored by the ESP32's native USB CDC")
    parser.add_argument("--host", default=os.environ.get("NES_BRIDGE_HOST", "127.0.0.1"),
                         help="bind address for the HTTP side (default: localhost only)")
    parser.add_argument("--port", type=int, default=int(os.environ.get("NES_BRIDGE_PORT", "8080")),
                         help="HTTP port for the tunnel/reverse proxy to hit")
    args = parser.parse_args()

    port = args.serial_port or autodetect_port()
    if not port:
        print("No --serial-port given and could not auto-detect a single USB serial device.", file=sys.stderr)
        print("Available ports:", file=sys.stderr)
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}  {p.description}", file=sys.stderr)
        sys.exit(1)

    print(f"Opening {port} @ {args.baud}...")
    ser = serial.Serial(port, args.baud, timeout=LINE_TIMEOUT)
    wait_for_gateway()

    httpd = ThreadingHTTPServer((args.host, args.port), BridgeHandler)
    print(f"Serving on http://{args.host}:{args.port} -> {port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
    finally:
        httpd.server_close()
        ser.close()


if __name__ == "__main__":
    main()

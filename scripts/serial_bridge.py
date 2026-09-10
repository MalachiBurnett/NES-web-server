"""Bridge real HTTP requests to the ESP32 gateway over USB serial.

Point a Cloudflare Tunnel (or any reverse proxy) at this server's port.
Each request is forwarded to the ESP32 as a single line (its own
line-based protocol, see NES_router/NES_router.ino's loop()/serve()), and the raw
HTTP response the ESP32 writes back is relayed to the client unchanged.

The ESP32 handles one request at a time - fetching an uncached page from
the NES takes ~0.7s, longer if the link has to retry, and the gateway gives
up after 5s (see docs/protocol.md); later hits are served instantly from its
RAM cache - so requests to the serial port are serialised behind a lock
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

RESPONSE_TIMEOUT = 8.0   # covers the gateway's 5s fetch deadline with margin
LINE_TIMEOUT = 2.0       # max time to wait for any single line while within budget

SERIAL_LOCK = threading.Lock()
ser = None  # opened in main()


def autodetect_port():
    candidates = [p for p in serial.tools.list_ports.comports()
                  if "USB" in (p.description or "") or "CDC" in (p.description or "")]
    if len(candidates) == 1:
        return candidates[0].device
    return None


def _read_line(deadline):
    """Read one line from the ESP32, honouring both the per-line and
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


def fetch(path):
    """Send one request to the ESP32 and return (status, reason, headers,
    body). Raises TimeoutError if no HTTP status line ever arrives."""
    deadline = time.monotonic() + RESPONSE_TIMEOUT

    ser.reset_input_buffer()
    ser.write(f"GET {path} HTTP/1.1\n".encode("ascii", errors="ignore"))
    ser.flush()

    status_line = None
    while True:
        line = _read_line(deadline)
        if line is None:
            raise TimeoutError("no response from ESP32 gateway")
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
            raise TimeoutError("connection to ESP32 gateway dropped mid-response")
        if line == "":
            break
        key, _, value = line.partition(":")
        headers[key.strip()] = value.strip()

    content_length = int(headers.get("Content-Length", "0"))
    body = _read_exact(content_length, deadline)
    return status, reason, headers, body


class BridgeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        with SERIAL_LOCK:
            try:
                status, reason, headers, body = fetch(self.path)
            except (TimeoutError, serial.SerialException) as e:
                self.send_response(504, "Gateway Timeout")
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(f"bridge: {e}\n".encode("utf-8"))
                return

        self.send_response(status, reason)
        for key, value in headers.items():
            if key.lower() == "content-length":
                continue
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

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
                         help="ignored by the ESP32's native USB CDC, kept for other boards")
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
    time.sleep(2)  # let the board finish its USB-CDC reset before the first request

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

"""Test the serial link between the gateway and this machine on its own.

The gateway sends 16384 bytes of a known pattern (GET /_serial) and this
checks that every one arrived, and if not, which went missing and how
far apart the losses were.  No NES is involved in the pattern, so running
it with the gateway's cable in the NES and then out shows whether the NES
has anything to do with lost bytes.

Stop the bridge first: only one program can hold the port.

    python scripts/serial_test.py COM4
    python scripts/serial_test.py COM4 --runs 5 --baud 250000
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial missing:  pip install -r requirements.txt")
    sys.exit(2)

SIZE = 16384


def expected(i):
    return ((i & 0xFF) ^ (i >> 8)) & 0xFF


EXPECTED = bytes(expected(i) for i in range(SIZE))


def analyse(data):
    """Walk the received bytes against the pattern.  Returns a list of
    (position in the pattern, bytes lost, bytes garbled, bytes extra)."""
    events = []
    j = k = 0
    run = 6   # bytes that must match again before we call it back in step
    while j < SIZE and k < len(data):
        if data[k] == EXPECTED[j]:
            j += 1
            k += 1
            continue
        # A receiver that loses the byte framing mid-stream can take a few
        # dozen bytes to find it again, all of them garbled.
        best = None
        for skip_rx in range(0, 64):
            for skip_exp in range(0, 72):
                a, b = k + skip_rx, j + skip_exp
                if a + run <= len(data) and b + run <= SIZE and data[a:a + run] == EXPECTED[b:b + run]:
                    if best is None or skip_rx + skip_exp < sum(best):
                        best = (skip_rx, skip_exp)
                    break
        if best is None:          # lost our place entirely: count the rest as garbled
            events.append((j, 0, len(data) - k, 0))
            return events, j
        skip_rx, skip_exp = best
        garbled = min(skip_rx, skip_exp)
        events.append((j, skip_exp - garbled, garbled, skip_rx - garbled))
        j += skip_exp
        k += skip_rx
    return events, j


def wait_online(ser, timeout=4.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        line = ser.readline()
        if b"online" in line:
            return True
    return False


def one_run(ser, byte_rate):
    ser.reset_input_buffer()
    ser.write(b"GET /_serial HTTP/1.1\n")
    ser.flush()
    status = None
    headers_done = False
    end = time.monotonic() + 5
    while time.monotonic() < end:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("latin-1").rstrip("\r\n")
        if status is None:
            if text.startswith("HTTP/"):
                status = text
            continue
        if text == "":
            headers_done = True
            break
    if not headers_done:
        print("  no response from the gateway (is it the new firmware? is the bridge still running?)")
        return None

    # The port's timeout is left as it was opened: changing it on Windows
    # restarts the Mega's USB chip's receiver mid-byte (see serial_bridge.py).
    data = bytearray()
    while len(data) < SIZE:
        chunk = ser.read(SIZE - len(data))
        if not chunk:
            break
        data += chunk

    events, reached = analyse(bytes(data))
    lost = sum(e[1] for e in events) + (SIZE - reached)
    garbled = sum(e[2] for e in events)
    extra = sum(e[3] for e in events)
    print(f"  received {len(data)} of {SIZE} bytes: {lost} lost, {garbled} garbled, {extra} extra, in {len(events)} places")
    if len(events) > 1:
        gaps = [(b[0] - a[0]) / byte_rate * 1000 for a, b in zip(events, events[1:])]
        gaps.sort()
        print(f"  time between faults: shortest {gaps[0]:.1f} ms, median {gaps[len(gaps) // 2]:.1f} ms, longest {gaps[-1]:.1f} ms")
    for pos, l, g, x in events[:8]:
        what = ", ".join(s for s in (f"{l} lost" if l else "", f"{g} garbled" if g else "", f"{x} extra" if x else "") if s)
        print(f"    at byte {pos:5} ({pos / byte_rate * 1000:6.1f} ms in): {what}")
    if len(events) > 8:
        print(f"    ... and {len(events) - 8} more")
    return lost + garbled + extra


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="e.g. COM4")
    parser.add_argument("--baud", type=int, default=250000)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1.0)
    print(f"opened {args.port} @ {args.baud}, waiting for the gateway...")
    if not wait_online(ser):
        print("the gateway never said it was online - carrying on anyway")
    byte_rate = args.baud / 10

    faults = 0
    for n in range(args.runs):
        print(f"run {n + 1}:")
        result = one_run(ser, byte_rate)
        if result is None:
            return 1
        faults += result
    print("\nall clean" if faults == 0 else f"\n{faults} faulty bytes over {args.runs} runs")
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

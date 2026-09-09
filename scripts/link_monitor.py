"""Interactive serial monitor for the link tester (src/firmware/NES_debug).

An alternative to `arduino-cli monitor` that also tees everything to a
log file, so the session can be pasted somewhere afterwards.

    python scripts/link_monitor.py [PORT] [LOGFILE]

Defaults: COM3, link-test.log.  Type commands (d0 1, d0 0, d0 sq, zero,
?) and press enter.  Ctrl-C to quit.
"""
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("pyserial missing:  pip install -r requirements.txt")
    sys.exit(2)

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM3"
LOG = sys.argv[2] if len(sys.argv) > 2 else "link-test.log"

stop = False

def reader(port, log):
    buf = b""
    while not stop:
        try:
            chunk = port.read(256)
        except Exception as e:
            print("\n[read error: %s]" % e)
            return
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            stamp = time.strftime("%H:%M:%S")
            print("%s  %s" % (stamp, text))
            log.write("%s  %s\n" % (stamp, text))
            log.flush()

def main():
    global stop
    try:
        port = serial.Serial(PORT, 115200, timeout=0.2)
    except Exception as e:
        print("could not open %s: %s" % (PORT, e))
        return 1

    log = open(LOG, "a", encoding="utf-8")
    log.write("\n=== session %s ===\n" % time.strftime("%Y-%m-%d %H:%M:%S"))

    print("connected to %s, logging to %s" % (PORT, LOG))
    print("type a command and press enter (d0 1 | d0 0 | d0 sq | zero | ?)")
    print("ctrl-c to quit\n")

    t = threading.Thread(target=reader, args=(port, log), daemon=True)
    t.start()

    try:
        for line in sys.stdin:
            cmd = line.strip()
            if not cmd:
                continue
            port.write((cmd + "\n").encode())
            port.flush()
            log.write(">> %s\n" % cmd)
            log.flush()
    except KeyboardInterrupt:
        pass
    finally:
        stop = True
        time.sleep(0.3)
        port.close()
        log.close()
        print("\nclosed. log saved to %s" % LOG)
    return 0

if __name__ == "__main__":
    sys.exit(main())

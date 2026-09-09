"""Assemble the bring-up test ROMs.

These are not the web server - they exercise the controller port link
so faults can be isolated one wire at a time.  Neither has any site
data to inject, which is why they do not go through build_rom.py.

    python scripts/build_test_roms.py          # both
    python scripts/build_test_roms.py d0       # just the D0 test

See docs/bring-up.md for what they do and how to read them.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ROMS = {
    "d0":   ("d0test.asm", "nes_d0_test.nes",   "D0 line test"),
    "link": ("debug.asm",  "nes_link_test.nes", "five phase link test"),
}

def build(key):
    source, out, label = ROMS[key]
    assembler = os.path.join(ROOT, "tools", "assembler", "assemble.exe")
    src_dir = os.path.join(ROOT, "src", "nes")
    output = os.path.join(ROOT, "build", out)

    print("Assembling %s (%s)..." % (label, source))
    try:
        r = subprocess.run(
            [assembler, source, output],
            cwd=src_dir,
            capture_output=True,
            text=True,
        )
    except Exception as e:
        print("ERROR: could not run assembler: %s" % e)
        return False

    if r.stdout.strip():
        print(r.stdout.strip())
    if r.returncode != 0:
        print("ERROR: assembly failed")
        print(r.stderr.strip())
        return False

    print("SUCCESS: build/%s" % out)
    return True

def main():
    wanted = sys.argv[1:] or list(ROMS)
    for key in wanted:
        if key not in ROMS:
            print("unknown target %r - pick from: %s" % (key, ", ".join(ROMS)))
            return 1
    ok = True
    for key in wanted:
        ok = build(key) and ok
        print("-" * 40)
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Compile the decompiled game's sources for the PS3 PPU target."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "port/tools"))
import ps3
import portsrc

def main():
    files = portsrc.sources()
    errors = []
    for source in files:
        _, error = ps3.compile_one(source)
        if error:
            errors.append((source.name, error))
        else:
            print("ok", source.name)
    for name, error in errors:
        print("FAILED", name, error[:400].replace("\\n", " | "))
    print("compiled:", len(files) - len(errors), "of", len(files))
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main())

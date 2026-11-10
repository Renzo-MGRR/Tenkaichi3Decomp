#!/usr/bin/env python3
"""Assemble the committed blank game-data tables for the PPU ELF."""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "port/data"
OUT = ROOT / "port/build/obj_ps3_data"


def main():
    files = sorted(SRC.glob("*.s"))
    if not files:
        print(f"no committed game-data tables in {SRC}")
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.o"):
        old.unlink()
    failed = []
    for source in files:
        obj = OUT / (source.stem + ".o")
        result = subprocess.run(["ppu-gcc", "-c", "-msoft-float", "-fno-pic", "-fno-pie",
                                 str(source), "-o", str(obj)], cwd=ROOT, capture_output=True, text=True)
        if result.returncode:
            failed.append((source.name, result.stderr.strip()))
    if failed:
        for name, error in failed:
            print(f"FAILED {name}: {error[-500:]}")
        return 1
    print(f"assembled {len(files)} PS3 data-table objects")
    return 0


if __name__ == "__main__":
    sys.exit(main())

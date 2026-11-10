#!/usr/bin/env python3
"""PS3 build of the port-side sources: the PS2's maths library, the VU0 files and port/src, through the
pipeline (the rules of port/tools/undefined.py, PS3 edition). The cut files (the settings window, the
mods, the accessibility, the PC headless driver) and plat_fastvec.c (only referenced with
-DSF_FAST_CALLS, which the PS3 build does not use in v1) are left out. plat_libm.c is compiled with
ppu-gcc in port/ps3/halbuild.sh: it defines the double-ABI bridges, and its body uses the PPU's own
floating unit, so a soft compile of it would call itself.

Usage: python3 port/tools/ps3port.py [names...]   (names: a file's stem, or omitted for everything)
"""
import pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
OBJ = ROOT / "port/build/obj_ps3"
sys.path.insert(0, str(ROOT / "port/tools"))
import ps3, portsrc

# The PC-only files (port/tools/undefined.py's set, Platform/Window UI, and everything cut for v1).
SKIP = {
    "plat_verify.c",    # the PC-only self-check (BT3_VERIFY)
    "a11y_game.c",      # the screen reader: cut for v1
    "mods_game.c",      # the script mods: cut for v1
    "quit_dialog.c",    # the GTK close-dialog: no window here
    "spec_menu.c",      # the spectator menu: cut for v1
    "headless.c",       # the PC headless driver and its __wrap_*s: the real DBZP.BIN is linked in on PS3
    "plat_libm.c",      # compiled natively by port/ps3/halbuild.sh (its own float unit)
    "plat_stub.c",      # compiled natively by port/ps3/halbuild.sh (PSL1GHT headers on top)
    "plat_crash.c",     # the crash report's Windows paths and glibc execinfo: PS3 gets its own later
    "plat_fprint.c",    # the fingerprint printer: touches st_mtim and creates a thread; come back to it
}

def prune():
    """Delete leftover objects of skipped files from an earlier run."""
    for f in SKIP:
        for stub in (OBJ / f"p_port_src_{f[:-2]}.o", OBJ / f"p_port_src_{f[:-2]}.i", OBJ / f"p_port_src_{f[:-2]}.ll"):
            stub.unlink(missing_ok=True)

def files():
    up = sorted((ROOT / "port/third_party/newlib_libm").glob("*.c"))
    vu0 = [ROOT / "src/port/vu0_a.c", ROOT / "src/port/vu0_b.c"]
    # Files of port/src and port/src/gs the PS3 v1 build can take as they are (compiled through the
    # pipeline). The SDL/users-window back ends (gs_gl, gs_gpu, gs_draw, snd_*, ui*.cpp, net*, mods*,
    # a11y) are cut: plat_stub.c and the stop-stubs stand in for their entry points.
    top = [f for f in sorted((ROOT / "port/src").glob("*.c")) if f.name not in SKIP and f.name[:4] != "bt3_"]
    gsnames = {"gs_sony.c", "gs_core.c", "gs_vu1.c", "menu_text.c", "menu_labels.c", "menu_translation.c",
               "namefont.c", "ui_text_extract.c", "ui_text_translate.c"}
    gs = [ROOT / "port/src/gs" / n for n in sorted(gsnames)]
    return up + vu0 + top + gs

def main(argv):
    names = set(argv)
    prune()
    fs = files()
    ok, failed = 0, []
    for f in fs:
        if names and f.stem not in names:
            continue
        extra = []
        if f.parent.name == "newlib_libm":
            extra = ["-include", "newlib_shim.h"]
        elif f.name in ("vu0_a.c", "vu0_b.c"):
            # the vector references: the PS2's VU0 entry names; port_libm renames the float routines so
            # the soft-compiled file calls the integer-ABI wrappers in plat_libm.o (its own float unit)
            extra = ["-include", "vu0_names.h", "-DREF_VU0_EXTERN_ARITH", "-include", "port_libm.h"]
        o, err = ps3.compile_one(f, route="newlib" if f.parent.name == "newlib_libm" else "port", extra_cpp=extra,
                                 hard_float=(f.name in ("gs_core.c",)))
        if err:
            print("FAILED", f.name, err[:400])
            failed.append(f.name)
        else:
            print("ok", f.name)
            ok += 1
    print("compiled:", ok, "failed:", len(failed))
    return 0 if not failed else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

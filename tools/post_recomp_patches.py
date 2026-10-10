#!/usr/bin/env python3
"""
post_recomp_patches.py — re-apply hand-written redirects that can't be expressed
in symbols.toml (i.e. routing a recompiled game function to a different
ultramodern *_recomp reimplementation).

Run after every N64Recomp regeneration, together with fix_fallthroughs.py:
    N64Recomp.exe recomp.toml
    py tools/fix_fallthroughs.py
    py tools/post_recomp_patches.py

Each entry redirects a function's body to call <target>(rdram, ctx) and return,
discarding the recompiled body. Idempotent.

Why these can't be symbols.toml renames:
- func_80087D70 is the game's osPiStartDma, but the name "osPiStartDma" is
  already taken by another copy (0x80095820). Routing it to ultramodern's
  osPiStartDma_recomp bypasses the stubbed PI-manager thread so DMAs actually
  complete (otherwise the game blocks forever on the DMA reply queue).
"""
import re, glob, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP_DIR = os.path.join(ROOT, "RecompiledFuncs")
MARKER = "@post-recomp-redirect"

# func name -> ultramodern reimplementation to call instead
REDIRECTS = {
    "func_80087D70": "osPiStartDma_recomp",   # game osPiStartDma -> bypass dead PI manager
    # func_8008C930_exception is the game's real osSendMesg (blocking, wakes
    # receivers via osStartThread); the scheduler's frame-done send goes through
    # it. Left stubbed, every send through it vanishes and the OSSched client
    # protocol deadlocks. Route it to ultramodern's osSendMesg.
    "func_8008C930_exception": "osSendMesg_recomp",
    # "dma_read_rom" is a misidentification: VA 0x8008A6B4 is the game's
    # sprintf (stores register args to caller-frame homes, calls vformatter
    # func_80093180 with emit cb 0x8008A690, NUL-terminates). The vformatter's
    # conversion dispatcher is an indirect ROM jump table that static
    # recompilation can't resolve, so the cluster is stubbed and every sprintf
    # call crashed the menu text path in bcopy. Route to the native
    # guest-ABI sprintf in main.cpp.
    "dma_read_rom": "swe1r_sprintf",
    # func_8008A710 is the game's osPiReadIo (a0=cart addr, a1=u32* out). Its
    # get-access/raw-read/rel-access leaves are stubbed, so the word-at-a-time
    # ROM reader fed the asset loader garbage and every asset lookup failed.
    "func_8008A710": "swe1r_pi_read_io",
}

def main():
    patched = 0
    for fp in glob.glob(os.path.join(RECOMP_DIR, "funcs_*.c")):
        lines = open(fp, encoding="utf-8", errors="replace").read().split("\n")
        out = []
        i = 0
        changed = False
        while i < len(lines):
            out.append(lines[i])
            m = re.match(r'^RECOMP_FUNC\s+void\s+(\w+)\s*\(', lines[i])
            if m and m.group(1) in REDIRECTS:
                name = m.group(1); target = REDIRECTS[name]
                # copy the two prologue decl lines, then inject the redirect
                # (only if not already injected)
                lookahead = "\n".join(lines[i:i+8])
                if MARKER in lookahead:
                    i += 1; continue
                # emit decls (next 2 non-empty lines are the hi/lo + c1cs decls)
                j = i + 1
                while j < len(lines) and lines[j].strip() in (
                        "uint64_t hi = 0, lo = 0, result = 0;", "int c1cs = 0;"):
                    out.append(lines[j]); j += 1
                out.append(f"    // {MARKER}: route to {target}")
                out.append(f"    {target}(rdram, ctx);")
                out.append("    return;")
                i = j; changed = True; patched += 1
                continue
            i += 1
        if changed:
            open(fp, "w", encoding="utf-8").write("\n".join(out))
    print(f"post-recomp redirects applied: {patched}")

if __name__ == "__main__":
    main()

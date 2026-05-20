#!/usr/bin/env python3
"""
Session 169 flake audit — run each Path C smoke N times in a row
and tabulate pass rates.  Anything that consistently fails is
either (a) a real bug we should fix or (b) a smoke-side bug
we should harden.  Anything that passes 5/5 is solid.

Smokes are ordered by recency so we exercise the most-recent
code first.  N is configurable; defaults to 3 for a fast pass.
"""
import os, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))

SMOKES = [
    # Session 169 — original Path C / wmterm flake-hunt set.
    "smoke_wmterm_polish_168.py",   # session 168
    "smoke_wmterm_color.py",        # session 166/167
    "smoke_wmterm_geom_cursor.py",  # session 162
    "smoke_wmd_resize_cursors.py",  # session 164
    "smoke_wmterm_select.py",       # session 165
    "smoke_wmterm_clear_wheel.py",  # session 163
    "smoke_wmterm_scrollback.py",   # session 159
    "smoke_wmterm_polish.py",       # session 161 — known flaky
    "smoke_wmterm_bg.py",           # session 160
    "smoke_wmterm_sighup.py",       # session 158
    "smoke_wmterm_fix.py",          # session 157
    # Session 171 — extended set: non-wmterm Path C smokes confirmed
    # flaky in the broader audit pass.  Both pass ~1/3 fresh-QEMU
    # runs; the retry mechanism is what catches them reliably.
    "smoke_wmedit.py",              # session 137 — startup paint race
    "smoke_workspaces.py",          # session 147 — small-target click flake
]

N = int(sys.argv[1]) if len(sys.argv) > 1 else 3


def run_one(smoke, retries=3):
    """Run one smoke; retry up to `retries` times if it fails.
    Returns (returncode, attempts_taken, last 20 lines)."""
    last = None
    for attempt in range(1, retries + 1):
        subprocess.run(["cmd", "/c", "taskkill /IM qemu-system-i386.exe /F"],
                       capture_output=True)
        time.sleep(4)
        try:
            r = subprocess.run(
                ["python", os.path.join(ROOT, smoke)],
                capture_output=True, text=True, timeout=240)
            rc, tail = r.returncode, r.stdout.splitlines()[-20:]
        except subprocess.TimeoutExpired:
            rc, tail = -1, ["<timeout>"]
        last = (rc, attempt, tail)
        if rc == 0:
            return last
    return last


def main():
    print(f"=== Flake audit — {N} runs per smoke ===\n")
    results = {}
    for smoke in SMOKES:
        if not os.path.exists(os.path.join(ROOT, smoke)):
            print(f"   {smoke:<35} SKIP (missing)")
            continue
        passes = 0
        attempts_total = 0
        last_fail = None
        for i in range(N):
            try:
                rc, attempts, tail = run_one(smoke)
            except subprocess.TimeoutExpired:
                rc, attempts, tail = -1, 1, ["<timeout>"]
            attempts_total += attempts
            if rc == 0:
                passes += 1
            else:
                last_fail = tail
        pct = passes * 100 // N
        results[smoke] = (passes, last_fail)
        mark = "OK   " if passes == N else ("FLAKY" if passes else "BROKE")
        print(f"   [{mark}] {smoke:<35} {passes}/{N} "
              f"({pct}%, {attempts_total} tries)", flush=True)

    print("\n=== summary ===")
    solid    = [s for s, (p, _) in results.items() if p == N]
    flaky    = [s for s, (p, _) in results.items() if 0 < p < N]
    broken   = [s for s, (p, _) in results.items() if p == 0]
    print(f"  {len(solid)} solid, {len(flaky)} flaky, {len(broken)} broken")
    for s in flaky:
        p, tail = results[s]
        print(f"\n--- {s} (last failure tail) ---")
        for l in tail: print(f"   {l}")
    for s in broken:
        p, tail = results[s]
        print(f"\n--- {s} (last failure tail) ---")
        for l in tail: print(f"   {l}")
    return 0 if not (flaky or broken) else 1


if __name__ == "__main__":
    sys.exit(main())

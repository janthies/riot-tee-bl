"""Cost of one BL_PHASE() toggle: 10000 toggles in a row before P0.16 rises.

  python3 burst.py <image .bin>     (image already flashed)
"""
import subprocess, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "boot-time"))
import bootbench as b

N = 10000
img = sys.argv[1]
subprocess.run(["arm-none-eabi-objcopy", "-I", "binary", "-O", "ihex", img, "image.hex"], check=True)
s = b.Scope("169.254.215.74"); bd = b.Board(None)
s.setup([1, 3, 4], 3, 0.01, 0.9, "3000000", 1)
# already flashed: bd.program("image.hex")
for n in range(5):
    s.arm(); bd.reset("reset")
    if not s.wait_stop(3):
        print(n, "no trigger"); continue
    d1, xinc, v1 = s.read_raw(1); d3, _, v3 = s.read_raw(3); d4, _, v4 = s.read_raw(4)
    s1, s3, s4 = v1(d1) > 0.9, v3(d3) > 0.9, v4(d4) > 0.9
    res = b.analyse(d3, xinc, v3)
    r, fall = res["_rise"], res["_fall"]
    hi = np.flatnonzero(s1[:r])
    first, last = hi[0], hi[-1] + 1
    t = (last - first) * xinc / (N - 1)
    e4 = b.edges(s4.astype(np.int8), round(1e-6 / xinc))
    e4 = e4[(e4 >= r) & (e4 <= fall)]
    if len(e4) < 3:
        print(n, "clock edges in window:", len(e4), "r", r, "fall", fall); continue
    f = 64 * (8000 / 16e6) / ((e4[-1] - e4[0]) * xinc / (len(e4) - 1))
    print(f"boot {n}: burst {(last - first) * xinc * 1e6:9.2f} us, gap to P0.16 {(r - last) * xinc * 1e9:6.0f} ns, "
          f"per toggle {t * 1e9:7.2f} ns = {t * f * 1e6:5.2f} cycles at {f:.4f} MHz")

#!/usr/bin/env python3
"""Boot-time benchmark for BL1 on the nRF9160 DK.

BL1 drives P0.16 high from the start of main() until just before the jump to
riot-tee (BL_TIMING_PIN in riot-tee-bl/src/main.c). A Rigol DS1000Z on the LAN
captures one pulse per reset, and the pulse width is computed from the raw
sample memory. Each round erases the chip and programs the image, which makes
the first boot a provisioning boot, and then resets the board for a number of
normal boots.

With the instrumented BL1, three more channels are read: a pin that toggles at
every phase boundary, a stage pin that riot-tee toggles when it starts and
before it calls RIOT and that RIOT toggles when it reaches main(), and a pin
that a timer toggles every 8000 ticks of the 16 MHz clock derived from HFCLK.

In the runs of 2026-10-29 one pin (P0.10) carries all markers of BL1, riot-tee and
RIOT; see README.md for the commands.

  bootbench.py run --rounds 20 --boots 4 --marker-channel 1 --clock-channel 4 --probe 10 \
      --mdepth 6000000 --timebase 0.02 --pre-divs 0.5 --image <image .bin>
  bootbench.py summary runs/<stamp>/results.csv
"""

import argparse
import csv
import hashlib
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
DEFAULT_IMAGE = (Path.home() / "hauptprojekt/RIOT/tests/sys/psa_riot_tee/bin"
                 / "nrf9160dk/tests_psa_riot_tee.bl.combined.bin")

OTP8 = 0x00FF8128       # UICR OTP[8] (0x00FF8000 + 0x108 + 8 * 4), BL1's marker
MAGIC = 0x52544231      # "RTB1", written last by a completed provisioning
ERASED = 0xFFFFFFFF
MAX_POINTS = 250000     # BYTE format, per :WAV:DATA? read (DS1000Z PG, p. 2-219)

# Phases of BL1 between two edges of the phase pin, in the order of main.c.
PHASES = {
    "normal": ["cc_init", "hash", "state", "compare", "kdr_push", "lcs_lock", "cc_off",
               "spu_lock"],
    "provisioning": ["cc_init", "hash", "state", "trng", "kmu_write", "otp_hash",
                     "otp_marker", "kdr_push", "lcs_lock", "cc_off", "spu_lock"],
}
PHASE_FIELDS = [f"{n}_us" for n in ["cc_init", "hash", "state", "compare", "trng", "kmu_write",
                                     "otp_hash", "otp_marker", "kdr_push", "lcs_lock", "cc_off",
                                     "spu_lock"]]
# After BL1, between the edges of the stage pin, in the order of riot-tee's
# main() (branch eval/boot-markers): BL1 ends, riot-tee main() starts, then one
# toggle after each start-up step, the last one before the call of the
# non-secure reset handler, and RIOT toggles when its main() starts
STAGES = [f"{n}_us" for n in [
    "tee_startup", "tee_spu_flash", "tee_spu_ram", "tee_spu_nsc", "tee_spu_periph",
    "tee_spu_gpio", "tee_led_pin", "tee_fpu_prio", "tee_trng", "tee_drbg", "tee_cc_init",
    "tee_platform_key", "tee_ns_setup", "riot_startup"]]
CLOCK_HALF_PERIOD = 8000 / 16e6     # CLOCK_REF_TICKS at the nominal 16 MHz

FIELDS = ["time", "round", "boot", "kind", "reset_mode", "status", "width_us",
          "scope_pwid_us", "t_rise_us", "pulses", "edges", "xinc_ns", "high_v", "low_v",
          "high_noise_mv", "low_noise_mv", *PHASE_FIELDS, *STAGES, "hfclk_mhz",
          "marker_before", "marker_after"]


class Scope:
    """SCPI over a raw TCP socket."""

    def __init__(self, host, port=5555):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buf = b""

    def write(self, cmd):
        self.sock.sendall(cmd.encode() + b"\n")

    def _fill(self):
        chunk = self.sock.recv(1 << 16)
        if not chunk:
            raise ConnectionError("scope closed the connection")
        self.buf += chunk

    def _recv(self, n):
        while len(self.buf) < n:
            self._fill()
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _readline(self):
        while b"\n" not in self.buf:
            self._fill()
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode().strip()

    def query(self, cmd):
        self.write(cmd)
        return self._readline()

    def query_block(self, cmd):
        """Reads an IEEE 488.2 block such as #9000120000<data>."""
        self.write(cmd)
        head = self._recv(2)
        n = int(self._recv(int(head[1:2])))
        data = self._recv(n)
        self._readline()
        return data

    def setup(self, channels, ch, timebase, level, mdepth, probe, pre_divs=1.0):
        """ch is the channel of the BL1 pulse, which triggers the capture."""
        self.write(":SYST:BEEP OFF")
        self.write(":RUN")
        for c in range(1, 5):
            self.write(f":CHAN{c}:DISP {1 if c in channels else 0}")
        for c in channels:
            self.write(f":CHAN{c}:PROB {probe}")
            # 0.5 V/div around +1 V shows -1 V to 3 V, for 1.8 V and 3 V I/O.
            self.write(f":CHAN{c}:SCAL 0.5")
            self.write(f":CHAN{c}:OFFS -1.0")
        self.write(f":TIM:MAIN:SCAL {timebase}")
        # Trigger pre_divs divisions from the left edge of the 12-division screen.
        self.write(f":TIM:MAIN:OFFS {(6 - pre_divs) * timebase}")
        self.write(":TRIG:MODE EDGE")
        self.write(f":TRIG:EDG:SOUR CHAN{ch}")
        self.write(":TRIG:EDG:SLOP POS")
        self.write(f":TRIG:EDG:LEV {level}")
        self.write(f":ACQ:MDEP {mdepth}")
        self.write(":TRIG:SWE SING")
        self.query("*OPC?")
        queries = ["*IDN?"]
        for c in channels:
            queries += [f":CHAN{c}:PROB?", f":CHAN{c}:SCAL?", f":CHAN{c}:OFFS?"]
        queries += [":TIM:MAIN:SCAL?", ":TIM:MAIN:OFFS?", ":TRIG:EDG:SOUR?",
                    ":TRIG:EDG:LEV?", ":ACQ:MDEP?", ":ACQ:SRAT?"]
        return {c: self.query(c) for c in queries}

    def _wait_status(self, want, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.query(":TRIG:STAT?") == want:
                return True
            time.sleep(0.02)
        return False

    def arm(self):
        self.write(":SING")
        if not self._wait_status("WAIT", 5):
            raise RuntimeError("scope did not arm")

    def wait_stop(self, timeout):
        return self._wait_status("STOP", timeout)

    def read_raw(self, ch):
        self.write(f":WAV:SOUR CHAN{ch}")
        self.write(":WAV:MODE RAW")
        self.write(":WAV:FORM BYTE")
        pre = self.query(":WAV:PRE?").split(",")
        points, xinc = int(pre[2]), float(pre[4])
        yinc, yorig, yref = float(pre[7]), float(pre[8]), float(pre[9])
        parts = []
        for start in range(1, points + 1, MAX_POINTS):
            self.write(f":WAV:STAR {start}")
            self.write(f":WAV:STOP {min(start + MAX_POINTS - 1, points)}")
            parts.append(self.query_block(":WAV:DATA?"))
        d = np.frombuffer(b"".join(parts), dtype=np.uint8)
        # Volts per sample as in the programming guide: (byte - YORigin - YREFerence) * YINCrement
        return d, xinc, lambda b: (b.astype(np.float64) - yorig - yref) * yinc

    def pwid(self, ch):
        """Positive pulse width as measured by the scope itself."""
        v = float(self.query(f":MEAS:ITEM? PWID,CHAN{ch}"))
        return v if v < 1e30 else float("nan")


class Board:
    def __init__(self, snr):
        self.base = ["nrfjprog", "--family", "NRF91"] + (["--snr", snr] if snr else [])

    def _run(self, *args):
        return subprocess.run(self.base + list(args), capture_output=True, text=True,
                              timeout=120, check=True).stdout

    def reset(self, mode):
        self._run("--" + mode)

    def program(self, image):
        self._run("--program", str(image), "--chiperase")

    def read_word(self, addr):
        # "0x00FF8128: FFFFFFFF   |....|"
        return int(self._run("--memrd", hex(addr)).split()[1], 16)


def levels(d):
    """Logic level of each sample: high above the middle between the low and
    the high level."""
    lo, hi = np.percentile(d, [1, 99])
    if hi - lo < 5:
        return None
    return (d > (lo + hi) / 2).astype(np.int8)


def edges(s, k):
    """Indices where s changes level, ignoring runs shorter than k samples
    (noise and ringing at an edge)."""
    t = np.flatnonzero(np.diff(s)) + 1
    bounds = [0, *t, len(s)]
    cur, out = s[0], []
    for start, end in zip(bounds[1:-1], bounds[2:]):
        if end - start < k and end != len(s):
            continue
        if s[start] != cur:
            out.append(start)
            cur = s[start]
    return np.array(out, dtype=np.int64)


def analyse(d, xinc, volts):
    s = levels(d)
    if s is None:
        return {"status": "flat"}
    if s[0]:
        return {"status": "starts-high"}
    e = edges(s, max(2, round(1e-6 / xinc)))
    rises, falls = e[0::2], e[1::2]
    if len(rises) == 0:
        return {"status": "no-rise"}
    r = rises[0]
    falls = falls[falls > r]
    if len(falls) == 0:
        return {"status": "no-fall"}
    f = falls[0]
    # Levels away from the edges: high inside the pulse, low before it.
    m = round(5e-6 / xinc)
    high, low = volts(d[r + m:f - m]), volts(d[:max(r - m, 1)])
    return {"status": "ok", "_rise": r, "_fall": f,
            "width_us": round((f - r) * xinc * 1e6, 2),
            "t_rise_us": round(r * xinc * 1e6, 2),    # from the start of the capture
            "pulses": len(rises),
            "high_v": round(high.mean(), 3), "low_v": round(low.mean(), 3),
            "high_noise_mv": round(high.std() * 1e3, 1),
            "low_noise_mv": round(low.std() * 1e3, 1)}


def digital(scope, ch, level):
    """Logic level of every sample of a channel, at a fixed threshold in V."""
    d, xinc, volts = scope.read_raw(ch)
    return (volts(d) > level).astype(np.int8)


def phases(s, r, f, kind):
    """Durations of the BL1 phases between the edges of the phase pin."""
    names = PHASES[kind]
    e = edges(s, 2)
    e = e[(e > r) & (e < f)]
    if len(e) != len(names) - 1:
        return None
    b = [r, *e, f]
    return {f"{n}_us": (b[i + 1] - b[i]) for i, n in enumerate(names)}


def measure(scope, board, args, kind, mode):
    scope.arm()
    board.reset(mode)
    if not scope.wait_stop(args.trigger_timeout):
        return {"status": "no-trigger"}
    d, xinc, volts = scope.read_raw(args.channel)
    res = analyse(d, xinc, volts)
    res["xinc_ns"] = round(xinc * 1e9, 3)
    res["scope_pwid_us"] = round(scope.pwid(args.channel) * 1e6, 2)
    if res["status"] != "ok":
        return res
    r, f = res.pop("_rise"), res.pop("_fall")
    us = lambda n: round(n * xinc * 1e6, 2)
    if args.phase_channel:
        ph = phases(digital(scope, args.phase_channel, args.level), r, f, kind)
        if ph is None:
            res["status"] = "phase-edges"
        else:
            res.update({k: us(v) for k, v in ph.items()})
    if args.stage_channel:
        # BL1 ends, riot-tee starts, riot-tee calls RIOT, RIOT reaches main()
        e = edges(digital(scope, args.stage_channel, args.level), 2)
        b = [f, *e[e > f][:len(STAGES)]]
        res.update({n: us(x1 - x0) for n, x0, x1 in zip(STAGES, b, b[1:])})
    if args.clock_channel:
        e = edges(digital(scope, args.clock_channel, args.level), 2)
        e = e[(e >= r) & (e <= f)]
        if len(e) >= 3:
            half = (e[-1] - e[0]) * xinc / (len(e) - 1)
            res["hfclk_mhz"] = round(64 * CLOCK_HALF_PERIOD / half, 4)
    return res


def measure_markers(scope, board, args, kind, mode):
    """All markers on one pin: BL1 toggles it at its start, after every phase
    and at its end, riot-tee after every start-up step, RIOT in main()."""
    scope.arm()
    board.reset(mode)
    if not scope.wait_stop(args.trigger_timeout):
        return {"status": "no-trigger"}
    d, xinc, volts = scope.read_raw(args.marker_channel)
    v = volts(d)
    s = (v > args.level).astype(np.int8)
    e = edges(s, max(2, round(0.2e-6 / xinc)))
    # A soft reset leaves the pin high if the previous boot ended high, so it
    # falls once before BL1 starts; BL1's first edge is the first rising one.
    e = e[np.argmax(s[e] == 1):] if len(e) else e
    nb = len(PHASES[kind]) + 1                  # edges of BL1: start, phases, end
    res = {"xinc_ns": round(xinc * 1e9, 3), "edges": len(e),
           "high_v": round(v[s == 1].mean(), 3), "low_v": round(v[s == 0].mean(), 3)}
    if len(e) != nb + len(STAGES):
        res["status"] = "marker-edges"
        args.raw_dir.mkdir(exist_ok=True)
        np.savez_compressed(args.raw_dir / f"{args.tag}.npz", d=d, xinc=xinc)
        return res
    us = lambda n: round(n * xinc * 1e6, 2)
    r, f = e[0], e[nb - 1]
    res.update(status="ok", width_us=us(f - r), t_rise_us=us(r), pulses=1)
    res.update({f"{n}_us": us(b - a) for n, a, b in zip(PHASES[kind], e[:nb], e[1:nb])})
    res.update({n: us(b - a) for n, a, b in zip(STAGES, e[nb - 1:], e[nb:])})
    if args.clock_channel:
        c = edges(digital(scope, args.clock_channel, args.level), 2)
        c = c[(c >= r) & (c <= f)]
        if len(c) >= 3:
            half = (c[-1] - c[0]) * xinc / (len(c) - 1)
            res["hfclk_mhz"] = round(64 * CLOCK_HALF_PERIOD / half, 4)
    return res


def boot(scope, board, args, kind, hexfile, mode):
    """One boot of the given kind. The marker in OTP[8] shows which path BL1
    takes: erased before a provisioning boot, set before a normal boot, and
    set after both."""
    if kind == "provisioning":
        board.program(hexfile)
    before = board.read_word(OTP8)
    if kind == "provisioning" and before != ERASED:
        return {"status": "not-erased", "marker_before": f"{before:08X}"}
    if kind == "normal" and before != MAGIC:
        return {"status": "not-provisioned", "marker_before": f"{before:08X}"}
    res = (measure_markers if args.marker_channel else measure)(scope, board, args, kind, mode)
    after = board.read_word(OTP8)
    res["marker_before"] = f"{before:08X}"
    res["marker_after"] = f"{after:08X}"
    if res["status"] == "ok" and after != MAGIC:
        res["status"] = "not-provisioned"
    return res


def prepare_image(src, outdir):
    shutil.copy(src, outdir / "image.bin")
    hexfile = outdir / "image.hex"
    subprocess.run(["arm-none-eabi-objcopy", "-I", "binary", "-O", "ihex",
                    str(outdir / "image.bin"), str(hexfile)], check=True)
    return hexfile, hashlib.sha256((outdir / "image.bin").read_bytes()).hexdigest()


def snapshot(path, only=()):
    """Uncommitted changes of the repository at path (limited to the paths in
    only, if given), untracked files included."""
    def git(*a):
        return subprocess.run(["git", *a], cwd=path, capture_output=True, text=True).stdout
    out = git("diff", "HEAD", "--", *only)
    for f in git("ls-files", "--others", "--exclude-standard", "--", *only).split():
        fp = Path(path, f)
        if f.startswith(("eval/", ".vscode/")) or fp.is_symlink() or not fp.is_file() \
                or fp.stat().st_size > 100_000:
            continue
        out += f"\n=== untracked {f}\n" + fp.read_text(errors="replace")
    return out


def tool_versions():
    def first(*cmd):
        r = subprocess.run(cmd, capture_output=True, text=True)
        return (r.stdout or r.stderr).splitlines()[0] if (r.stdout or r.stderr) else "?"
    return {"arm-none-eabi-gcc": first("arm-none-eabi-gcc", "--version"),
            "cmake": first("cmake", "--version"), "nrfjprog": first("nrfjprog", "--version")}


def git_state(path):
    """Commit and modified tracked files of the repository that holds path."""
    def git(*a):
        return subprocess.run(["git", *a], cwd=path, capture_output=True, text=True).stdout.strip()
    head = git("rev-parse", "HEAD") or "no git repository"
    modified = git("status", "--porcelain", "--untracked-files=no").replace("\n", ", ")
    return f"{git('rev-parse', '--show-toplevel')} {git('branch', '--show-current')} {head}" + \
        (f" modified: {modified}" if modified else "")


def run(args):
    outdir = HERE / "runs" / time.strftime("%Y%m%d-%H%M%S")
    outdir.mkdir(parents=True)
    hexfile, sha = prepare_image(args.image, outdir)
    scope = Scope(args.scope)
    board = Board(args.snr)
    if args.marker_channel:
        args.channel = args.marker_channel      # trigger on BL1's first edge
    args.raw_dir = outdir / "raw"
    channels = sorted({args.channel, args.phase_channel, args.stage_channel,
                       args.clock_channel} - {None})
    settings = scope.setup(channels, args.channel, args.timebase, args.level, args.mdepth,
                           args.probe, args.pre_divs)
    with open(outdir / "meta.txt", "w") as f:
        f.write(f"command: {' '.join(sys.argv)}\nimage: {args.image}\nsha256: {sha}\n")
        f.write(f"riot-tee-bl: {git_state(HERE)}\nRIOT: {git_state(args.image.parent)}\n")
        for k, v in settings.items():
            f.write(f"{k} {v}\n")
        f.write(f"riot-tee: {git_state(args.tee_src)}\n")
        for k, v in tool_versions().items():
            f.write(f"{k}: {v}\n")
    # Uncommitted changes and build flags, so that the measured build can be restored
    riot = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=args.image.parent,
                          capture_output=True, text=True).stdout.strip()
    for name, path, only in [("riot-tee-bl", HERE / "../..", ("src", "CMakeLists.txt")),
                             ("riot-tee", args.tee_src, ()), ("RIOT", riot, ())]:
        (outdir / f"{name}.diff").write_text(snapshot(path, only))
    flags = outdir / "build-flags"
    flags.mkdir()
    for pkg in ("riot-tee-bl", "riot-tee"):
        src = args.image.parent / f"pkg-build/{pkg}/CMakeFiles/{pkg}.elf.dir/flags.make"
        if src.exists():
            shutil.copy(src, flags / f"{pkg}.flags.make")
    print(f"writing {outdir}", flush=True)

    failures = 0
    with open(outdir / "results.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS, restval="")
        w.writeheader()
        for rnd in range(args.rounds):
            for n in range(1 if args.no_provision else 0, args.boots + 1):
                kind = "provisioning" if n == 0 else "normal"
                mode = args.reset_mode
                if mode == "alternate":
                    mode = "pinreset" if (rnd + n) % 2 else "reset"
                args.tag = f"round{rnd}-boot{n}"
                try:
                    res = boot(scope, board, args, kind, hexfile, mode)
                except (subprocess.SubprocessError, RuntimeError, OSError) as e:
                    res = {"status": f"error: {type(e).__name__}"}
                res.update(time=time.strftime("%H:%M:%S"), round=rnd, boot=n, kind=kind,
                           reset_mode=mode)
                w.writerow(res)
                f.flush()
                print(f"{res['time']} round {rnd} boot {n} {kind:12} {res['status']:10} "
                      f"{res.get('width_us', float('nan')):10.2f} us", flush=True)
                failures = 0 if res["status"] == "ok" else failures + 1
                if failures >= 3:
                    sys.exit("3 failed boots in a row, stopping")
                if n == 0 and res.get("marker_after") != f"{MAGIC:08X}":
                    break   # without a completed provisioning the normal boots halt


def summary(args):
    rows = list(csv.DictReader(open(args.csv)))
    print(f"{'kind':12} {'n':>5} {'failed':>6} {'mean us':>10} {'std us':>8} "
          f"{'min us':>10} {'median us':>10} {'max us':>10}")
    for kind in ["provisioning", "normal"]:
        sel = [r for r in rows if r["kind"] == kind]
        ok = np.array([float(r["width_us"]) for r in sel if r["status"] == "ok"])
        if len(ok) == 0:
            print(f"{kind:12} {0:5} {len(sel):6}")
            continue
        print(f"{kind:12} {len(ok):5} {len(sel) - len(ok):6} {ok.mean():10.2f} "
              f"{ok.std(ddof=1) if len(ok) > 1 else 0:8.2f} {ok.min():10.2f} "
              f"{np.median(ok):10.2f} {ok.max():10.2f}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--rounds", type=int, default=100)
    r.add_argument("--boots", type=int, default=10, help="normal boots per round")
    r.add_argument("--no-provision", action="store_true",
                   help="skip the chip erase, measure normal boots of the image on the board")
    r.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    r.add_argument("--scope", default="169.254.215.74")
    r.add_argument("--channel", type=int, default=3)
    r.add_argument("--level", type=float, default=0.9, help="trigger level in V")
    r.add_argument("--timebase", type=float, default=0.01,
                   help="s/div, 12 divisions; the scope only takes 1-2-5 steps")
    r.add_argument("--pre-divs", type=float, default=1.0,
                   help="divisions before the trigger point")
    r.add_argument("--mdepth", default="1200000")
    r.add_argument("--trigger-timeout", type=float, default=3.0)
    r.add_argument("--reset-mode", default="reset",
                   choices=["reset", "pinreset", "debugreset", "alternate"],
                   help="alternate: pin reset when round + boot is odd, else reset")
    r.add_argument("--marker-channel", type=int,
                   help="one channel for all markers (BL1, riot-tee, RIOT on BL_TIMING_PHASE_PIN)")
    r.add_argument("--phase-channel", type=int, help="channel of BL_TIMING_PHASE_PIN")
    r.add_argument("--stage-channel", type=int, help="channel of the stage pin (riot-tee, RIOT)")
    r.add_argument("--tee-src", type=Path, default=Path.home() / "work/riot-tee-eval",
                   help="riot-tee source tree that the image was built from")
    r.add_argument("--clock-channel", type=int, help="channel of BL_TIMING_CLOCK_PIN")
    r.add_argument("--probe", type=float, default=1, help="probe attenuation of all channels")
    r.add_argument("--snr")
    s = sub.add_parser("summary")
    s.add_argument("csv", type=Path)
    args = p.parse_args()
    run(args) if args.cmd == "run" else summary(args)


if __name__ == "__main__":
    main()

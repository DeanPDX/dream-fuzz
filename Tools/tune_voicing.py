#!/usr/bin/env python3
"""Fit Dream Fuzz voicing constants (df::Tuning) against reference recordings.

Renders the dry DI track through the real plugin processor (DreamFuzzRender)
with candidate tuning overrides and minimizes, via CMA-ES, the mismatch
against the reference-fuzz recordings:

    banded LTAS shape RMSE  (60 Hz - 12 kHz, log bands, level-normalized)
  + active-RMS level error
  + envelope-dB RMSE        (10 ms frames, captures compression/sustain/gate)
  + crest-factor error

summed over every (tone, fuzz) knob setting that has a reference track.

Requires: numpy scipy soundfile cma. References must be WAV at the same
sample rate as the dry track (decode mp3 with e.g.
`afconvert -f WAVE -d LEF32@44100 in.mp3 out.wav`).

Example (the fit that produced the current df::Tuning defaults):

  python3 Tools/tune_voicing.py \
      --render build/DreamFuzzRender_artefacts/Release/DreamFuzzRender \
      --dry dry.wav --ref 5:5:ref50.wav --ref 10:10:ref100.wav \
      --out best.json

Resume/refine with --resume best.json --sigma 0.08. Bake the result by
copying the values into df::Tuning in Source/FuzzDSP.h (round them; verify
the objective survives with --evaluate best.json).
"""
import argparse
import json
import os
import subprocess
import tempfile
import numpy as np
import soundfile as sf
from multiprocessing import Pool
from scipy import signal

# name, lo, hi, default, log-scale? — bounds are the search box for CMA-ES.
PARAMS = [
    ("gainAMinDb",    2.0,    14.0,   5.8,    False),
    ("gainAMaxDb",    12.0,   36.0,   29.3,   False),
    ("gainBMinDb",    6.0,    20.0,   15.8,   False),
    ("gainBMaxDb",    26.0,   54.0,   44.1,   False),
    ("fbAmtMin",      0.0,    0.15,   0.0007, False),
    ("fbAmtMax",      0.0,    0.10,   0.0,    False),
    ("sagAmtMin",     0.0,    0.25,   0.005,  False),
    ("sagAmtMax",     0.15,   0.90,   0.61,   False),
    ("makeupMinDb",   -24.0,  -2.0,   -10.0,  False),
    ("makeupMaxDb",   -28.0,  -4.0,   -18.4,  False),
    ("miller1MaxHz",  6000.0, 16000.0, 13250.0, True),
    ("miller1MinHz",  2500.0, 9000.0, 7250.0, True),
    ("miller2MaxHz",  5000.0, 14000.0, 9600.0, True),
    ("miller2MinHz",  1500.0, 8000.0, 7200.0, True),
    ("coupleHz",      5.0,    100.0,  12.5,   True),
    ("inputHPHz",     5.0,    60.0,   10.5,   True),
    ("loadLPHz",      1500.0, 12000.0, 7800.0, True),
    ("outputHPHz",    5.0,    80.0,   15.5,   True),
    ("tiltHz",        300.0,  8000.0, 4350.0, True),
    ("tiltLowFullDb", -14.0,  0.0,    -0.5,   False),
    ("tiltHighFullDb", 0.0,   22.0,   15.9,   False),
    ("fizzHz",        4000.0, 16000.0, 15000.0, True),
    ("fizzQ",         0.5,    2.8,    2.0,    False),
    ("bias1",         0.0,    0.20,   0.005,  False),
    ("bias2",         0.04,   0.30,   0.262,  False),
    ("kPos1",         0.4,    1.6,    0.91,   False),
    ("kNeg1",         0.15,   1.2,    0.76,   False),
    ("kPos2",         0.15,   1.5,    0.48,   False),
    ("kNeg2",         0.05,   1.2,    0.22,   False),
    ("sagRelMs",      30.0,   300.0,  77.0,   True),
]
NAMES = [p[0] for p in PARAMS]
W_SHAPE, W_LEVEL, W_ENV, W_CREST = 1.0, 0.6, 0.8, 0.25

CFG = {}   # filled in each process: render path, dry path, refs, fs


def to_x(values):
    out = []
    for (n, lo, hi, d, lg), v in zip(PARAMS, values):
        out.append(np.log(v / lo) / np.log(hi / lo) if lg and lo > 0
                   else (v - lo) / (hi - lo))
    return np.array(out)


def to_phys(x):
    out = {}
    for (n, lo, hi, d, lg), v in zip(PARAMS, np.clip(x, 0, 1)):
        out[n] = float(lo * (hi / lo) ** v) if lg and lo > 0 else float(lo + (hi - lo) * v)
    return out


def band_ltas(x, fs, fmin=60.0, fmax=12000.0, nbands=48, nfft=8192):
    f, p = signal.welch(x, fs, nperseg=nfft, noverlap=nfft // 2, window="hann")
    edges = np.geomspace(fmin, fmax, nbands + 1)
    out = np.empty(nbands)
    for i in range(nbands):
        m = (f >= edges[i]) & (f < edges[i + 1])
        out[i] = 10 * np.log10(p[m].mean() + 1e-18)
    return out


def envelope_db(x, fs, hop_ms=10):
    n = int(hop_ms * 1e-3 * fs)
    nf = len(x) // n
    fr = x[: nf * n].reshape(nf, n)
    return 10 * np.log10(np.mean(fr**2, axis=1) + 1e-12)


def metrics(x, fs):
    env = envelope_db(x, fs)
    act = env > env.max() - 45.0
    arms = 10 * np.log10(np.mean(10 ** (env[act] / 10)))
    n = int(0.05 * fs)
    nf = len(x) // n
    fr = x[: nf * n].reshape(nf, n)
    fdb = 10 * np.log10(np.mean(fr**2, axis=1) + 1e-12)
    a2 = fdb > fdb.max() - 45
    crest = 20 * np.log10(np.abs(fr[a2]).max() / (np.sqrt(np.mean(fr[a2] ** 2)) + 1e-12))
    return band_ltas(x, fs), env, act, arms, crest


def init_worker(cfg):
    CFG.update(cfg)
    CFG["ref_metrics"] = {}
    for tag, (tone, fuzz, path) in CFG["refs"].items():
        w, fs = sf.read(path, always_2d=True)
        CFG["fs"] = fs
        CFG["ref_metrics"][tag] = metrics(w.mean(axis=1), fs)


def eval_setting(args):
    tag, overrides, uid = args
    tone, fuzz, _ = CFG["refs"][tag]
    out = os.path.join(CFG["tmp"], f"r{uid}_{tag}.wav")
    cmd = [CFG["render"], CFG["dry"], out, str(tone), "0", str(fuzz)]
    cmd += [f"{k}={v:.6g}" for k, v in overrides.items()]
    try:
        subprocess.run(cmd, capture_output=True, timeout=120, check=True)
        x, _ = sf.read(out, always_2d=True)
    except Exception:
        return 1e3
    finally:
        try:
            os.remove(out)
        except OSError:
            pass
    x = x.mean(axis=1)
    if not np.all(np.isfinite(x)) or np.abs(x).max() > 8:
        return 1e3

    fs = CFG["fs"]
    lt, env, act, arms, crest = metrics(x, fs)
    rlt, renv, ract, rarms, rcrest = CFG["ref_metrics"][tag]

    shape = np.sqrt(np.mean(((lt - lt.mean()) - (rlt - rlt.mean())) ** 2))
    m = ract[: min(len(act), len(ract))]
    e_o = env[: len(m)][m] - arms
    e_r = renv[: len(m)][m] - rarms
    envrmse = np.sqrt(np.mean((e_o - e_r) ** 2))
    return (W_SHAPE * shape + W_LEVEL * abs(arms - rarms)
            + W_ENV * envrmse + W_CREST * abs(crest - rcrest))


def objective_batch(xs, pool, gen):
    tags = list(CFG["refs"])
    jobs = [(tag, to_phys(x), f"g{gen}_i{i}") for i, x in enumerate(xs) for tag in tags]
    flat = pool.map(eval_setting, jobs)
    k = len(tags)
    return [sum(flat[i * k : (i + 1) * k]) for i in range(len(xs))]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--render", required=True, help="DreamFuzzRender binary")
    ap.add_argument("--dry", required=True, help="dry DI wav")
    ap.add_argument("--ref", action="append", required=True, metavar="TONE:FUZZ:WAV",
                    help="reference track at knob setting, repeatable (level assumed 0 dB)")
    ap.add_argument("--out", default="best.json")
    ap.add_argument("--resume", help="best.json to continue from")
    ap.add_argument("--evaluate", help="just evaluate a best.json (no optimization)")
    ap.add_argument("--sigma", type=float, default=0.18)
    ap.add_argument("--maxiter", type=int, default=120)
    ap.add_argument("--popsize", type=int, default=20)
    ap.add_argument("--workers", type=int, default=8)
    args = ap.parse_args()

    refs = {}
    for i, spec in enumerate(args.ref):
        tone, fuzz, path = spec.split(":", 2)
        refs[f"s{i}_{tone}_{fuzz}"] = (float(tone), float(fuzz), path)

    cfg = dict(render=os.path.abspath(args.render), dry=os.path.abspath(args.dry),
               refs=refs, tmp=tempfile.mkdtemp(prefix="dreamfuzz_tune_"))
    init_worker(cfg)

    if args.evaluate:
        with open(args.evaluate) as f:
            params = json.load(f)["params"]
        with Pool(args.workers, initializer=init_worker, initargs=(cfg,)) as pool:
            f0 = objective_batch([to_x([params[n] for n in NAMES])], pool, "eval")[0]
        print(f"objective: {f0:.3f}")
        return

    if args.resume:
        with open(args.resume) as f:
            prev = json.load(f)
        x0, f_best = to_x([prev["params"][n] for n in NAMES]), prev["objective"]
    else:
        x0, f_best = to_x([p[3] for p in PARAMS]), None

    import cma
    es = cma.CMAEvolutionStrategy(
        x0, args.sigma,
        {"bounds": [0.0, 1.0], "popsize": args.popsize, "maxiter": args.maxiter, "verbose": -1})

    with Pool(args.workers, initializer=init_worker, initargs=(cfg,)) as pool:
        if f_best is None:
            f_best = objective_batch([x0], pool, "base")[0]
        best = (f_best, to_phys(x0))
        print(f"start objective: {best[0]:.3f}", flush=True)

        gen = 0
        while not es.stop():
            xs = es.ask()
            fs_ = objective_batch(xs, pool, gen)
            es.tell(xs, fs_)
            i = int(np.argmin(fs_))
            if fs_[i] < best[0]:
                best = (fs_[i], to_phys(xs[i]))
                with open(args.out, "w") as f:
                    json.dump({"objective": best[0], "params": best[1]}, f, indent=2)
            gen += 1
            if gen % 5 == 0:
                print(f"gen {gen:3d}: best {best[0]:.3f}  sigma {es.sigma:.3f}", flush=True)

    print(f"final best {best[0]:.3f} -> {args.out}")


if __name__ == "__main__":
    main()

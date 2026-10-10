#!/usr/bin/env python3
"""Reproduce focused OFFLINE gate diagnosis; never controls a host or a player.

Inputs are the preserved 1004/1005/1006 canonical stereo PCM16 captures.
Full paired captures are fed to retain gate history, while traces are bounded.
Synthetic background controls are deterministic independent noise, not gameplay.
"""
import argparse
import array
import csv
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import wave


def canonical(path):
    with path.open("rb") as f:
        h = f.read(44)
    assert len(h) == 44 and h[:4] == b"RIFF" and h[8:16] == b"WAVEfmt "
    assert struct.unpack_from("<IHHIIHH", h, 16) == (16, 1, 2, 48000, 192000, 4, 16)
    assert h[36:40] == b"data"
    assert struct.unpack_from("<I", h, 40)[0] == path.stat().st_size - 44
    assert struct.unpack_from("<I", h, 4)[0] == path.stat().st_size - 8


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write(path, data):
    with wave.open(str(path), "wb") as w:
        w.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
        w.writeframes(data)
    canonical(path)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--sim", type=Path, required=True)
    p.add_argument("--evidence", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--only", choices=("truth", "pairs", "all"), default="all")
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PGATE_", "SIM_", "TRUTH_"))}
    manifest = {"scope": "offline only; no live quality claim", "sim_sha256": digest(args.sim),
                "inputs": {}, "runs": []}
    for day in ("1004", "1005", "1006"):
        for role in ("self", "peer"):
            path = args.evidence / "inputs" / f"{day}-{role}.wav"
            canonical(path)
            manifest["inputs"][str(path)] = digest(path)

    def run(argv, overrides=None):
        label = Path(argv[4] if argv[0] == "real-trace" else argv[6]).name
        log = args.out / f"{label}.log"
        command = [str(args.sim.resolve()), *map(str, argv)]
        print("RUN", label, flush=True)
        with log.open("w") as f:
            subprocess.run(command, env={**env, **(overrides or {})}, stdout=f, stderr=subprocess.STDOUT, check=True)
        manifest["runs"].append({"command": command, "environment_overrides": overrides or {}, "log": str(log)})
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    if args.only in ("truth", "all"):
        voice = args.out / "voice.wav"
        with wave.open(str(args.evidence / "inputs/1004-self.wav"), "rb") as w:
            w.setpos(int(32.5 * 48000))
            write(voice, w.readframes(32 * 48000))
        backgrounds = []
        for seed in (2701, 2702):
            path = args.out / f"independent-background-{seed}.wav"
            rng = random.Random(seed)
            samples = array.array("h", (rng.randrange(-300, 301) for _ in range(45 * 48000 * 2)))
            if sys.byteorder != "little":
                samples.byteswap()
            write(path, samples.tobytes())
            backgrounds.append(path)
        manifest["synthetic_controls"] = {"voice_source_start_s": 32.5, "voice_length_s": 32,
            "background_duration_s": 45, "background_seeds": [2701, 2702],
            "background_distribution": "independent uniform PCM integer [-300,300] per channel/sample",
            "derived_sha256": {str(path): digest(path) for path in [voice, *backgrounds]}}
        # Same deterministic event schedule, each offset once: only voice class changes.
        for cls in ("voice", "whisper"):
            run(["truth-unseen", voice, ",".join(map(str, backgrounds)), "0", "0", "45", args.out / f"unseen-{cls}"],
                {"TRUTH_CLASS": cls, "TRUTH_LIST": "1"})
        # Preserve and exercise the legacy voiced-training case for comparison, explicitly labelled.
        run(["truth", voice, ",".join(map(str, backgrounds)), "0", "0", "45", args.out / "legacy-trained-whisper"],
            {"TRUTH_CLASS": "whisper", "TRUTH_LIST": "1"})

    if args.only in ("pairs", "all"):
        for day, start, end in (("1004", 52, 64), ("1005", 265, 278), ("1006", 411, 424)):
            prefix = args.out / f"paired-{day}"
            run(["real-trace", args.evidence / "inputs" / f"{day}-self.wav",
                 args.evidence / "inputs" / f"{day}-peer.wav", args.evidence / f"sections-{day}.txt",
                 prefix, str(prefix) + ".csv", str(start), str(end)],
                {"PGATE_TRACE": f"{start},{end}"})
            with Path(str(prefix) + ".csv").open() as f:
                rows = list(csv.DictReader(f))
            assert rows and len(rows) == (end - start) * 200
            assert all(int(r["frames"]) == 240 for r in rows)
            assert all(int(r["frame"]) == start * 48000 + i * 240 for i, r in enumerate(rows))
    print(args.out / "manifest.json")


if __name__ == "__main__":
    main()

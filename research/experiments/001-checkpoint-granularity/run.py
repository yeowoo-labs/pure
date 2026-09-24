#!/usr/bin/env python3
"""Build, check, measure, and preserve raw data using only the Python stdlib."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import shlex
import statistics
import subprocess
import tempfile
from datetime import datetime, timezone


ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "benchmark.c"
FLAGS = ["-std=c17", "-O3", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]


def command(args):
    return subprocess.check_output(args, text=True).strip()


def write_csv(path, rows):
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--target-ms", type=float, default=20.0)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    if args.trials < 3 or not 1 <= args.target_ms <= 1000:
        parser.error("use at least 3 trials and a target from 1 to 1000 ms")
    if not args.check_only and args.output is None:
        parser.error("--output is required for measurements")
    cc = shlex.split(os.environ.get("CC", "cc"))
    environment = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(), "machine": platform.machine(),
        "compiler": command(cc + ["--version"]), "flags": FLAGS,
        "python": platform.python_version(),
        "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "trials": args.trials, "target_ms": args.target_ms,
        "seed": 20260922, "timing": "batch elapsed time / update ticks; not input latency",
        "memory": "sizeof engine + requested live array bytes; excludes allocator, harness, code, OS",
        "power_control": "not controlled; shared host; preliminary measurements",
    }
    # One CPU reduces migration noise. This is recorded, not assumed to remove noise.
    if hasattr(os, "sched_getaffinity"):
        affinity = sorted(os.sched_getaffinity(0))
        environment["original_affinity"] = affinity
        os.sched_setaffinity(0, {affinity[0]})
        environment["measurement_affinity"] = sorted(os.sched_getaffinity(0))
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        allowed = ("model name", "CPU implementer", "CPU architecture", "CPU part", "Model")
        environment["cpu_description"] = sorted({
            line for line in cpuinfo.read_text().splitlines()
            if line.split(":", 1)[0].strip() in allowed
        })
    with tempfile.TemporaryDirectory(prefix="pure-checkpoints-") as tmp:
        binary = Path(tmp) / "benchmark"
        subprocess.run(cc + FLAGS + [str(SOURCE), "-o", str(binary)], check=True)
        environment["release_check"] = json.loads(command([str(binary), "--check"]))
        checked = Path(tmp) / "benchmark-checked"
        check_flags = ["-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(cc + check_flags + [str(SOURCE), "-o", str(checked)], check=True)
        environment["sanitizer_check"] = json.loads(command([str(checked), "--check"]))
        print("Differential checks passed in release and ASan/UBSan builds.", flush=True)
        if args.check_only:
            print(json.dumps(environment, indent=2))
            return
        # Fail before measurements if a previous run would be overwritten.
        args.output.mkdir(parents=True, exist_ok=False)

        cases = []
        for chains, length in [(16, 256), (16, 1024)]:
            for workload in ["roots", "uniform", "tail", "dense"]:
                for block in [1, 4, 16, 64, length]:
                    cases.append((chains, length, workload, block))

        def sample(case, replays):
            chains, length, workload, block = case
            return json.loads(command([str(binary), str(chains), str(length), str(block),
                                       workload, "256", str(replays), str(environment["seed"])]))

        calibration = {}
        for case in cases:
            pilot = sample(case, 2)
            wanted = math.ceil(2 * args.target_ms * 1e6 / max(1, pilot["elapsed_ns"]))
            # Even replay counts leave the same final model in every strategy.
            calibration[case] = min(2048, max(2, wanted + wanted % 2))
        environment["calibration_replays"] = [
            {"chains": c[0], "length": c[1], "workload": c[2], "block": c[3], "replays": r}
            for c, r in calibration.items()
        ]
        rows = []
        checksums = {}
        rng = random.Random(81173)
        for trial in range(args.trials):
            order = cases.copy()
            rng.shuffle(order)
            for case in order:
                result = sample(case, calibration[case])
                chains, length, workload, block = case
                key = (chains, length, workload)
                if checksums.setdefault(key, result["checksum"]) != result["checksum"]:
                    raise RuntimeError(f"cross-strategy output mismatch: {case}")
                row = {"trial": trial, "chains": chains, "length": length,
                       "workload": workload, "block": block, "replays": calibration[case],
                       **result}
                row["ns_per_tick"] = result["elapsed_ns"] / result["ticks"]
                row["evaluations_per_tick"] = result["node_evaluations"] / result["ticks"]
                rows.append(row)
            write_csv(args.output / "raw.csv", rows)
            print(f"Completed trial {trial + 1}/{args.trials}.", flush=True)
        summary = []
        for case in cases:
            chains, length, workload, block = case
            group = [r for r in rows if (r["chains"], r["length"], r["workload"], r["block"]) == case]
            times = [r["ns_per_tick"] for r in group]
            summary.append({"chains": chains, "length": length, "workload": workload, "block": block,
                            "median_ns_per_tick": statistics.median(times),
                            "min_ns_per_tick": min(times), "max_ns_per_tick": max(times),
                            "core_owned_bytes": group[0]["core_owned_bytes"],
                            "cache_bytes": group[0]["cache_bytes"],
                            "evaluations_per_tick": group[0]["evaluations_per_tick"]})
        write_csv(args.output / "summary.csv", summary)
        environment["completed_utc"] = datetime.now(timezone.utc).isoformat()
        environment["binary_bytes"] = binary.stat().st_size
        (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
        print(f"Saved raw data, summary, and environment to {args.output}")


if __name__ == "__main__":
    main()

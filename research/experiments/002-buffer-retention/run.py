#!/usr/bin/env python3
"""Build, independently verify, and measure the retention experiment."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shlex
import signal
import statistics
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent
FLAGS = ["-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O2", "-g"]


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def build(cc, output, sanitizer=False):
    command = [*cc, *FLAGS]
    if sanitizer:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += [str(ROOT / "benchmark.c"), "-o", str(output)]
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return {"command": command, "stderr": result.stderr, "sha256": digest(output),
            "size_bytes": output.stat().st_size}


def shape_for(shape, rng, i):
    if shape == "sparse":
        return 65536, 64
    if shape == "dense":
        return 65536, 49152
    if shape == "mixed":
        source = rng.choice([4096, 16384, 65536, 262144])
        keep = max(16, source // rng.choice([1, 2, 4, 8, 64, 1024]))
        return source, keep
    if shape == "test":
        return [(1, 1), (63, 17), (4097, 12), (65536, 49152), (127, 127)][i % 5]
    raise ValueError(shape)


def fixture(path, shape, count, seed):
    rng = random.Random(seed)
    expected = hashlib.sha256()
    total = 0
    with path.open("wb") as stream:
        for i in range(count):
            size, keep = shape_for(shape, rng, i)
            offset = rng.randrange(size - keep + 1)
            # All input bytes exist on disk; no sparse files or seeking past data.
            tile = rng.randbytes(256)
            payload = (tile * ((size + 255) // 256))[:size]
            stream.write(struct.pack("<III", size, offset, keep))
            stream.write(payload)
            out = struct.pack("<I", keep) + payload[offset:offset + keep]
            expected.update(out)
            total += len(out)
    return {"path": path.name, "shape": shape, "records": count, "seed": seed,
            "input_bytes": path.stat().st_size, "input_sha256": digest(path),
            "expected_output_bytes": total, "expected_output_sha256": expected.hexdigest()}


def reference(data):
    result = bytearray()
    at = 0
    while at < len(data):
        if len(data) - at < 12:
            raise ValueError("short header")
        size, offset, keep = struct.unpack_from("<III", data, at)
        at += 12
        if not 0 < size <= 8 * 1024 * 1024 or not 0 < keep <= size or offset > size - keep:
            raise ValueError("invalid frame")
        if len(data) - at < size:
            raise ValueError("short payload")
        result.extend(struct.pack("<I", keep))
        result.extend(data[at + offset:at + offset + keep])
        at += size
    return bytes(result)


def command(binary, case, policy, *extra):
    args = [str(binary), "--policy", policy["policy"], "--pool", str(policy.get("pool", 1)),
            "--ratio", str(policy.get("ratio", 4)), "--budget", str(case.get("budget", 8388608)),
            "--slots", str(case.get("slots", 512)), "--hold-us", str(case.get("hold_us", 0)),
            "--service-us", str(case.get("service_us", 0))]
    return args + list(extra)


def metrics(stderr):
    # Sanitizer diagnostics must not be silently ignored.
    text = stderr.decode().strip()
    try:
        result = json.loads(text)
    except json.JSONDecodeError as exc:
        raise AssertionError(f"Expected metrics only; tool/runtime diagnostics:\n{text}") from exc
    assert result["unreleased_buffer_bytes"] == 0, result
    assert result["peak_active_bytes"] <= result["peak_reserved_bytes"] <= result["budget_bytes"], result
    return result


def check(binary, tmp, policies):
    checks = []
    test = tmp / "test.frames"
    fixture(test, "test", 45, 177)
    data = test.read_bytes()
    expected = reference(data)
    for policy in policies:
        for slow in [False, True]:
            case = {"slots": 7, "hold_us": 100 if slow else 0, "budget": 300000}
            result = subprocess.run(command(binary, case, policy), input=data, capture_output=True, timeout=15)
            m = metrics(result.stderr)
            assert result.returncode == 0 and m["status"] == "ok", (policy, result.stderr)
            assert result.stdout == expected and m["emitted"] == 45
            assert m["input_bytes"] == len(data) and m["output_bytes"] == len(expected)
            assert m["live_at_stop_bytes"] == 0
            checks.append(f'{policy["name"]}: byte equality, accounting, cleanup, slow={slow}')
        # Truncation at both header and payload boundaries, including exact missing payload.
        bad_inputs = [b"x", struct.pack("<III", 12, 0, 4),
                      struct.pack("<III", 12, 0, 4) + b"123",
                      struct.pack("<III", 12, 11, 4) + b"x" * 12,
                      struct.pack("<III", 0, 0, 0),
                      struct.pack("<III", 8 * 1024 * 1024 + 1, 0, 1)]
        for bad in bad_inputs:
            result = subprocess.run(command(binary, {"hold_us": 1000}, policy),
                                    input=data[:13] + bad, capture_output=True, timeout=15)
            m = metrics(result.stderr)
            assert result.returncode == 2 and m["status"] in {"invalid_frame", "truncated_input"}
        checks.append(f'{policy["name"]}: six malformed inputs with pending buffer cleanup')
        result = subprocess.run(command(binary, {"hold_us": 1000}, policy, "--cancel-after", "3"),
                                input=data, capture_output=True, timeout=15)
        m = metrics(result.stderr)
        prefix_end = 0
        for _ in range(3):
            prefix_end += 4 + struct.unpack_from("<I", expected, prefix_end)[0]
        assert result.returncode == 3 and m["status"] == "cancelled" and m["emitted"] == 3
        assert result.stdout == expected[:prefix_end]
        checks.append(f'{policy["name"]}: cancellation preserves completed output prefix and releases pending buffers')
        result = subprocess.run(command(binary, {}, policy), input=b"", capture_output=True, timeout=15)
        assert result.returncode == 0 and metrics(result.stderr)["emitted"] == 0
        checks.append(f'{policy["name"]}: empty stream')
    # Tight budget must refuse a whole-frame requirement, while streaming projection can work.
    large = struct.pack("<III", 1048576, 17, 64) + b"a" * 1048576
    for policy in policies:
        result = subprocess.run(command(binary, {"budget": 131072, "slots": 1}, policy),
                                input=large, capture_output=True, timeout=15)
        m = metrics(result.stderr)
        assert m["status"] == ("ok" if policy["policy"] == "project" else "budget_too_small")
    checks.append("frame larger than buffer budget: bounded refusal versus projection")
    if Path("/dev/full").exists():
        with open("/dev/full", "wb") as sink:
            result = subprocess.run(command(binary, {"hold_us": 1000}, policies[0]), input=data,
                                    stdout=sink, stderr=subprocess.PIPE, timeout=15)
        assert result.returncode == 2 and metrics(result.stderr)["status"] == "output_failed"
        checks.append("real output failure through /dev/full, pending buffers released")
    # Real signal cancellation after allocation, while waiting for the delayed sink.
    with test.open("rb") as source, open(os.devnull, "wb") as sink:
        proc = subprocess.Popen(command(binary, {"hold_us": 500000}, policies[0]),
                                stdin=source, stdout=sink, stderr=subprocess.PIPE)
        time.sleep(0.08)
        proc.send_signal(signal.SIGTERM)
        _, error = proc.communicate(timeout=5)
    assert proc.returncode == 3 and metrics(error)["status"] == "cancelled", error
    checks.append("SIGTERM interrupts delayed sink and releases buffers")
    if platform.system() == "Linux":
        # Regression: inherited rusage peak cannot stand in for current-image HWM.
        resident = tmp / "rss.frames"
        fixture(resident, "sparse", 512, 817)
        values = {}
        for policy in [{"policy": "borrow"}, {"policy": "project"}]:
            with resident.open("rb") as source, open(os.devnull, "wb") as sink:
                result = subprocess.run(command(binary, {"hold_us": 10000}, policy),
                                        stdin=source, stdout=sink, stderr=subprocess.PIPE, timeout=15)
            m = metrics(result.stderr)
            assert result.returncode == 0 and m["rss_peak_source"] == "linux_proc_vmhwm_approximate"
            assert m["rss_peak_bytes"] > 0
            values[policy["policy"]] = m["rss_peak_bytes"]
        # Only a sanity check for this deliberately wide memory gap, not exact RSS arithmetic.
        assert values["borrow"] > values["project"], values
        checks.append("current-image VmHWM distinguishes large retained sources from small projections")
    return checks


def sample(binary, source, output, case, policy):
    with source.open("rb") as inp, output.open("wb") as out:
        result = subprocess.run(command(binary, case, policy), stdin=inp, stdout=out,
                                stderr=subprocess.PIPE, timeout=30)
    m = metrics(result.stderr)
    assert result.returncode == 0 and m["status"] == "ok", result.stderr
    return m


def summarize(rows):
    result = []
    names = ["elapsed_ns", "cpu_ns", "peak_reserved_bytes", "peak_active_bytes", "rss_peak_bytes",
             "copy_bytes", "active_byte_seconds", "latency_mean_ns", "latency_p99_upper_ns",
             "budget_waits", "allocations", "reuses"]
    groups = sorted({(r["case"], r["variant"]) for r in rows})
    for case, variant in groups:
        group = [r for r in rows if (r["case"], r["variant"]) == (case, variant)]
        value = {"case": case, "variant": variant, "samples": len(group)}
        for name in names:
            values = [r[name] for r in group]
            value[name] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        result.append(value)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.check_only and args.output is None:
        parser.error("provide --check-only or --output (new directory)")
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    cc = shlex.split(os.environ.get("CC", "cc"))
    plan = json.loads((ROOT / "plan.json").read_text())
    affinity = None
    if hasattr(os, "sched_getaffinity"):
        affinity = min(os.sched_getaffinity(0))
        os.sched_setaffinity(0, {affinity})
    with tempfile.TemporaryDirectory(prefix="pure-retention-") as folder:
        tmp = Path(folder)
        native, sanitized = tmp / "bench", tmp / "bench-sanitized"
        builds = [build(cc, native), build(cc, sanitized, True)]
        verification = {"native": check(native, tmp, plan["policies"]),
                        "asan_ubsan": check(sanitized, tmp, plan["policies"])}
        print(f'Verified {sum(map(len, verification.values()))} scenario groups (native + ASan/UBSan).', flush=True)
        if args.check_only:
            return
        sources = {name: digest(ROOT / name) for name in ["benchmark.c", "run.py", "plan.json"]}
        environment = {"platform": platform.platform(), "machine": platform.machine(),
                       "python": platform.python_version(), "cpu_affinity": affinity,
                       "compiler": subprocess.check_output([*cc, "--version"], text=True),
                       "builds": builds, "source_sha256": sources,
                       "clock": "CLOCK_MONOTONIC; CPU = CLOCK_PROCESS_CPUTIME_ID",
                       "timing": "includes file IO, scheduling, deliberate sleep, metrics, buffer cleanup; excludes startup/build/fixture/verification",
                       "limitations": "shared host; unregulated thermals; cached files; output page cache, no fsync; synthetic closed-loop arrivals",
                       "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        if Path("/proc/cpuinfo").exists():
            environment["cpuinfo"] = Path("/proc/cpuinfo").read_text()
        dump(args.output / "environment.json", environment)
        dump(args.output / "verification.json", verification)
        dump(args.output / "plan.json", plan)
        (args.output / "sources").mkdir()
        for name in sources:
            (args.output / "sources" / name).write_bytes((ROOT / name).read_bytes())
        fixtures, rows = [], []
        rng = random.Random(plan["seed"])
        for case in plan["cases"]:
            source, output = tmp / "input.frames", tmp / "output.frames"
            info = fixture(source, case["shape"], case["records"], plan["seed"])
            info["case"] = case["name"]
            fixtures.append(info)
            # Warm each strategy once. The following measurements use fresh processes.
            for policy in plan["policies"]:
                sample(native, source, output, case, policy)
                assert digest(output) == info["expected_output_sha256"]
            for repeat in range(plan["repeats"]):
                order = list(plan["policies"])
                rng.shuffle(order)
                for policy in order:
                    m = sample(native, source, output, case, policy)
                    assert m["accepted"] == m["emitted"] == case["records"]
                    assert m["input_bytes"] == info["input_bytes"]
                    assert m["output_bytes"] == info["expected_output_bytes"]
                    assert digest(output) == info["expected_output_sha256"]
                    rows.append({"case": case["name"], "variant": policy["name"], "repeat": repeat, **m})
            dump(args.output / "raw.json", rows)
            dump(args.output / "fixtures.json", fixtures)
            print(f'Completed {case["name"]}: {len(plan["policies"])} variants x {plan["repeats"]} trials.', flush=True)
        dump(args.output / "summary.json", summarize(rows))
        with (args.output / "raw.csv").open("w") as out:
            writer = csv.DictWriter(out, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        # Detect accidental source edits during a measurement run.
        assert sources == {name: digest(ROOT / name) for name in sources}
        dump(args.output / "complete.json", {"samples": len(rows), "all_output_hashes_match": True,
                                             "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())})
        print(args.output.resolve())


if __name__ == "__main__":
    main()

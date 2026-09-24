#!/usr/bin/env python3
"""Verify and measure yyjson parsing with bounded, differently paced consumers."""
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
FLAGS = ["-std=c17", "-O2", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
FLAGS += ["-DYYJSON_DISABLE_WRITER=1", "-DYYJSON_DISABLE_UTILS=1", "-DYYJSON_DISABLE_INCR_READER=1"]
SOURCE_NAMES = ["benchmark.c", "run.py", "plan.json", "vendor/yyjson/UPSTREAM.json",
                "vendor/yyjson/yyjson.c", "vendor/yyjson/yyjson.h", "vendor/yyjson/LICENSE"]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def first_members(pairs):
    result = {}
    for key, value in pairs:
        result.setdefault(key, value)
    return result


def reference(data, keys):
    values = [[] for _ in keys]
    for line in data.splitlines():
        row = json.loads(line, object_pairs_hook=first_members)
        if not isinstance(row, dict):
            raise ValueError("object required")
        for k, key in enumerate(keys):
            if not isinstance(row.get(key), str):
                raise ValueError("string field required")
            values[k].append(row[key].encode("utf-8"))
    return values


def decode(data, keys, expected, partial=False):
    at = 0
    found = [[] for _ in keys]
    while at < len(data):
        if len(data) - at < 16:
            raise AssertionError("incomplete output header")
        k, length, sequence = struct.unpack_from("<IIQ", data, at)
        at += 16
        assert k < len(keys) and sequence == len(found[k])
        assert length <= len(data) - at
        found[k].append(data[at:at + length])
        at += length
    for k in range(len(keys)):
        assert found[k] == (expected[k][:len(found[k])] if partial else expected[k])
    return [len(items) for items in found]


def build(cc, output, sanitizer=False):
    command = [*cc, *FLAGS]
    if sanitizer:
        command += ["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += [str(ROOT / "benchmark.c"), str(ROOT / "vendor/yyjson/yyjson.c"), "-o", str(output)]
    start = time.monotonic_ns()
    result = subprocess.run(command, capture_output=True, text=True, check=True)
    return {"command": command, "stderr": result.stderr, "sha256": digest(output),
            "size_bytes": output.stat().st_size, "build_wall_ns": time.monotonic_ns() - start}


def command(binary, case, policy, *extra):
    keys = case.get("keys", ["body", "tag", "tag"])
    holds = case.get("hold_us", [0, 5000, 8000][:len(keys)])
    services = case.get("service_us", [0] * len(keys))
    result = [str(binary), "--policy", policy["policy"], "--fanout", str(len(keys)),
              "--slots", str(case.get("slots", 64)), "--budget", str(case.get("budget", 8388608)),
              "--pool", str(policy.get("pool", 1)), "--ratio", str(policy.get("ratio", 2))]
    for k, key in enumerate(keys):
        result += [f"--key{k}", key, f"--hold{k}-us", str(holds[k]), f"--service{k}-us", str(services[k])]
    return result + list(extra)


def metrics(stderr):
    try:
        m = json.loads(stderr)
    except (ValueError, UnicodeError) as exc:
        raise AssertionError(f"Expected metrics only, got diagnostics: {stderr!r}") from exc
    assert m["unreleased_buffer_bytes"] == 0
    assert m["peak_reserved_bytes"] <= m["budget_bytes"]
    assert sum(q["emitted"] for q in m["queues"]) == m["emitted"]
    assert m["parser_version"] == "0.12.0"
    return m


def invoke(binary, case, policy, data, *extra):
    proc = subprocess.run(command(binary, case, policy, *extra), input=data,
                          capture_output=True, timeout=20)
    return proc, metrics(proc.stderr)


def verify(binary, policies, tmp):
    checks = []
    rows = [{"body": "A" * 33000, "tag": "one"},
            {"body": "한글😀\n\x00\"\\", "tag": ""},
            {"body": "", "tag": "둘\t\r"},
            {"body": "z" * 66000, "tag": "three", "meta": [True, None, {"x": -3.5}]}] * 7
    data = b"".join(json.dumps(row, ensure_ascii=i % 2 == 0).encode() + b"\n" for i, row in enumerate(rows))
    for policy in policies:
        for keys in [["tag"], ["body", "tag"], ["body", "tag", "tag"]]:
            case = {"keys": keys, "hold_us": [500 * k for k in range(len(keys))], "slots": 3}
            proc, m = invoke(binary, case, policy, data)
            assert proc.returncode == 0 and m["status"] == "ok", proc.stderr
            counts = decode(proc.stdout, keys, reference(data, keys))
            assert counts == [len(rows)] * len(keys) and m["accepted"] == len(rows)
            assert m["live_at_stop_bytes"] == 0
            checks.append(f'{policy["name"]}: {len(keys)} consumer(s), UTF-8/escapes/NUL/empty, slot reuse')
        keys = ["body", "tag", "tag"]
        for sample in [b"", data.rstrip(b"\n"), b'{"body":"first","body":"second","tag":"x"}\n']:
            proc, m = invoke(binary, {}, policy, sample)
            assert proc.returncode == 0 and m["status"] == "ok"
            decode(proc.stdout, keys, reference(sample, keys))
        checks.append(f'{policy["name"]}: empty input, final JSON without newline, first duplicate member')
        malformed = [b'{"body":"x","tag":', b'{"body":"x","tag":"y","extra":[1,]}',
                     b'{"body":"x","tag":3}', b'{"body":"x"}', b'[]',
                     b'{"body":"\xff","tag":"x"}', b'\n',
                     b'{"body":"x","tag":"y"} true']
        for bad in malformed:
            proc, m = invoke(binary, {"hold_us": [1000, 2000, 3000]}, policy, data[:data.index(b"\n") + 1] + bad)
            assert proc.returncode == 2 and m["status"] in {"invalid_json", "invalid_schema"}, proc.stderr
        checks.append(f'{policy["name"]}: eight invalid JSON/schema cases with retained owners')
        proc, m = invoke(binary, {}, policy, data, "--cancel-after", "7")
        assert proc.returncode == 3 and m["status"] == "cancelled" and m["emitted"] == 7
        counts = decode(proc.stdout, keys, reference(data, keys), partial=True)
        assert sum(counts) == 7
        checks.append(f'{policy["name"]}: cancellation preserves each branch prefix and releases all shared owners')
        # Source can fit; the many JSON nodes require more parser storage than the budget.
        heavy = json.dumps({"body": "x", "tag": "y", "meta": [0] * 20000}).encode()
        proc, m = invoke(binary, {"slots": 1, "budget": 160000}, policy, heavy)
        assert proc.returncode == 2 and m["status"] == "budget_too_small", proc.stderr
        assert m["accepted"] == 0
        checks.append(f'{policy["name"]}: parser allocation charged and refused within total budget')
    # Same decoded field is shared after the document metadata has been freed.
    sample = json.dumps({"body": "z" * 40000, "tag": "small"}).encode() + b"\n"
    case = {"keys": ["tag", "tag", "tag"], "hold_us": [0, 1000, 2000]}
    _, separate = invoke(binary, case, {"policy": "copy"}, sample)
    proc, shared = invoke(binary, case, {"policy": "shared_copy"}, sample)
    decode(proc.stdout, case["keys"], reference(sample, case["keys"]))
    assert separate["selection_copy_bytes"] == 15 and shared["selection_copy_bytes"] == 5
    assert shared["dedup_hits"] == 2
    checks.append("identical selections copied once and live until the final consumer")
    case = {"keys": ["body", "tag"], "hold_us": [0, 3000]}
    proc, m = invoke(binary, case, {"policy": "compact"}, sample)
    decode(proc.stdout, case["keys"], reference(sample, case["keys"]))
    assert m["compacted"] == 1 and m["selection_copy_bytes"] == 5
    checks.append("last-owner compaction keeps the delayed decoded value valid")
    proc, m = invoke(binary, {}, policies[0], b'x' * (1024 * 1024 + 1))
    assert proc.returncode == 2 and m["status"] == "record_too_large"
    checks.append("overlong NDJSON record refused without leaked source")
    if Path("/dev/full").exists():
        with open("/dev/full", "wb") as sink:
            proc = subprocess.run(command(binary, {}, policies[0]), input=sample,
                                  stdout=sink, stderr=subprocess.PIPE, timeout=10)
        assert proc.returncode == 2 and metrics(proc.stderr)["status"] == "output_failed"
        checks.append("actual /dev/full failure releases multiple owners")
    source = tmp / "signal.ndjson"
    source.write_bytes(sample * 20)
    with source.open("rb") as inp, open(os.devnull, "wb") as sink:
        proc = subprocess.Popen(command(binary, {"hold_us": [500000] * 3}, policies[0]),
                                stdin=inp, stdout=sink, stderr=subprocess.PIPE)
        time.sleep(0.1)
        proc.send_signal(signal.SIGTERM)
        _, stderr = proc.communicate(timeout=5)
    assert proc.returncode == 3 and metrics(stderr)["status"] == "cancelled"
    checks.append("SIGTERM during delayed fan-out releases all owners")
    return checks


def make_fixture(path, case, seed):
    rng = random.Random(seed)
    hashes = [hashlib.sha256() for _ in case["keys"]]
    sizes = [0] * len(hashes)
    with path.open("wb") as stream:
        for seq in range(case["records"]):
            if case["shape"] == "large":
                body_size, padding = 32768, 8192
            elif case["shape"] == "mixed":
                body_size, padding = rng.choice([(256, 1024), (4096, 16384), (32768, 8192), (65536, 65536)])
            elif case["shape"] == "metadata":
                body_size, padding = 512, 512
            else:
                raise ValueError(case["shape"])
            tile = rng.randbytes(128).hex()
            body = (tile * ((body_size + 255) // 256))[:body_size]
            tag = f'{seq:08x}-한글-"-\\-\n-\x00-' + tile[:32]
            row = {"id": seq, "body": body, "tag": tag, "padding": "p" * padding,
                   "meta": list(range(2048)) if case["shape"] == "metadata" else {"ok": True, "n": seq}}
            encoded = json.dumps(row, ensure_ascii=seq % 2 == 0, separators=(",", ":")).encode("utf-8")
            stream.write(encoded + b"\n")
            # Python's independent decoder defines the expected decoded UTF-8 output.
            decoded = json.loads(encoded)
            for k, key in enumerate(case["keys"]):
                selected = decoded[key].encode("utf-8")
                hashes[k].update(struct.pack("<QI", seq, len(selected)) + selected)
                sizes[k] += len(selected)
    return {"case": case["name"], "seed": seed, "records": case["records"], "input_bytes": path.stat().st_size,
            "input_sha256": digest(path), "keys": case["keys"], "output_bytes_by_branch": sizes,
            "output_sha256_by_branch": [h.hexdigest() for h in hashes]}


def verify_output(path, fixture):
    count = [0] * len(fixture["keys"])
    hashes = [hashlib.sha256() for _ in count]
    sizes = [0] * len(count)
    with path.open("rb") as stream:
        while header := stream.read(16):
            assert len(header) == 16
            k, n, seq = struct.unpack("<IIQ", header)
            assert k < len(count) and seq == count[k] and n <= 1024 * 1024
            data = stream.read(n)
            assert len(data) == n
            hashes[k].update(struct.pack("<QI", seq, n) + data)
            count[k] += 1
            sizes[k] += n
    assert count == [fixture["records"]] * len(count)
    assert sizes == fixture["output_bytes_by_branch"]
    assert [h.hexdigest() for h in hashes] == fixture["output_sha256_by_branch"]


def sample(binary, source, output, case, policy, fixture):
    with source.open("rb") as inp, output.open("wb") as sink:
        proc = subprocess.run(command(binary, case, policy), stdin=inp, stdout=sink,
                              stderr=subprocess.PIPE, timeout=30)
    m = metrics(proc.stderr)
    assert proc.returncode == 0 and m["status"] == "ok", proc.stderr
    assert m["accepted"] == case["records"] and m["emitted"] == case["records"] * len(case["keys"])
    assert m["input_bytes"] == fixture["input_bytes"]
    assert m["live_at_stop_bytes"] == 0
    verify_output(output, fixture)
    return m


def summary(rows):
    keys = ["peak_reserved_bytes", "source_peak_bytes", "output_peak_bytes", "parser_peak_bytes",
            "rss_peak_bytes", "elapsed_ns", "cpu_ns", "parse_call_ns", "selection_copy_bytes",
            "reader_copy_bytes", "parser_realloc_copy_bytes", "source_byte_seconds", "budget_waits",
            "compacted", "dedup_hits"]
    result = []
    for case, variant in sorted({(r["case"], r["variant"]) for r in rows}):
        group = [r for r in rows if (r["case"], r["variant"]) == (case, variant)]
        item = {"case": case, "variant": variant, "samples": len(group)}
        for key in keys:
            values = [r[key] for r in group]
            item[key] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        item["queues"] = [{key: statistics.median(r["queues"][k][key] for r in group)
                           for key in ["peak_count", "latency_mean_ns", "latency_p99_upper_ns"]}
                          for k in range(len(group[0]["queues"]))]
        result.append(item)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not args.check_only and args.output is None:
        parser.error("provide --check-only or --output NEW_DIRECTORY")
    if args.output:
        args.output.mkdir(parents=True, exist_ok=False)
    upstream = json.loads((ROOT / "vendor/yyjson/UPSTREAM.json").read_text())
    for name, item in upstream["files"].items():
        assert digest(ROOT / "vendor/yyjson" / name) == item["sha256"]
    plan = json.loads((ROOT / "plan.json").read_text())
    cc = shlex.split(os.environ.get("CC", "cc"))
    affinity = None
    if hasattr(os, "sched_getaffinity"):
        affinity = min(os.sched_getaffinity(0))
        os.sched_setaffinity(0, {affinity})
    with tempfile.TemporaryDirectory(prefix="pure-fanout-") as folder:
        tmp = Path(folder)
        binary, sanitizer = tmp / "benchmark", tmp / "benchmark-sanitized"
        print("Building optimized parser and pipeline.", flush=True)
        builds = [build(cc, binary)]
        print("Building parser and pipeline with ASan/UBSan (-O1).", flush=True)
        builds.append(build(cc, sanitizer, True))
        print("Checking native and instrumented builds.", flush=True)
        checks = {"native": verify(binary, plan["policies"], tmp),
                  "asan_ubsan": verify(sanitizer, plan["policies"], tmp)}
        print(f'Verified {sum(map(len, checks.values()))} scenario groups (native + ASan/UBSan).', flush=True)
        if args.check_only:
            return
        sources = {name: digest(ROOT / name) for name in SOURCE_NAMES}
        environment = {"platform": platform.platform(), "machine": platform.machine(), "python": platform.python_version(),
                       "compiler": subprocess.check_output([*cc, "--version"], text=True), "cpu_affinity": affinity,
                       "builds": builds, "source_sha256": sources, "upstream": upstream,
                       "timing": "I/O, parsing, selection, consumers, instrumentation, cleanup; excludes setup/build/verification",
                       "parse_call_ns": "wall duration inside yyjson_read_opts including allocator backpressure and serviced consumers",
                       "rss": "Linux current-image VmHWM approximate; zero means unavailable on other OSes",
                       "limits": "synthetic closed-loop arrivals/delays; one thread; uncontrolled thermals/power; cached file I/O; no fsync",
                       "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        if Path("/proc/cpuinfo").exists():
            environment["cpuinfo"] = Path("/proc/cpuinfo").read_text()
        dump(args.output / "environment.json", environment)
        dump(args.output / "verification.json", checks)
        dump(args.output / "plan.json", plan)
        for name in sources:
            destination = args.output / "sources" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((ROOT / name).read_bytes())
        rng, rows, fixtures = random.Random(plan["seed"]), [], []
        for case in plan["cases"]:
            source, output = tmp / "input.ndjson", tmp / "branches.bin"
            info = make_fixture(source, case, plan["seed"])
            fixtures.append(info)
            for policy in plan["policies"]:
                sample(binary, source, output, case, policy, info)
            for repeat in range(plan["repeats"]):
                order = list(plan["policies"])
                rng.shuffle(order)
                for policy in order:
                    value = sample(binary, source, output, case, policy, info)
                    rows.append({"case": case["name"], "variant": policy["name"], "repeat": repeat, **value})
            dump(args.output / "raw.json", rows)
            dump(args.output / "fixtures.json", fixtures)
            print(f'Completed {case["name"]}: {len(plan["policies"])} variants x {plan["repeats"]} trials.', flush=True)
        dump(args.output / "summary.json", summary(rows))
        flat = [{**{k: v for k, v in row.items() if k != "queues"}, "queues": json.dumps(row["queues"])} for row in rows]
        with (args.output / "raw.csv").open("w") as out:
            writer = csv.DictWriter(out, fieldnames=list(flat[0]))
            writer.writeheader()
            writer.writerows(flat)
        assert sources == {name: digest(ROOT / name) for name in SOURCE_NAMES}
        dump(args.output / "complete.json", {"samples": len(rows), "all_branch_outputs_match": True,
                                             "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())})
        print(args.output.resolve())


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Build/test the shared Zig core or serve the local experiment. No Python dependencies."""
import argparse
from datetime import datetime, timezone
from functools import partial
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
VERSION = "0.15.2"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(zig):
    compiler = shutil.which(zig)
    if compiler is None:
        raise SystemExit("Zig 0.15.2가 필요합니다. ziglang.org/download 또는 --zig /path/to/zig를 사용하세요.")
    version = subprocess.check_output([compiler, "version"], text=True).strip()
    if version != VERSION:
        raise SystemExit(f"이 실험은 Zig {VERSION}로 고정했습니다. 현재: {version}")
    build_dir = ROOT / "build"
    build_dir.mkdir(exist_ok=True)
    cache = ["--cache-dir", str(build_dir / "cache"),
             "--global-cache-dir", str(build_dir / "global-cache")]
    source = ROOT / "core.zig"
    native = [compiler, "test", str(source), *cache]
    print("Checking native core…", flush=True)
    checked = subprocess.run(native, capture_output=True, text=True)
    print(checked.stdout + checked.stderr, end="", flush=True)
    checked.check_returncode()
    wasm = ROOT / "web" / "core.wasm"
    wasm.parent.mkdir(exist_ok=True)
    flags = ["-target", "wasm32-freestanding", "-O", "ReleaseSafe", "-fno-entry",
             "-rdynamic", "--export-memory", "--stack", "16384",
             "--initial-memory=65536", "--max-memory=65536"]
    command = [compiler, "build-exe", str(source), *flags, f"-femit-bin={wasm}", *cache]
    subprocess.run(command, check=True)
    (build_dir / "build.json").write_text(json.dumps({
        "built_utc": datetime.now(timezone.utc).isoformat(),
        "zig_version": version, "flags": flags,
        "source_sha256": digest(source), "wasm_sha256": digest(wasm),
        "wasm_file_bytes": wasm.stat().st_size,
        "native_test_output": checked.stdout + checked.stderr,
    }, ensure_ascii=False, indent=2) + "\n")
    print(f"Built {wasm} ({wasm.stat().st_size:,} bytes)")


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    compile_cmd = sub.add_parser("build", help="native tests + WebAssembly build")
    compile_cmd.add_argument("--zig", default="zig")
    serve = sub.add_parser("serve", help="serve web/ on loopback only")
    serve.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    if args.action == "build":
        build(args.zig)
        return
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    if not (ROOT / "web" / "core.wasm").is_file():
        parser.error("먼저 manage.py build를 실행하세요.")
    handler = partial(Handler, directory=str(ROOT / "web"))
    with ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"Pure: http://127.0.0.1:{args.port} (Ctrl+C to stop)", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()

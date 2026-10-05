#!/usr/bin/env python3
"""What HTTP adds to an event (docs/design/022): pure-serve (store in RAM,
cores 0-2) with a module, a write that does almost nothing (POST
/inputs/size of 033's graph.aup) and a read (GET /views/far), each 2,000
times from one connection; the medians in microseconds.

roundtrip.py PURE_SERVE MODULE   (run the client apart: taskset -c 3)"""
import http.client
import json
import statistics
import subprocess
import sys
import tempfile
import time

serve, module = sys.argv[1], sys.argv[2]
with tempfile.TemporaryDirectory() as work:
    host = subprocess.Popen(['taskset', '-c', '0-2', serve, '--module', module, '--store', work + '/store', '--port', '0'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    port = json.loads(host.stdout.readline())['port']
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=60)

    def request(method, path):
        connection.request(method, path, b'', {'Content-Length': '0'})
        answer = connection.getresponse()
        answer.read()
        assert answer.status == 200, (path, answer.status)

    out = {}
    for name, (method, path) in {'write_us': ('POST', '/inputs/size?nodes=100000'), 'read_us': ('GET', '/views/far')}.items():
        took = []
        for _ in range(2000):
            began = time.perf_counter()
            request(method, path)
            took.append((time.perf_counter() - began) * 1e6)
        out[name] = round(statistics.median(took), 1)
    host.terminate()
    host.communicate(timeout=30)
    print(json.dumps(out))

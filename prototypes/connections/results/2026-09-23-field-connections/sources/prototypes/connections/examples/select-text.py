#!/usr/bin/env python3
"""Save a UTF-8 document selection through selection.pure (character indices)."""
import argparse
import http.client
import json
from pathlib import Path
import re
import struct
from urllib.parse import urlsplit

LIMIT = (2 * 6 + 32) * 32768 + 8192


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('file', type=Path)
    parser.add_argument('--key', required=True)
    parser.add_argument('--start', type=int, required=True, help='first Unicode code point, starting at 0')
    parser.add_argument('--end', type=int, required=True, help='exclusive Unicode code point boundary')
    parser.add_argument('--revision', type=int, default=0, help='0 creates; otherwise expected stored revision')
    parser.add_argument('--url', default='http://127.0.0.1:8767')
    args = parser.parse_args()
    url = urlsplit(args.url)
    try:
        port = url.port or 80
    except ValueError as error:
        parser.error(str(error))
    if (url.scheme != 'http' or url.hostname not in ['localhost', '127.0.0.1'] or
            url.username is not None or url.password is not None or url.path not in ['', '/'] or
            url.query or url.fragment):
        parser.error('use a local HTTP server origin')
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', args.key) or not 0 <= args.revision <= 2**63 - 1:
        parser.error('invalid object key or revision')
    try:
        with args.file.open('rb') as source:
            document = source.read(LIMIT - 8 + 1)
        if len(document) > LIMIT - 8:
            parser.error('document exceeds the prototype request limit')
        text = document.decode('utf-8')
    except (OSError, UnicodeError) as error:
        parser.error(str(error))
    if not 0 <= args.start <= args.end <= len(text):
        parser.error('selection is outside the document')
    start = len(text[:args.start].encode('utf-8'))
    selected = text[args.start:args.end].encode('utf-8')
    # The boundary layout is explicit; storage receives only selected bytes.
    payload = struct.pack('<II', start, len(selected)) + document
    headers = {'Content-Type': 'application/octet-stream'}
    headers['If-None-Match' if args.revision == 0 else 'If-Match'] = (
        '*' if args.revision == 0 else f'"{args.revision}"')
    connection = http.client.HTTPConnection(url.hostname, port, timeout=12)
    try:
        connection.request('PUT', '/objects/' + args.key, payload, headers)
        response = connection.getresponse()
        body = response.read(LIMIT + 1)
        if response.status != 200:
            parser.error(f'storage response {response.status}: {response.getheader("X-Pure-Error")}')
        if body != selected:
            parser.error('response differs from the selection; check the installed program (write may have occurred)')
        revision = response.getheader('ETag', '')
        if not re.fullmatch(r'"[1-9][0-9]*"', revision) or int(revision[1:-1]) > 2**63 - 1:
            parser.error('response has no valid stored revision (write may have occurred)')
    except (OSError, http.client.HTTPException) as error:
        parser.error(f'{error}; storage outcome is unknown if the request was sent')
    finally:
        connection.close()
    print(json.dumps({'revision': revision[1:-1], 'selected': selected.decode('utf-8'),
                      'stored_bytes': len(selected)}, ensure_ascii=False))


if __name__ == '__main__':
    main()

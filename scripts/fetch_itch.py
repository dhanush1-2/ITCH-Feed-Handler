#!/usr/bin/env python3
"""Fetch and verify a NASDAQ TotalView-ITCH 5.0 sample file.

    fetch_itch.py <file-name> [--out data/]
    fetch_itch.py --self-test

Integrity checks (the .md5sum companions return HTTP 404, so none are used):
  1. the downloaded size equals the server's Content-Length (resume uses HTTP Range)
  2. gzip's CRC32 and ISIZE trailer, checked by streaming the whole file through gzip
  3. the decompressed stream ends with an 'S' (system event) message, event code 'C'
A failed check deletes the decompressed output and exits nonzero.
"""
import argparse
import datetime
import gzip
import hashlib
import json
import os
import struct
import sys
import tempfile
import urllib.error
import urllib.request
import zlib

BASE_URL = "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/"
CHUNK = 1 << 20


class FetchError(Exception):
    pass


def http_length(url):
    req = urllib.request.Request(url, method="HEAD")
    with urllib.request.urlopen(req, timeout=60) as r:
        n = r.headers.get("Content-Length")
        if n is None:
            raise FetchError("server sent no Content-Length")
        return int(n)


def download(url, dest, log=print):
    """Download url to dest, resuming a partial file. Returns the final size."""
    total = http_length(url)
    have = os.path.getsize(dest) if os.path.exists(dest) else 0
    if have > total:
        raise FetchError(f"{dest} is {have} bytes, larger than the server's {total}; delete it")
    while have < total:
        req = urllib.request.Request(url, headers={"Range": f"bytes={have}-"})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                if have > 0 and r.status != 206:
                    raise FetchError(f"server ignored Range (status {r.status})")
                with open(dest, "ab") as f:
                    while True:
                        buf = r.read(CHUNK)
                        if not buf:
                            break
                        f.write(buf)
                        have += len(buf)
                        if (have // CHUNK) % 100 == 0:
                            log(f"downloaded {have}/{total} bytes ({100 * have / total:.1f}%)")
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            log(f"connection dropped at {have}/{total}: {e}; resuming")
            have = os.path.getsize(dest) if os.path.exists(dest) else 0
    size = os.path.getsize(dest)
    if size != total:
        raise FetchError(f"size {size} != Content-Length {total}")
    return size


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def gunzip_checked(gz_path, out_path, require_end_marker=True):
    """Stream-decompress gz_path to out_path. gzip verifies CRC32 and ISIZE at EOF.

    Returns (decompressed_size, last_message_ok). On any failure the output is
    removed and FetchError is raised.
    """
    tmp = out_path + ".part"
    tail = b""
    size = 0
    try:
        with gzip.open(gz_path, "rb") as src, open(tmp, "wb") as dst:
            while True:
                b = src.read(CHUNK)
                if not b:
                    break
                dst.write(b)
                size += len(b)
                tail = (tail + b)[-64:]
        # gzip.open raised on a bad CRC/ISIZE/truncation before reaching here.
        end_ok = len(tail) >= 14 and tail[-14:-12] == b"\x00\x0c" and tail[-12:-11] == b"S" \
            and tail[-1:] == b"C"
        if require_end_marker and not end_ok:
            raise FetchError("stream does not end with an 'S' event-code 'C' message")
        os.replace(tmp, out_path)
        return size, end_ok
    except (OSError, EOFError, zlib.error, struct.error) as e:
        raise FetchError(f"gzip integrity check failed: {e}") from e
    finally:
        for p in (tmp,):
            if os.path.exists(p):
                os.remove(p)
        # a failed run must not leave a plausible-looking output
        if sys.exc_info()[0] is not None and os.path.exists(out_path):
            os.remove(out_path)


def fetch(name, out_dir, log=print):
    if not name.endswith(".gz"):
        raise FetchError("file name must end in .gz")
    url = BASE_URL + name
    os.makedirs(out_dir, exist_ok=True)
    gz_path = os.path.join(out_dir, name)
    out_path = gz_path[:-3]
    gz_size = download(url, gz_path, log)
    log("download complete; hashing")
    gz_sha = sha256_file(gz_path)
    log("decompressing through gzip (CRC32 + size trailer)")
    try:
        size, end_ok = gunzip_checked(gz_path, out_path)
    except FetchError:
        if os.path.exists(out_path):
            os.remove(out_path)
        raise
    manifest = {
        "url": url,
        "file": os.path.basename(out_path),
        "gz_bytes": gz_size,
        "decompressed_bytes": size,
        "gz_sha256": gz_sha,
        "ends_with_system_event_C": end_ok,
        "fetched_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    mpath = os.path.join(out_dir, os.path.basename(out_path) + ".manifest.json")
    with open(mpath, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    log(f"wrote {out_path} ({size} bytes) and {mpath}")


def self_test():
    # A tiny framed stream: system event 'O' first, system event 'C' last.
    first = b"\x00\x0cS\x00\x00\x00\x00\x0a\x11\xea\x0e\x8c\x43O"
    last = b"\x00\x0cS\x00\x00\x00\x00\x0a\x11\xea\x0e\x8c\x43C"
    body = bytes((i * 131 + 7) & 0xFF for i in range(200000))
    payload = first + body + last
    failures = []
    with tempfile.TemporaryDirectory() as d:
        good = os.path.join(d, "good.gz")
        with gzip.open(good, "wb") as f:
            f.write(payload)
        raw = open(good, "rb").read()

        def expect(label, data, should_pass, **kw):
            p = os.path.join(d, label + ".gz")
            o = os.path.join(d, label)
            with open(p, "wb") as f:
                f.write(data)
            try:
                gunzip_checked(p, o, **kw)
                ok = True
            except FetchError:
                ok = False
            exists = os.path.exists(o)
            if ok != should_pass or (not should_pass and exists):
                failures.append(f"{label}: passed={ok} output_exists={exists}")
            else:
                print(f"self-test {label}: ok")

        expect("good", raw, True)
        expect("truncated", raw[: len(raw) // 2], False)
        expect("truncated_trailer", raw[:-4], False)
        flipped = bytearray(raw)
        flipped[len(flipped) // 2] ^= 0xFF
        expect("byte_flipped", bytes(flipped), False)
        badtrailer = bytearray(raw)
        badtrailer[-5] ^= 0x01  # inside the CRC32 field
        expect("bad_crc", bytes(badtrailer), False)
        noend = gzip.compress(first + body)
        expect("no_end_marker", noend, False)
        expect("no_end_marker_allowed", noend, True, require_end_marker=False)
    if failures:
        print("SELF-TEST FAILED:\n  " + "\n  ".join(failures), file=sys.stderr)
        return 1
    print("self-test passed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("name", nargs="?", help="e.g. 12302019.NASDAQ_ITCH50.gz")
    ap.add_argument("--out", default="data/")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.name:
        ap.error("file name required")
    try:
        fetch(a.name, a.out, log=lambda s: print(s, flush=True))
    except (FetchError, urllib.error.URLError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

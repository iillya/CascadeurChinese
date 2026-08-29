#!/usr/bin/env python3
"""Append the release payload to the installer EXE.

The installer reads its own PE file: the payload is appended after the PE and
ends with [ manifestSize u32 ][ count u32 ][ magic u64 ]. Each manifest entry
is [ nameLen u32 ][ name utf-8 ][ offset u64 ][ size u64 ][ sha256 32 ].
"""

import hashlib
import os
import struct
import glob
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIVE = os.path.join(ROOT, "build", "out")
DIST = os.path.join(ROOT, "dist")

MAGIC = 0x314D4B545343  # "SCSTM1"

def payload_files():
    """Native binaries + every filled dictionary (skip the placeholder pool)."""
    # Runtime probe/capture outputs are only scaffolding for building the
    # dictionary: every entry is an empty-valued placeholder, which the hook
    # deliberately ignores, so shipping them just bloats the installer.
    skip = {
        "static_zh.json",
        "untranslated_captured_zh.json",
        "untranslated_captured2_zh.json",
        "untranslated_scan_zh.json",
        "term_context_zh.json",
        "tour_untranslated_zh.json",
        "tr_probe_missing_zh.json",
    }
    files = [
        "CascadeurChineseHook.dll",
        "CascadeurChineseLauncher.exe",
    ]
    trans = os.path.join(ROOT, "translations")
    for path in sorted(glob.glob(os.path.join(trans, "*_zh.json"))):
        name = os.path.basename(path)
        if name in skip:
            continue
        files.append(os.path.join("translations", name))
    return files

def source_for(rel):
    """Map a payload entry to its source file on disk."""
    if rel.startswith("translations" + os.sep) or rel.startswith("translations/"):
        return os.path.join(ROOT, rel.replace("/", os.sep))
    return os.path.join(NATIVE, rel.replace("/", os.sep))


def build_payload():
    blobs = []
    entries = []
    offset = 0
    for rel in payload_files():
        path = source_for(rel)
        if not os.path.isfile(path):
            raise SystemExit(f"missing payload file: {path}")
        with open(path, "rb") as fh:
            data = fh.read()
        name = rel.replace(os.sep, "/").encode("utf-8")
        entries.append((name, offset, len(data), hashlib.sha256(data).digest()))
        blobs.append(data)
        offset += len(data)
    return blobs, entries


def main():
    if not os.path.isdir(DIST):
        os.makedirs(DIST)
    installer = os.path.join(NATIVE, "CascadeurChineseInstaller.exe")
    if not os.path.isfile(installer):
        raise SystemExit(f"installer not built: {installer}")
    with open(installer, "rb") as fh:
        base = fh.read()

    blobs, entries = build_payload()
    # Offsets are absolute file offsets (from the start of the installer EXE),
    # because the installer indexes the whole file by e.offset.
    payload_start = len(base)
    entries = [(n, off + payload_start, sz, sha) for (n, off, sz, sha) in entries]
    payload = b"".join(blobs)
    manifest = bytearray()
    for name, offset, size, sha in entries:
        manifest += struct.pack("<I", len(name))
        manifest += name
        manifest += struct.pack("<QQ", offset, size)
        manifest += sha

    tail = struct.pack("<IIQ", len(manifest), len(entries), MAGIC)
    out = base + payload + bytes(manifest) + tail
    dest = os.path.join(DIST, "CascadeurChineseInstaller.exe")
    tmp = dest + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(out)
    # Replace atomically; tolerate the destination being momentarily locked
    # (e.g. by an antivirus scan of a previous build).
    try:
        os.replace(tmp, dest)
    except PermissionError:
        # Destination held by another process; retry shortly, else keep tmp.
        time.sleep(1.0)
        try:
            os.replace(tmp, dest)
        except Exception:
            print(f"[warn] could not overwrite {dest}; left {tmp}")
            tmp = dest  # report the actual file path
    print(f"embedded {len(entries)} payload files -> {dest}")
    print(f"  total size {len(out)/(1024*1024):.2f} MB")


if __name__ == "__main__":
    main()

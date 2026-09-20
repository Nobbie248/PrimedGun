#!/usr/bin/env python3
"""Merge Dolphin pipeline UID caches (Cache/<GameID>.uidcache) into one seed file.

PrimedGun ships seeds as Data/Sys/PipelineUIDs/<GameID>-<record size>.uidcache. The Android
launcher copies the matching seed into the cache directory on a fresh install (and appends
any records a user's cache lacks), so "Compile Shaders Before Starting" has something to
compile the first time (see DirectoryInitialization.seedPipelineUidCaches).

The record size is sizeof(SerializedGXPipelineUid), which differs between MSVC builds (692)
and GCC/Clang builds such as Quest and Linux (581) at UID version 11, so only merge caches
written by the same kind of build and name the output with that size.

Usage:
  merge-uidcache.py --record-size 581 Data/Sys/PipelineUIDs/GM8E01-581.uidcache \\
      Data/Sys/PipelineUIDs/GM8E01-581.uidcache <quest cache dir>/GM8E01.uidcache

Inputs may come from a running or killed game: a trailing partial record is dropped.
Duplicate records are written once, in first-seen order, so listing the existing seed first
keeps its content.
"""

import argparse
import struct
import sys

MAGIC = 0x44495550  # "PUID"
HEADER = struct.Struct("<II")


def read_records(path, record_size, expected_version):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HEADER.size:
        sys.exit(f"{path}: too short to be a uidcache")
    magic, version = HEADER.unpack_from(data)
    if magic != MAGIC:
        sys.exit(f"{path}: not a uidcache (bad magic)")
    if expected_version is not None and version != expected_version:
        sys.exit(f"{path}: UID version {version}, expected {expected_version}")
    payload = data[HEADER.size:]
    whole = len(payload) // record_size
    dropped = len(payload) - whole * record_size
    if dropped:
        print(f"{path}: dropping {dropped} trailing bytes (partial record, or wrong --record-size)")
    return version, [payload[i * record_size:(i + 1) * record_size] for i in range(whole)]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--record-size", type=int, required=True,
                        help="sizeof(SerializedGXPipelineUid) of the build that wrote the inputs")
    parser.add_argument("output")
    parser.add_argument("inputs", nargs="+")
    args = parser.parse_args()

    version = None
    seen = set()
    merged = []
    for path in args.inputs:
        version, records = read_records(path, args.record_size, version)
        new = 0
        for record in records:
            if record not in seen:
                seen.add(record)
                merged.append(record)
                new += 1
        print(f"{path}: {len(records)} records, {new} new")

    with open(args.output, "wb") as f:
        f.write(HEADER.pack(MAGIC, version))
        f.write(b"".join(merged))
    print(f"{args.output}: {len(merged)} records, UID version {version}")


if __name__ == "__main__":
    main()

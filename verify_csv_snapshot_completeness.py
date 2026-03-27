#!/usr/bin/env python3
"""
Verify whether all edges in a CSV file are present in JasmineGraph temporal
bitmap snapshots (.ebm).

This script reads all graph{ID}_part{P}_bitmaps.ebm files, extracts edge keys,
and compares them with unique directed edges from the CSV.

Usage:
    python3 verify_csv_snapshot_completeness.py <csv_path> [snapshot_dir] [graph_id]

Examples:
    python3 verify_csv_snapshot_completeness.py wikipedia_chunk_1.csv
    python3 verify_csv_snapshot_completeness.py wikipedia_chunk_1.csv /var/tmp/jasminegraph-localstore/temporal_snapshots
    python3 verify_csv_snapshot_completeness.py wikipedia_chunk_1.csv /var/tmp/jasminegraph-localstore/temporal_snapshots 1
"""

import csv
import glob
import os
import re
import struct
import sys
from collections import defaultdict

PROPERTIES_PATH = "conf/jasminegraph-server.properties"
EBM_REGEX = re.compile(r"graph(\d+)_part(\d+)_bitmaps\.ebm$")


def resolve_snapshot_dir(override_dir):
    if override_dir:
        return override_dir

    if not os.path.exists(PROPERTIES_PATH):
        return "/var/tmp/jasminegraph-localstore/temporal_snapshots"

    with open(PROPERTIES_PATH, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("org.jasminegraph.server.instance.temporalsnapshotfolder="):
                return line.split("=", 1)[1].strip()

    return "/var/tmp/jasminegraph-localstore/temporal_snapshots"


def read_u32(f):
    b = f.read(4)
    if len(b) < 4:
        return None
    return struct.unpack("<I", b)[0]


def read_u64(f):
    b = f.read(8)
    if len(b) < 8:
        return None
    return struct.unpack("<Q", b)[0]


def read_len_prefixed_string(f):
    ln = read_u32(f)
    if ln is None:
        return None
    payload = f.read(ln)
    if len(payload) < ln:
        return None
    return payload.decode("utf-8", errors="ignore")


def load_snapshot_edges(snapshot_dir, only_graph_id=None):
    files = glob.glob(os.path.join(snapshot_dir, "graph*_part*_bitmaps.ebm"))
    by_graph_partition = defaultdict(str)

    for fp in files:
        m = EBM_REGEX.search(os.path.basename(fp))
        if not m:
            continue
        graph_id, partition_id = map(int, m.groups())
        if only_graph_id is not None and graph_id != only_graph_id:
            continue
        by_graph_partition[(graph_id, partition_id)] = fp

    edges_by_graph = defaultdict(set)
    partitions_by_graph = defaultdict(set)

    for (graph_id, partition_id), ebm_path in by_graph_partition.items():
        partitions_by_graph[graph_id].add(partition_id)

        with open(ebm_path, "rb") as f:
            header = f.read(64)
            if len(header) < 64:
                continue

            if header[0:8] != b"JGBINDEX":
                continue

            edge_bitmap_count = read_u64(f)
            if edge_bitmap_count is None:
                continue

            for _ in range(edge_bitmap_count):
                src = read_len_prefixed_string(f)
                dst = read_len_prefixed_string(f)
                data_size = read_u32(f)

                if src is None or dst is None or data_size is None:
                    break

                bitmap_payload = f.read(data_size)
                if len(bitmap_payload) < data_size:
                    break

                edges_by_graph[graph_id].add(f"{src}_{dst}")

    return edges_by_graph, partitions_by_graph, len(files)


def load_csv_edges(csv_path):
    edges = set()
    with open(csv_path, "r", newline="") as f:
        reader = csv.reader(f)
        for row in reader:
            if len(row) < 2:
                continue
            src = row[0].strip().strip('"')
            dst = row[1].strip().strip('"')
            if not src or not dst:
                continue
            if src.lower() == "source" and dst.lower() == "target":
                continue
            edges.add(f"{src}_{dst}")
    return edges


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    csv_path = sys.argv[1]
    override_dir = sys.argv[2] if len(sys.argv) > 2 else None
    graph_id = int(sys.argv[3]) if len(sys.argv) > 3 else None

    if not os.path.exists(csv_path):
        print(f"ERROR: CSV file not found: {csv_path}")
        return 2

    snapshot_dir = resolve_snapshot_dir(override_dir)
    print(f"snapshot_dir={snapshot_dir}")

    if not os.path.isdir(snapshot_dir):
        print("ERROR: Snapshot directory does not exist")
        return 3

    edges_by_graph, partitions_by_graph, total_snap_files = load_snapshot_edges(snapshot_dir, graph_id)
    print(f"snapshot_files={total_snap_files}")

    if not edges_by_graph:
        print("ERROR: No snapshot edges found. Stream first, then re-run this check.")
        return 4

    csv_edges = load_csv_edges(csv_path)
    print(f"csv_unique_edges={len(csv_edges)}")

    for gid in sorted(edges_by_graph.keys()):
        snapshot_edges = edges_by_graph[gid]
        missing = csv_edges - snapshot_edges
        extra = snapshot_edges - csv_edges

        print("---")
        print(f"graph_id={gid}")
        print(f"partitions_detected={sorted(partitions_by_graph[gid])}")
        print(f"snapshot_unique_edges={len(snapshot_edges)}")
        print(f"missing_count={len(missing)}")
        print(f"extra_count={len(extra)}")

        if len(missing) == 0:
            print("status=OK all CSV edges are present in snapshot bitmaps")
        else:
            print("status=MISMATCH some CSV edges are missing from snapshot bitmaps")

        if missing:
            print(f"missing_sample={sorted(list(missing))[:10]}")
        if extra:
            print(f"extra_sample={sorted(list(extra))[:10]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
Verify JasmineGraph triangle count at a temporal snapshot using NetworkX.

Reads the .ebm bitmap-index files written by JasmineGraph workers, extracts all
edges that were active at a given snapshot, builds an undirected NetworkX graph,
and counts triangles to compare with the value reported by the `histrian` command.

Usage:
    python3 verify_triangles_networkx.py [graph_id] [snapshot_id]

Defaults: graph_id=2, snapshot_id=4
"""

import struct
import os
import sys
import time

try:
    import pyroaring
except ImportError:
    sys.exit("pyroaring is required: pip3 install pyroaring")

try:
    import networkx as nx
except ImportError:
    sys.exit("networkx is required: pip3 install networkx")


SNAPSHOT_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "env", "data", "temporal_snapshots"
)

BITMAP_FILE_HEADER_SIZE = 64   # sizeof(BitmapFileHeader) in C++
EBM_MAGIC = b"JGBINDEX"


def read_length_prefixed_string(buf: bytes, offset: int):
    """Read uint32-length-prefixed string; return (string, new_offset)."""
    if offset + 4 > len(buf):
        raise ValueError(f"Buffer too short for string length at offset {offset}")
    length = struct.unpack_from("<I", buf, offset)[0]
    offset += 4
    if offset + length > len(buf):
        raise ValueError(f"Buffer too short for string data: need {length} bytes at {offset}")
    return buf[offset:offset + length].decode("utf-8"), offset + length


def edge_active_at_snapshot(bitmap_bytes: bytes, snapshot_id: int) -> bool:
    """
    Return True if the edge's lifespan bitmap contains any value in [0, snapshot_id].
    This mirrors HistoryTriangles / EdgeLifespanBitmap::intersectsRange(0, snapshotId).
    """
    bm = pyroaring.BitMap.deserialize(bitmap_bytes)
    if len(bm) == 0:
        return False
    return bm.min() <= snapshot_id


def read_ebm_file(filepath: str, snapshot_id: int):
    """
    Parse one graph{G}_part{P}_bitmaps.ebm file.

    File layout (mirrors TemporalStorePersistence::saveBitmapIndex):
        [64-byte BitmapFileHeader]
        uint64_t  edgeCount          (redundant with header.edgeCount but always written)
        For each edge:
            uint32_t  srcLen + src bytes
            uint32_t  dstLen + dst bytes
            uint32_t  bitmapDataSize + bitmap bytes (CRoaring portable format)

    Returns (partition_id, active_edges) where active_edges is a list of (src, dst)
    tuples for edges whose lifespan bitmap intersects [0, snapshot_id].
    """
    with open(filepath, "rb") as f:
        raw = f.read()

    if len(raw) < BITMAP_FILE_HEADER_SIZE:
        raise ValueError(f"File too short: {filepath}")

    magic = raw[:8]
    if magic != EBM_MAGIC:
        raise ValueError(f"Bad magic in {filepath}: {magic!r}")

    version      = struct.unpack_from("<I", raw,  8)[0]
    graph_id     = struct.unpack_from("<I", raw, 12)[0]
    partition_id = struct.unpack_from("<I", raw, 16)[0]
    latest_snap  = struct.unpack_from("<I", raw, 20)[0]

    if version != 1:
        raise ValueError(f"Unsupported version {version} in {filepath}")

    # Skip the 64-byte header, then read the edge count (8 bytes).
    pos = BITMAP_FILE_HEADER_SIZE
    edge_count = struct.unpack_from("<Q", raw, pos)[0]
    pos += 8

    active_edges = []
    for _ in range(edge_count):
        src, pos = read_length_prefixed_string(raw, pos)
        dst, pos = read_length_prefixed_string(raw, pos)

        if pos + 4 > len(raw):
            break
        data_size = struct.unpack_from("<I", raw, pos)[0]
        pos += 4

        bitmap_bytes = raw[pos:pos + data_size]
        pos += data_size

        if edge_active_at_snapshot(bitmap_bytes, snapshot_id):
            active_edges.append((src, dst))

    return partition_id, active_edges, edge_count, latest_snap


def main():
    graph_id    = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    snapshot_id = int(sys.argv[2]) if len(sys.argv) > 2 else 4

    print(f"Graph ID      : {graph_id}")
    print(f"Snapshot ID   : {snapshot_id}")
    print(f"Snapshot dir  : {SNAPSHOT_DIR}")
    print("=" * 60)

    all_edges = []
    for part_id in range(50):
        filepath = os.path.join(
            SNAPSHOT_DIR, f"graph{graph_id}_part{part_id}_bitmaps.ebm"
        )
        if not os.path.exists(filepath):
            continue

        pid, active, total, latest = read_ebm_file(filepath, snapshot_id)
        print(
            f"  Part {pid}: {total:>7} stored edges, "
            f"{len(active):>7} active at snap {snapshot_id}  "
            f"(latest snap in file: {latest})"
        )
        all_edges.extend(active)

    if not all_edges:
        print(f"\nERROR: No edges found for graph {graph_id}. "
              f"Check SNAPSHOT_DIR and graph/partition files.")
        sys.exit(1)

    self_loops = [(s, d) for s, d in all_edges if s == d]
    clean_edges = [(s, d) for s, d in all_edges if s != d]
    print(f"\nTotal raw directed edges (all partitions): {len(all_edges)}")
    if self_loops:
        print(f"  Self-loops detected        : {len(self_loops)}  "
              f"(these are skipped — they produce spurious triangles)")
    print(f"  Non-self-loop edges        : {len(clean_edges)}")

    # Build undirected graph — skip self-loops to match the corrected C++ behaviour.
    t0 = time.time()
    G = nx.Graph()
    G.add_edges_from(clean_edges)
    build_ms = (time.time() - t0) * 1000

    print(f"Unique nodes               : {G.number_of_nodes()}")
    print(f"Unique undirected edges    : {G.number_of_edges()}")
    print(f"Graph build time           : {build_ms:.0f} ms")

    # Count triangles
    print("\nCounting triangles with NetworkX (this may take a minute)…")
    t0 = time.time()
    tri_per_node = nx.triangles(G)
    nx_triangles = sum(tri_per_node.values()) // 3
    count_ms = (time.time() - t0) * 1000

    jasminegraph_count = int(sys.argv[3]) if len(sys.argv) > 3 else None

    print(f"\n{'='*60}")
    print(f"NetworkX triangle count    : {nx_triangles:,}")
    print(f"Triangle count time        : {count_ms:.0f} ms")
    if jasminegraph_count is not None:
        diff = jasminegraph_count - nx_triangles
        pct  = diff / nx_triangles * 100 if nx_triangles else float('nan')
        print(f"JasmineGraph count         : {jasminegraph_count:,}")
        print(f"Difference (JG - NX)       : {diff:+,}  ({pct:+.3f}%)")
        if diff == 0:
            print("RESULT: EXACT MATCH ✓")
        elif abs(pct) < 0.01:
            print(f"RESULT: within 0.01% — effectively correct")
        else:
            print(f"RESULT: discrepancy detected")


if __name__ == "__main__":
    main()

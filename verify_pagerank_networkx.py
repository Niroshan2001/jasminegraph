#!/usr/bin/env python3
"""
Verify JasmineGraph temporal PageRank at a given snapshot using NetworkX.

Reads the same .ebm bitmap-index files used by verify_triangles_networkx.py,
reconstructs the graph that was active at the requested snapshot, runs
networkx.pagerank(), and compares the top-K nodes / scores with the values
reported by the `histrian_pagerank` command.

Usage:
    python3 verify_pagerank_networkx.py [graph_id] [snapshot_id] [top_k]
                                        [alpha] [jasminegraph_output_file]

    graph_id               – default 2
    snapshot_id            – default 4
    top_k                  – number of top nodes to print (default 10, 0 = all)
    alpha                  – damping factor, default 0.85
    jasminegraph_output_file – optional path to a file whose lines are
                               "<rank>\\t<node>\\t<score>" as printed by the
                               histrian_pagerank command; when supplied the
                               script does a side-by-side comparison.

Example:
    python3 verify_pagerank_networkx.py 2 4 10 0.85 jg_pagerank.txt
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


# ---------------------------------------------------------------------------
# Helpers copied from verify_triangles_networkx.py (same file format)
# ---------------------------------------------------------------------------

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
    Mirrors EdgeLifespanBitmap::intersectsRange(0, snapshotId) in C++.
    """
    bm = pyroaring.BitMap.deserialize(bitmap_bytes)
    if len(bm) == 0:
        return False
    return bm.min() <= snapshot_id


def read_ebm_file(filepath: str, snapshot_id: int):
    """
    Parse one graph{G}_part{P}_bitmaps.ebm file and return edges active at
    the given snapshot.

    Returns (partition_id, active_edges, total_stored, latest_snapshot)
    where active_edges is a list of (src, dst) strings.
    """
    with open(filepath, "rb") as f:
        raw = f.read()

    if len(raw) < BITMAP_FILE_HEADER_SIZE:
        raise ValueError(f"File too short: {filepath}")

    magic = raw[:8]
    if magic != EBM_MAGIC:
        raise ValueError(f"Bad magic in {filepath}: {magic!r}")

    version      = struct.unpack_from("<I", raw,  8)[0]
    partition_id = struct.unpack_from("<I", raw, 16)[0]
    latest_snap  = struct.unpack_from("<I", raw, 20)[0]

    if version != 1:
        raise ValueError(f"Unsupported version {version} in {filepath}")

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


# ---------------------------------------------------------------------------
# PageRank verification
# ---------------------------------------------------------------------------

def load_jasminegraph_output(filepath: str):
    """
    Parse a file whose lines are "<rank>\\t<node>\\t<score>" (as printed by
    the histrian_pagerank command) into a list of (node, score) tuples.
    Header / separator lines are skipped automatically.
    """
    results = []
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("Rank") or line.startswith("---"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            try:
                node  = parts[1].strip()
                score = float(parts[2].strip())
                results.append((node, score))
            except ValueError:
                continue
    return results


def compare_rankings(nx_top: list, jg_top: list, top_k: int):
    """Print a side-by-side comparison table."""
    k = min(len(nx_top), len(jg_top), top_k if top_k > 0 else max(len(nx_top), len(jg_top)))
    print(f"\n{'Rank':<6} {'NX Node':<30} {'NX Score':>14}   {'JG Node':<30} {'JG Score':>14}  Match")
    print("-" * 100)
    matches = 0
    for i in range(k):
        nx_node, nx_score = nx_top[i] if i < len(nx_top) else ("—", float("nan"))
        jg_node, jg_score = jg_top[i] if i < len(jg_top) else ("—", float("nan"))
        node_match = "✓" if nx_node == jg_node else "✗"
        if nx_node == jg_node:
            matches += 1
        rel_error = ""
        if nx_node == jg_node and nx_score != 0:
            pct = abs(nx_score - jg_score) / nx_score * 100
            rel_error = f"  ({pct:.4f}% diff)"
        print(f"{i+1:<6} {nx_node:<30} {nx_score:>14.8f}   {jg_node:<30} {jg_score:>14.8f}  {node_match}{rel_error}")
    print("-" * 100)
    print(f"Top-{k} node rank agreement: {matches}/{k}  ({matches/k*100:.1f}%)")

    # Score correlation for shared nodes
    nx_dict = dict(nx_top[:k])
    jg_dict = dict(jg_top[:k])
    common = set(nx_dict) & set(jg_dict)
    if common:
        errors = [abs(nx_dict[n] - jg_dict[n]) / nx_dict[n] * 100 for n in common if nx_dict[n] != 0]
        if errors:
            print(f"Score relative error (common nodes): "
                  f"mean={sum(errors)/len(errors):.4f}%  "
                  f"max={max(errors):.4f}%")


def main():
    graph_id    = int(sys.argv[1])   if len(sys.argv) > 1 else 2
    snapshot_id = int(sys.argv[2])   if len(sys.argv) > 2 else 4
    top_k       = int(sys.argv[3])   if len(sys.argv) > 3 else 10
    alpha       = float(sys.argv[4]) if len(sys.argv) > 4 else 0.85
    jg_file     = sys.argv[5]        if len(sys.argv) > 5 else None

    print(f"Graph ID      : {graph_id}")
    print(f"Snapshot ID   : {snapshot_id}")
    print(f"Top-K         : {top_k if top_k > 0 else 'all'}")
    print(f"Alpha (damping): {alpha}")
    print(f"Snapshot dir  : {SNAPSHOT_DIR}")
    print("=" * 60)

    # ------------------------------------------------------------------
    # 1. Read .ebm files and collect active edges
    # ------------------------------------------------------------------
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
              f"Check SNAPSHOT_DIR and partition files.")
        sys.exit(1)

    self_loops  = [(s, d) for s, d in all_edges if s == d]
    clean_edges = [(s, d) for s, d in all_edges if s != d]
    print(f"\nTotal raw directed edges   : {len(all_edges)}")
    if self_loops:
        print(f"  Self-loops skipped       : {len(self_loops)}")
    print(f"  Non-self-loop edges      : {len(clean_edges)}")

    # ------------------------------------------------------------------
    # 2. Build a directed graph (PageRank is direction-sensitive)
    # ------------------------------------------------------------------
    t0 = time.time()
    G = nx.DiGraph()
    G.add_edges_from(clean_edges)
    build_ms = (time.time() - t0) * 1000

    print(f"Unique nodes               : {G.number_of_nodes()}")
    print(f"Unique directed edges      : {G.number_of_edges()}")
    print(f"Graph build time           : {build_ms:.0f} ms")

    # ------------------------------------------------------------------
    # 3. Run NetworkX PageRank
    # ------------------------------------------------------------------
    print(f"\nRunning NetworkX PageRank (alpha={alpha}, max_iter=100)…")
    t0 = time.time()
    pr = nx.pagerank(G, alpha=alpha, max_iter=100, tol=1.0e-6)
    pr_ms = (time.time() - t0) * 1000
    print(f"PageRank time              : {pr_ms:.0f} ms")

    # Sort descending by score
    ranked = sorted(pr.items(), key=lambda x: x[1], reverse=True)

    # ------------------------------------------------------------------
    # 4. Print top-K results
    # ------------------------------------------------------------------
    k_print = top_k if top_k > 0 else len(ranked)
    print(f"\n{'='*60}")
    print(f"Top-{top_k if top_k > 0 else 'all'} PageRank results (NetworkX):")
    print(f"{'Rank':<6} {'Node':<35} {'Score':>18}")
    print("-" * 60)
    for i, (node, score) in enumerate(ranked[:k_print]):
        print(f"{i+1:<6} {node:<35} {score:>18.10f}")

    # ------------------------------------------------------------------
    # 5. Optional: compare with JasmineGraph output file
    # ------------------------------------------------------------------
    if jg_file:
        if not os.path.exists(jg_file):
            print(f"\nWARNING: JasmineGraph output file not found: {jg_file}")
        else:
            jg_top = load_jasminegraph_output(jg_file)
            print(f"\nLoaded {len(jg_top)} entries from JasmineGraph output: {jg_file}")
            k_cmp = top_k if top_k > 0 else min(len(ranked), len(jg_top))
            compare_rankings(ranked[:k_cmp], jg_top[:k_cmp], k_cmp)

    # ------------------------------------------------------------------
    # 6. Summary
    # ------------------------------------------------------------------
    print(f"\n{'='*60}")
    print(f"Nodes processed            : {G.number_of_nodes()}")
    print(f"Edges processed            : {G.number_of_edges()}")
    print(f"PageRank sum (should=1.0)  : {sum(pr.values()):.10f}")
    print(f"Max score                  : {ranked[0][1]:.10f}  (node {ranked[0][0]})")
    print(f"Min score                  : {ranked[-1][1]:.10f}  (node {ranked[-1][0]})")


if __name__ == "__main__":
    main()

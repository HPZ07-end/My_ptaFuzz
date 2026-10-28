"""Small, deterministic helpers shared by training and experiment reports."""

import hashlib
import json
from pathlib import Path


def append_jsonl(path, row):
    with Path(path).open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(row, ensure_ascii=False, allow_nan=False) + "\n")


def exploration_progress(evaluations, max_candidates, elapsed, max_time):
    # Decisions are numbered 0..N-1: the last budgeted decision reaches epsilon_end.
    if max_candidates is not None:
        return min(1.0, max(0.0, evaluations / max(1, max_candidates - 1)))
    return min(1.0, max(0.0, elapsed / max(1, max_time)))


def structural_group(path):
    from scripts.netlist_features import structural_signature
    signature = sorted(structural_signature(path).items())
    return hashlib.sha256(json.dumps(signature).encode("utf-8")).hexdigest()[:20]


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

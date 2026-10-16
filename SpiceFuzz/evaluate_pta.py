"""Replay saved decks with the same evaluator used by the learning loop."""

import argparse
import hashlib
import json
from pathlib import Path

from scripts.dc_evaluation import write_evaluation_manifest
from scripts.pta_pipeline import evaluate_candidate
from scripts.pta_reward import calculate_pta_reward


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--netlist", required=True, type=Path, help="One deck or directory of decks")
    parser.add_argument("--pta_bin", required=True, type=Path)
    parser.add_argument("--ngspice_bin", type=Path)
    parser.add_argument("--pta_config", type=Path)
    parser.add_argument("--pta_option", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--output_root", type=Path, default=Path("test/replay"))
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    for executable in (args.pta_bin, args.ngspice_bin):
        if executable and not executable.is_file():
            parser.error(f"simulator executable not found: {executable}")
    decks = sorted(p for p in args.netlist.rglob("*") if p.suffix.lower() in {".cir", ".sp", ".net"}) if args.netlist.is_dir() else [args.netlist]
    if not decks or any(not p.is_file() for p in decks):
        parser.error("no netlist files found")
    rows = []
    for deck in decks:
        # Input-name collisions across subdirectories get independent roots.
        identity = hashlib.sha256(str(deck.resolve()).encode()).hexdigest()[:12]
        root = args.output_root / (deck.stem + "_" + identity)
        evaluation = evaluate_candidate(deck, args.timeout, args.pta_bin, root,
                                        args.ngspice_bin, args.pta_config, args.pta_option)
        reward = calculate_pta_reward(evaluation, objective="ngspice_differential" if args.ngspice_bin else "pta_internal")
        root.mkdir(parents=True, exist_ok=True)
        write_evaluation_manifest(evaluation, root / "evaluation.json", reward.as_dict())
        rows.append({"netlist": str(deck), "stage": evaluation.stage.value,
                     "nr_differential": evaluation.nr_differential_candidate,
                     "confirmed_target": evaluation.authoritative_target,
                     "trainable": evaluation.trainable, "reward": reward.total,
                     "evaluation": str(root / "evaluation.json")})
        print(json.dumps(rows[-1], ensure_ascii=False))
    (args.output_root / "summary.json").write_text(json.dumps(rows, indent=2, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()

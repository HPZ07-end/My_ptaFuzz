import argparse
import shutil
import tempfile
from datetime import datetime
from pathlib import Path

from scripts.pta_testing import SUCCESS, parse_with_pta, validate_pta_dc_netlist


def clean_netlist(source, destination):
    lines = source.read_text(errors="replace").splitlines()
    if not lines:
        return False

    cleaned = [lines[0].strip()]
    for raw_line in lines[1:]:
        line = raw_line.strip()
        if not line or line.startswith("*"):
            continue
        comment_index = line.find(";")
        if comment_index != -1:
            line = line[:comment_index].strip()
        if line:
            cleaned.append(line)

    if len(cleaned) > 100:
        return False
    destination.write_text("\n".join(cleaned) + "\n")
    return True


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Build a PTA-compatible SpiceFuzz seed-netlist pool"
    )
    parser.add_argument("--raw_data_path", type=Path, required=True)
    parser.add_argument("--seed_netlist_path", type=Path, required=True)
    parser.add_argument(
        "--pta_bin",
        type=Path,
        required=True,
        help="Path to the PTA simulator executable (spice or spice.exe)",
    )
    parser.add_argument("--maxtime", type=int, default=60)
    return parser.parse_args()


def main():
    args = parse_arguments()
    if not args.raw_data_path.is_dir():
        raise FileNotFoundError(
            f"Raw-data directory not found: {args.raw_data_path}"
        )
    pta_bin = args.pta_bin.resolve()
    if not pta_bin.is_file():
        raise FileNotFoundError(f"PTA simulator not found: {pta_bin}")

    args.seed_netlist_path.mkdir(parents=True, exist_ok=True)
    accepted = 0
    rejected = 0
    with tempfile.TemporaryDirectory(prefix="spicefuzz-pta-prep-") as directory:
        workdir = Path(directory)
        for source in args.raw_data_path.rglob("*"):
            if not source.is_file() or source.suffix.lower() not in {
                ".cir", ".net", ".sp"
            }:
                continue
            candidate = workdir / source.name
            shutil.copy(source, candidate)
            cleaned = workdir / f"cleaned-{source.name}"
            if not clean_netlist(candidate, cleaned):
                rejected += 1
                print(f"REJECT {source}: empty or more than 100 lines")
                continue
            dc_valid, validation_error = validate_pta_dc_netlist(cleaned)
            if not dc_valid:
                rejected += 1
                print(f"REJECT {source}: {validation_error}")
                continue
            code, details = parse_with_pta(cleaned, args.maxtime, pta_bin)
            if code != SUCCESS:
                rejected += 1
                message = details.get("stderr", "").strip()
                print(f"REJECT {source}: {message}")
                continue
            name = datetime.now().strftime("%Y%m%d%H%M%S%f") + ".cir"
            shutil.copy(cleaned, args.seed_netlist_path / name)
            accepted += 1

    print(f"PTA seed preparation: accepted={accepted}, rejected={rejected}")


if __name__ == "__main__":
    main()

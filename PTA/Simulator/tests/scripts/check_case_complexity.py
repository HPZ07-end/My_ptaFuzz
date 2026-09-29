#!/usr/bin/env python3
"""Audit OP/TRAN case counts, naming, line tiers and basic deck shape."""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import sys


EXPECTED_PER_LEVEL = {1: 4, 2: 4, 3: 5, 4: 5}
LEVEL_RANGES = {
    1: (10, 20),
    2: (20, 40),
    3: (40, 100),
    4: (101, None),
}
NAME_PATTERN = re.compile(r"^level([1-4])_(\d{2})_[a-z0-9_]+\.cir$")
ALLOWED_DIRECTIVES = {".model", ".op", ".tran", ".print", ".end"}
ALLOWED_ELEMENT_INITIALS = set("rclvidqm")
EXPECTED_LEVEL_BY_INDEX = {
    **{index: 1 for index in range(1, 5)},
    **{index: 2 for index in range(5, 9)},
    **{index: 3 for index in range(9, 14)},
    **{index: 4 for index in range(14, 19)},
}


def _matches_range(level: int, count: int) -> bool:
    lower, upper = LEVEL_RANGES[level]
    return count >= lower and (upper is None or count <= upper)


def _check_deck_shape(
    path: Path,
    analysis: str,
    lines: list[str],
    errors: list[str],
) -> None:
    label = f"{analysis}/{path.name}"
    if not lines:
        errors.append(f"{label}: empty file")
        return
    if any(not line.strip() for line in lines):
        errors.append(f"{label}: blank physical lines are not allowed as padding")
    comment_count = sum(line.lstrip().startswith("*") for line in lines[1:])
    if comment_count > 3:
        errors.append(
            f"{label}: {comment_count} full-line comments look like tier padding"
        )
    if lines[-1].strip().lower() != ".end":
        errors.append(f"{label}: final physical line must be .end")

    statements: list[str] = []
    for line_number, raw_line in enumerate(lines[1:], start=2):
        line = raw_line.strip()
        if not line or line.startswith("*"):
            continue
        token = line.split(None, 1)[0].lower()
        statements.append(line.lower())
        if token.startswith("."):
            if token not in ALLOWED_DIRECTIVES:
                errors.append(f"{label}:{line_number}: unsupported directive {token}")
        elif token[0] not in ALLOWED_ELEMENT_INITIALS:
            errors.append(f"{label}:{line_number}: unsupported element {token}")

    own_directive = f".{analysis}"
    other_directive = ".tran" if analysis == "op" else ".op"
    directive_tokens = [statement.split(None, 1)[0] for statement in statements]
    if directive_tokens.count(own_directive) != 1:
        errors.append(f"{label}: expected exactly one {own_directive} directive")
    if other_directive in directive_tokens:
        errors.append(f"{label}: must not contain {other_directive}")
    if not any(statement.startswith(f".print {analysis}") for statement in statements):
        errors.append(f"{label}: missing .print {analysis}")


def inspect(root: Path) -> list[str]:
    errors: list[str] = []
    testcase_root = root / "tests" / "cases"

    for analysis in ("op", "tran"):
        directory = testcase_root / analysis
        if not directory.is_dir():
            errors.append(f"{directory}: directory does not exist")
            continue

        paths = sorted(directory.glob("*.cir"))
        if len(paths) != 18:
            errors.append(f"{analysis}: expected 18 .cir files, found {len(paths)}")

        levels: Counter[int] = Counter()
        indices: list[int] = []
        line_counts: list[int] = []
        for path in paths:
            match = NAME_PATTERN.fullmatch(path.name)
            if not match:
                errors.append(f"{analysis}/{path.name}: invalid filename")
                continue

            level = int(match.group(1))
            index = int(match.group(2))
            levels[level] += 1
            indices.append(index)
            if EXPECTED_LEVEL_BY_INDEX.get(index) != level:
                errors.append(
                    f"{analysis}/{path.name}: index {index:02d} belongs to "
                    f"level {EXPECTED_LEVEL_BY_INDEX.get(index, '?')}"
                )

            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except (OSError, UnicodeError) as exc:
                errors.append(f"{analysis}/{path.name}: cannot read: {exc}")
                continue

            count = len(lines)
            line_counts.append(count)
            if not _matches_range(level, count):
                lower, upper = LEVEL_RANGES[level]
                expected = f">={lower}" if upper is None else f"{lower}-{upper}"
                errors.append(
                    f"{analysis}/{path.name}: {count} lines; level {level} "
                    f"requires {expected}"
                )
            _check_deck_shape(path, analysis, lines, errors)
            print(f"{analysis:4s}  L{level}  {count:3d} lines  {path.name}")

        if levels != Counter(EXPECTED_PER_LEVEL):
            errors.append(
                f"{analysis}: level distribution is {dict(sorted(levels.items()))}; "
                f"expected {EXPECTED_PER_LEVEL}"
            )
        if sorted(indices) != list(range(1, 19)):
            errors.append(f"{analysis}: case indices must be exactly 01 through 18")
        if not line_counts or max(line_counts) <= 300:
            errors.append(f"{analysis}: the most complex case must exceed 300 lines")

    return errors


def _parse_args() -> argparse.Namespace:
    default_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=default_root,
        help="repository root containing tests/cases/ (default: repository root)",
    )
    return parser.parse_args()


def main() -> int:
    errors = inspect(_parse_args().root.resolve())
    if errors:
        print("\nComplexity audit failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print("\nComplexity audit passed: 18 OP + 18 TRAN; levels 4/4/5/5; maxima >300.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

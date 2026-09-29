import math
import re
from pathlib import Path

import numpy as np


DEVICE_TYPES = ("r", "c", "l", "v", "i", "d", "q", "m", "x")
FEATURE_NAMES = (
    "line_count",
    "device_count",
    "node_count",
    "nonlinear_fraction",
    "resistor_fraction",
    "capacitor_fraction",
    "inductor_fraction",
    "voltage_source_fraction",
    "current_source_fraction",
    "diode_fraction",
    "bjt_fraction",
    "mos_fraction",
    "subcircuit_fraction",
    "model_count",
    "has_initial_hint",
    "value_span_decades",
)

_NUMBER = re.compile(
    r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?(?:meg|mil|[tgkmunpf])?$",
    re.IGNORECASE,
)
_SUFFIX_SCALE = {
    "t": 1e12,
    "g": 1e9,
    "meg": 1e6,
    "k": 1e3,
    "mil": 25.4e-6,
    "m": 1e-3,
    "u": 1e-6,
    "n": 1e-9,
    "p": 1e-12,
    "f": 1e-15,
}


def _scaled_count(value, reference=100.0):
    return min(1.0, math.log1p(value) / math.log1p(reference))


def _parse_number(token):
    token = token.strip().rstrip(",)")
    if "=" in token:
        token = token.split("=", 1)[1]
    if not _NUMBER.fullmatch(token):
        return None
    lower = token.lower()
    for suffix in ("meg", "mil", "t", "g", "k", "m", "u", "n", "p", "f"):
        if lower.endswith(suffix):
            return float(lower[:-len(suffix)]) * _SUFFIX_SCALE[suffix]
    return float(lower)


def _element_nodes(tokens):
    kind = tokens[0][0].lower()
    counts = {"r": 2, "c": 2, "l": 2, "v": 2, "i": 2, "d": 2,
              "q": 3, "m": 4}
    if kind in counts:
        return tokens[1:1 + counts[kind]]
    if kind == "x" and len(tokens) > 2:
        positional = [token for token in tokens[1:] if "=" not in token]
        return positional[:-1]
    return []


def extract_netlist_features(netlist):
    """Return bounded structural/numeric features for one SPICE deck."""
    if isinstance(netlist, Path):
        text = netlist.read_text(errors="replace")
    else:
        text = str(netlist)

    logical_lines = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("*"):
            continue
        for marker in (";", "//", "$"):
            marker_index = line.find(marker)
            if marker_index >= 0:
                line = line[:marker_index].strip()
        if line:
            logical_lines.append(line)

    device_counts = {kind: 0 for kind in DEVICE_TYPES}
    nodes = set()
    model_count = 0
    has_initial_hint = False
    numeric_values = []

    # The first non-directive line is the SPICE title and not an element.
    title_consumed = False
    for line in logical_lines:
        lower = line.lower()
        tokens = line.replace("(", " ").replace(")", " ").split()
        if not tokens:
            continue
        if not title_consumed:
            title_consumed = True
            continue
        if lower.startswith(".model"):
            model_count += 1
        if lower.startswith((".ic", ".nodeset")):
            has_initial_hint = True
        if line.startswith(".") or line.startswith("+"):
            for token in tokens:
                if "=" in token:
                    value = _parse_number(token)
                    if value is not None and math.isfinite(value) and value != 0:
                        numeric_values.append(abs(value))
            continue
        kind = tokens[0][0].lower()
        if kind not in device_counts:
            continue
        device_counts[kind] += 1
        nodes.update(node.lower() for node in _element_nodes(tokens) if node != "0")

        # Values in name=value fields are never node identifiers.  The fourth
        # token is the scalar value for primitive R/C/L/V/I forms.
        for token in tokens[1:]:
            if "=" in token:
                value = _parse_number(token)
                if value is not None and math.isfinite(value) and value != 0:
                    numeric_values.append(abs(value))
        if kind in {"r", "c", "l", "v", "i"} and len(tokens) >= 4:
            value_token = tokens[4] if len(tokens) >= 5 and tokens[3].lower() == "dc" else tokens[3]
            value = _parse_number(value_token)
            if value is not None and math.isfinite(value) and value != 0:
                numeric_values.append(abs(value))

    device_count = sum(device_counts.values())
    denominator = max(1, device_count)
    nonlinear_count = device_counts["d"] + device_counts["q"] + device_counts["m"]
    if numeric_values:
        span = math.log10(max(numeric_values) / min(numeric_values))
        value_span = min(1.0, max(0.0, span) / 18.0)
    else:
        value_span = 0.0

    return np.asarray([
        _scaled_count(len(logical_lines), 250.0),
        _scaled_count(device_count, 200.0),
        _scaled_count(len(nodes), 200.0),
        nonlinear_count / denominator,
        device_counts["r"] / denominator,
        device_counts["c"] / denominator,
        device_counts["l"] / denominator,
        device_counts["v"] / denominator,
        device_counts["i"] / denominator,
        device_counts["d"] / denominator,
        device_counts["q"] / denominator,
        device_counts["m"] / denominator,
        device_counts["x"] / denominator,
        _scaled_count(model_count, 20.0),
        float(has_initial_hint),
        value_span,
    ], dtype=np.float32)


def build_a2c_state(selection_counts, reward_ema, netlist_features):
    counts = np.asarray(selection_counts, dtype=np.float32)
    rewards = np.asarray(reward_ema, dtype=np.float32)
    total = max(1.0, float(counts.sum()))
    frequencies = counts / total
    bounded_rewards = np.tanh(rewards / 6.0)
    return np.concatenate((frequencies, bounded_rewards, netlist_features)).astype(np.float32)

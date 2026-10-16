import math
import re
import hashlib
from collections import Counter
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
    "resistor_span", "capacitor_span", "inductor_span", "voltage_span", "current_span",
    "geometry_span", "weak_bias_fraction", "control_coupling_fraction",
    "resistor_scale", "capacitor_scale", "inductor_scale", "voltage_scale", "current_scale", "geometry_scale",
    "model_is_scale", "model_gain_scale", "model_kp_scale", "model_threshold_scale",
)

SOLVER_FEATURE_NAMES = (
    "feedback_available", "ng_nr", "ng_gmin", "ng_source", "ng_optran", "ng_failed",
    "ng_iterations", "ng_iterations_available", "pta_verified", "pta_steps",
    "pta_steps_available", "pta_rejected_fraction", "pta_residual", "pta_residual_available",
    "pta_derivative", "pta_derivative_available",
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
    model_values = {key: [] for key in ("is", "bf", "br", "kp", "vto")}
    values_by_type = {kind: [] for kind in ("r", "c", "l", "v", "i", "geometry")}
    control_nodes = []

    # The first non-directive line is the SPICE title and not an element.
    title_consumed = False
    for line in logical_lines:
        lower = line.lower()
        tokens = re.sub(r"\s*=\s*", "=", line.replace("(", " ").replace(")", " ")).split()
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
                        key = token.split("=", 1)[0].lower()
                        if lower.startswith(".model") and key in model_values:
                            model_values[key].append(abs(value))
            continue
        kind = tokens[0][0].lower()
        if kind not in device_counts:
            continue
        device_counts[kind] += 1
        nodes.update(node.lower() for node in _element_nodes(tokens) if node != "0")
        if kind in {"q", "m"} and len(tokens) > 2:
            control_nodes.append(tokens[2].lower())

        # Values in name=value fields are never node identifiers.  The fourth
        # token is the scalar value for primitive R/C/L/V/I forms.
        for token in tokens[1:]:
            if "=" in token:
                value = _parse_number(token)
                if value is not None and math.isfinite(value) and value != 0:
                    if kind == "m" and token.lower().startswith(("w=", "l=")):
                        values_by_type["geometry"].append(abs(value))
        if kind in {"r", "c", "l", "v", "i"} and len(tokens) >= 4:
            value_token = tokens[4] if len(tokens) >= 5 and tokens[3].lower() == "dc" else tokens[3]
            value = _parse_number(value_token)
            if value is not None and math.isfinite(value) and value != 0:
                values_by_type[kind].append(abs(value))

    device_count = sum(device_counts.values())
    denominator = max(1, device_count)
    nonlinear_count = device_counts["d"] + device_counts["q"] + device_counts["m"]

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
        *[min(1.0, (math.log10(max(values)) - math.log10(min(values))) / 18.0)
          if values else 0.0 for values in values_by_type.values()],
        sum(value >= 1e8 for value in values_by_type["r"]) / max(1, device_counts["r"]),
        (len(control_nodes) - len(set(control_nodes))) / max(1, len(control_nodes)),
        *[min(1.0, max(0.0, (math.log10(float(np.median(values))) - low) / (high - low)))
          if values else 0.0 for values, (low, high) in zip(values_by_type.values(),
              ((-3, 15), (-18, 0), (-15, 6), (-9, 9), (-15, 6), (-12, 0)))],
        *[min(1.0, max(0.0, (math.log10(float(np.median(values))) - low) / (high - low)))
          if values else 0.0 for values, (low, high) in zip(
              (model_values["is"], model_values["bf"] + model_values["br"], model_values["kp"]),
              ((-30, -3), (0, 6), (-12, 3)))],
        min(1.0, float(np.median(model_values["vto"])) / 10.0) if model_values["vto"] else 0.0,
    ], dtype=np.float32)


def build_a2c_state(selection_counts, reward_ema, netlist_features, solver_features=None):
    counts = np.asarray(selection_counts, dtype=np.float32)
    rewards = np.asarray(reward_ema, dtype=np.float32)
    total = max(1.0, float(counts.sum()))
    frequencies = counts / total
    bounded_rewards = np.tanh(rewards / 6.0)
    solver_features = np.zeros(len(SOLVER_FEATURE_NAMES), dtype=np.float32) if solver_features is None else solver_features
    return np.concatenate((frequencies, bounded_rewards, netlist_features, solver_features)).astype(np.float32)


def extract_solver_features(evaluation):
    """Only call with a previously evaluated parent, never its future child."""
    values = dict.fromkeys(SOLVER_FEATURE_NAMES, 0.0)
    if evaluation is None:
        return np.asarray(list(values.values()), dtype=np.float32)
    values["feedback_available"] = 1.0
    ng = evaluation.ngspice
    if ng:
        key = {"nr": "ng_nr", "gmin": "ng_gmin", "source_stepping": "ng_source", "optran": "ng_optran"}.get(ng.stage.value)
        if ng.succeeded and key:
            values[key] = 1.0
        values["ng_failed"] = float(ng.artifacts.get("failure_kind") == "convergence")
        iterations = ng.artifacts.get("iterations")
        if iterations is not None:
            values["ng_iterations_available"] = 1.0
            values["ng_iterations"] = _scaled_count(iterations, 10000.0)
    successes = [o for o in evaluation.outcomes.values() if o.verified_pta]
    if successes:
        evidence = successes[0].artifacts["validation"]
        values["pta_verified"] = 1.0
        accepted, rejected = evidence.get("accepted_steps"), evidence.get("rejected_steps")
        if accepted is not None and rejected is not None:
            values["pta_steps_available"] = 1.0
            values["pta_steps"] = _scaled_count(accepted, 10000.0)
            values["pta_rejected_fraction"] = rejected / max(1, accepted + rejected)
        for key, source in (("pta_residual", "normalized_dc_residual"), ("pta_derivative", "normalized_derivative")):
            if evidence.get(source) is not None:
                values[key] = min(1.0, max(0.0, evidence[source]))
                values[key + "_available"] = 1.0
    return np.asarray(list(values.values()), dtype=np.float32)


def structural_signature(path):
    """Two graph-color refinements invariant to names, comments and row order."""
    rows, node_labels = [], {"0": "ground", "gnd": "ground"}
    for raw in Path(path).read_text(errors="replace").splitlines()[1:]:
        tokens = re.split(r";|\$|//", raw, maxsplit=1)[0].lower().split()
        if not tokens or tokens[0].startswith((".", "*", "+")):
            continue
        nodes = _element_nodes(tokens)
        if nodes:
            rows.append((tokens[0][0], nodes))
            for node in nodes:
                node_labels.setdefault(node, "node")
    colors = [kind for kind, _ in rows]
    for _ in range(2):
        colors = [hashlib.sha256((kind + ":" + ",".join(node_labels[node] for node in nodes)).encode()).hexdigest()[:16]
                  for kind, nodes in rows]
        neighborhoods = {node: [] for node in node_labels}
        for color, (_, nodes) in zip(colors, rows):
            for role, node in enumerate(nodes):
                neighborhoods[node].append(f"{role}:{color}")
        node_labels = {node: "ground" if node in {"0", "gnd"} else hashlib.sha256("|".join(sorted(items)).encode()).hexdigest()[:16]
                       for node, items in neighborhoods.items()}
    return Counter(colors)


def circuit_novelty(path, reference_paths):
    signature, features = structural_signature(path), extract_netlist_features(Path(path))
    distances = []
    for reference in reference_paths:
        other = structural_signature(reference)
        union = sum((signature | other).values())
        graph = 1.0 - sum((signature & other).values()) / max(1, union)
        numeric = float(np.mean(np.abs(features - extract_netlist_features(Path(reference)))))
        distances.append(0.8 * graph + 0.2 * numeric)
    return min(distances) if distances else 1.0

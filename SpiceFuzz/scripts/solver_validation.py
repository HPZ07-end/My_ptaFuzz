"""Validate solver evidence without changing either simulator's equations."""

import json
import math
import re
from pathlib import Path


def read_operating_point(path):
    """Read one complete, real ASCII OP plot; reject headers/stale partial output."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="strict")
        head, payload = re.split(r"(?im)^Values:\s*\n", text, maxsplit=1)
        def field(name):
            match = re.search(r"(?im)^" + re.escape(name) + r":\s*(.+)$", head)
            if not match:
                raise ValueError(f"missing {name}")
            return match.group(1).strip()
        if field("Plotname").lower() != "operating point":
            raise ValueError("not an operating-point plot")
        if field("Flags").lower().split() != ["real"]:
            raise ValueError("expected real ASCII OP values")
        count, points = int(field("No. Variables")), int(field("No. Points"))
        if count < 1 or points != 1:
            raise ValueError("expected exactly one nonempty OP point")
        variable_text = re.split(r"(?im)^Variables:\s*\n", head, maxsplit=1)[1]
        variables = [line.split() for line in variable_text.splitlines() if line.strip()]
        if len(variables) != count:
            raise ValueError("incomplete variable table")
        tokens = payload.split()
        if len(tokens) != count + 1 or tokens[0] != "0":
            raise ValueError("incomplete or extra OP values")
        solution, kinds = {}, {}
        for index, (entry, token) in enumerate(zip(variables, tokens[1:])):
            if len(entry) != 3 or int(entry[0]) != index:
                raise ValueError("invalid variable index")
            name, kind = entry[1].lower(), entry[2].lower()
            if name in solution or kind not in {"voltage", "current"}:
                raise ValueError("duplicate variable or invalid physical quantity")
            value = float(token)
            if not math.isfinite(value):
                raise ValueError("non-finite OP value")
            solution[name], kinds[name] = value, kind
        if "voltage" not in kinds.values():
            raise ValueError("OP has no node voltage")
        return {"valid": True, "solution": solution, "kinds": kinds, "reason": ""}
    except (OSError, UnicodeError, ValueError, IndexError) as error:
        return {"valid": False, "solution": {}, "kinds": {}, "reason": str(error)}


def solutions_agree(first, second, relative=1e-4):
    if not first or first.keys() != second.keys():
        return False
    for name, value in first.items():
        absolute = 1e-9 if name.startswith("i(") or "#branch" in name else 1e-6
        if not math.isclose(value, second[name], rel_tol=relative, abs_tol=absolute):
            return False
    return True


def validate_pta_artifacts(artifacts):
    """Require a successful report, complete raw, and actual PTA steady state."""
    op = read_operating_point(artifacts["raw"])
    evidence = {"op_valid": op["valid"], "solution": op["solution"],
                "verified_pta": False, "reason": op["reason"], "actual_stage": "unknown"}
    try:
        report = Path(artifacts["report"]).read_text(errors="replace")
        if not re.search(r"(?m)^Status: succeeded\s*$", report):
            raise ValueError("missing successful solver report")
        method = re.search(r"(?m)^\s*Method path:\s*(.+)$", report)
        if not method:
            raise ValueError("missing actual method path")
        path = method.group(1).lower()
        stage = ("pta" if "pta succeeded" in path else
                 "source_stepping" if "source stepping succeeded" in path else
                 "nr" if ("newton succeeded" in path or "newton-raphson succeeded" in path
                          or "direct sparse linear solve succeeded" in path) else "unknown")
        evidence["actual_stage"] = stage
        evidence["method_path"] = method.group(1)
        total = re.search(r"(?m)^\s*Total iterations:\s*(\d+)", report)
        evidence["iterations"] = int(total.group(1)) if total else None
        if not op["valid"]:
            raise ValueError(op["reason"])
        if stage == "unknown":
            raise ValueError("successful report has unknown method")
        if stage == "pta":
            last_attempt, final = None, None
            with Path(artifacts["trace"]).open(encoding="utf-8") as stream:
                for line in stream:
                    record = json.loads(line)
                    if record.get("record_type") == "attempt":
                        last_attempt = record
                    elif record.get("record_type") == "final":
                        final = record
            if not final or final.get("converged") is not True or final.get("status") != "succeeded":
                raise ValueError("missing successful final PTA trace")
            if not last_attempt or last_attempt.get("reached_steady_state") is not True:
                raise ValueError("PTA did not reach steady state")
            if not last_attempt.get("accepted") or not last_attempt.get("newton", {}).get("converged"):
                raise ValueError("PTA endpoint Newton solve failed")
            for key, option in (("normalized_derivative", "derivative_tolerance"),
                                ("normalized_dc_residual", "dc_residual_tolerance")):
                match = re.search(r"(?m)^\s*pta\." + option + r":\s*(\S+)", report)
                tolerance = float(match.group(1)) if match else None
                value = last_attempt.get(key)
                if (tolerance is None or not math.isfinite(tolerance) or tolerance <= 0
                        or not isinstance(value, (float, int)) or not math.isfinite(value)
                        or value < 0 or value > tolerance):
                    raise ValueError(f"PTA {key} exceeds effective tolerance")
                evidence[key] = value
            source_scale = last_attempt.get("source_scale")
            if not isinstance(source_scale, (float, int)) or not math.isclose(source_scale, 1.0):
                raise ValueError("PTA stopped before full source ramp")
            trace_solution = {}
            for key, prefix in (("node_voltages", "v"), ("branch_currents", "i")):
                for item in final.get("solution", {}).get(key, []):
                    trace_solution[f"{prefix}({item['name'].lower()})"] = item["value"]
            if not solutions_agree(op["solution"], trace_solution):
                raise ValueError("raw OP and final PTA solution disagree")
            evidence["accepted_steps"] = None
            evidence["rejected_steps"] = None
            for name, label in (("accepted_steps", "Accepted"), ("rejected_steps", "Rejected")):
                match = re.search(r"(?m)^\s*" + label + r" pseudo-time steps:\s*(\d+)", report)
                evidence[name] = int(match.group(1)) if match else None
            evidence["verified_pta"] = True
        evidence["reason"] = ""
        return True, evidence
    except (OSError, ValueError, KeyError, TypeError) as error:
        evidence["reason"] = str(error)
        return False, evidence

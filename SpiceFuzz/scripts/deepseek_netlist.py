import json
import re


BEGIN_NETLIST = "BEGIN_NETLIST"
END_NETLIST = "END_NETLIST"
_FENCED_BLOCK = re.compile(
    r"```[ \t]*(?:spice|cir|netlist|plaintext)?[ \t]*\r?\n([\s\S]*?)```",
    re.IGNORECASE,
)
_GENERIC_FENCED_BLOCK = re.compile(r"```([\s\S]*?)```")
_DIRECTIVE = re.compile(
    r"^\.(?:model|subckt|ends|nodeset|ic|op|print)\b",
    re.IGNORECASE,
)
_OP = re.compile(r"^\.op(?:\s|$)", re.IGNORECASE)
_END = re.compile(r"^\.end(?:\s|$)", re.IGNORECASE)
_ELEMENT_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_.$:+-]+$")


def _is_element(line):
    tokens = line.split()
    if not tokens or not _ELEMENT_NAME.fullmatch(tokens[0]):
        return False
    minimum_tokens = {
        "r": 4,
        "c": 4,
        "l": 4,
        "v": 4,
        "i": 4,
        "d": 4,
        "q": 5,
        "m": 6,
        "x": 4,
    }
    kind = tokens[0][0].lower()
    return kind in minimum_tokens and len(tokens) >= minimum_tokens[kind]


def _is_netlist_line(line):
    return bool(
        _is_element(line)
        or _DIRECTIVE.match(line)
        or line.startswith(("+", "*"))
    )


def _json_string_candidates(reply):
    """Yield string leaves from a JSON-wrapped model response."""
    stripped = reply.strip()
    if not stripped.startswith(("{", "[", '"')):
        return []
    try:
        value = json.loads(stripped)
    except (TypeError, ValueError):
        return []

    strings = []

    def visit(item):
        if isinstance(item, str):
            strings.append(item)
        elif isinstance(item, dict):
            preferred = ("netlist", "content", "output", "code")
            for key in preferred:
                if key in item:
                    visit(item[key])
            for key, child in item.items():
                if key not in preferred:
                    visit(child)
        elif isinstance(item, list):
            for child in item:
                visit(child)

    visit(value)
    return strings


def _normalize_candidate(candidate):
    candidate = re.sub(
        r"<think>[\s\S]*?</think>",
        "",
        candidate,
        flags=re.IGNORECASE,
    )
    lines = [line.strip() for line in candidate.splitlines() if line.strip()]
    if not lines:
        return None, "empty candidate"
    if lines[0].lower() in {"spice", "cir", "netlist", "plaintext"}:
        lines.pop(0)

    first_syntax = None
    end_index = None
    for index, line in enumerate(lines):
        if first_syntax is None and (_is_element(line) or _DIRECTIVE.match(line)):
            first_syntax = index
        if first_syntax is not None and _END.match(line):
            end_index = index
            break
    if first_syntax is None:
        return None, "no supported SPICE element or directive found"

    if end_index is None:
        last_syntax = max(
            index
            for index, line in enumerate(lines[first_syntax:], first_syntax)
            if _is_netlist_line(line)
        )
        body = lines[first_syntax:last_syntax + 1]
    else:
        body = lines[first_syntax:end_index + 1]

    element_count = sum(_is_element(line) for line in body)
    if element_count == 0:
        return None, "no supported circuit element found"

    # Missing or duplicated analysis terminators are serialization defects,
    # not circuit mutations. Repair them locally, then let PTA's parser remain
    # the authority on whether the resulting deck is valid.
    repaired = []
    op_seen = False
    for line in body:
        if _OP.match(line):
            if op_seen:
                continue
            op_seen = True
        if not _END.match(line):
            repaired.append(line)
    if not op_seen:
        repaired.append(".op")
    repaired.append(".end")

    return (
        "SpiceFuzz generated DC operating-point netlist\n"
        + "\n".join(repaired)
        + "\n"
    ), "ok"


def extract_deepseek_netlist_with_diagnostic(reply):
    """Return a normalized deck and an actionable extraction diagnostic."""
    if not reply or not reply.strip():
        return None, "DeepSeek returned empty content"
    candidates = []
    marker_pattern = re.compile(
        rf"{BEGIN_NETLIST}\s*([\s\S]*?)\s*{END_NETLIST}",
        re.IGNORECASE,
    )
    candidates.extend(marker_pattern.findall(reply))
    candidates.extend(_FENCED_BLOCK.findall(reply))
    candidates.extend(_GENERIC_FENCED_BLOCK.findall(reply))
    candidates.extend(_json_string_candidates(reply))
    candidates.append(reply)
    diagnostics = []
    for candidate in candidates:
        normalized, diagnostic = _normalize_candidate(candidate)
        if normalized is not None:
            return normalized, diagnostic
        diagnostics.append(diagnostic)
    useful = next(
        (item for item in diagnostics if item != "empty candidate"),
        "no candidate block found",
    )
    return None, useful


def extract_deepseek_netlist(reply):
    """Accept marker blocks, Markdown fences, JSON, or a plain-text deck."""
    return extract_deepseek_netlist_with_diagnostic(reply)[0]


def build_deepseek_format_repair_prompt():
    return f"""Your previous response could not be parsed as a complete SPICE DC operating-point netlist. Regenerate the full candidate from the original input; do not merely continue the truncated response.

Reformat the same candidate without changing its circuit. Return only this structure:
{BEGIN_NETLIST}
SpiceFuzz generated DC operating-point netlist
<complete SPICE deck containing exactly one .op>
.end
{END_NETLIST}

Do not use Markdown backticks, a language tag, or explanatory prose.
"""

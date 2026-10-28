PTA_MUTATOR_SYSTEM_PROMPT = """You are a SPICE netlist mutation engine for numerical-convergence research.

Generate one physically meaningful DC operating-point test case that can be replayed unchanged under several solver ablations. The purpose is to stress nonlinear convergence naturally through circuit topology and parameter scales, never by sabotaging a solver configuration.

Follow the requested mutation strategy exactly, make the smallest coherent change needed, and preserve unrelated behavior. A separate evaluator determines whether a solver converges; never claim or fabricate an outcome.
"""


COMMON_NETLIST_CONTRACT = """Common-netlist contract:
1. Return exactly one complete netlist between literal BEGIN_NETLIST and END_NETLIST lines. Do not use Markdown backticks, language tags, or explanatory prose.
2. The first physical line must be exactly "SpiceFuzz generated DC operating-point netlist" so the existing cleaner preserves it as the title. Include exactly one .op analysis and place .end as the final non-comment statement.
3. Use only R, C, L, independent DC V/I, D, Q, M, and X elements. Models may be D, NPN, PNP, NMOS, PMOS, NCH, or PCH using only parameters supported by the supplied syntax guidance.
4. After the required title line, allowed directives are .model, .subckt/.ends, .op, .nodeset, .ic, .print op, and .end.
5. Do not use .pstran, .dc, .tran, .ac, .pz, .noise, .option/.options, .include, .lib, global .param, .func, .if, controlled or behavioral sources, JFETs, switches, or VDMOS.
6. Do not add iteration limits, tolerances, gmin settings, source-stepping settings, PTA settings, or any other solver-specific control. All ablations must receive the identical deck.
7. The circuit must be DC well posed: every node has a finite DC path, model and instance values are finite, required positive values stay positive, ideal voltage sources do not conflict, and there are no ideal-source loops or deliberately singular/floating structures.
8. Prefer genuine convergence stressors such as regenerative feedback, near symmetry, multiple equilibria, steep nonlinear transitions, high gain, and multi-decade but finite parameter scales. Syntax errors, unsupported features, NaN/Inf, and structurally singular circuits are invalid results.
"""


def build_pta_mutation_prompt(seed, mutation, guidance, allow_initial_hint_mutations=False, solver_feedback=""):
    guidance_text = guidance.strip() if guidance else (
        "Apply the named mutation using only the common-netlist contract."
    )
    return f"""Mutation strategy:
{mutation}

Strategy-specific guidance:
{guidance_text}

{COMMON_NETLIST_CONTRACT}

Input netlist:
```
{seed.rstrip()}
```

Initial-condition policy:
{"Initial hints may be mutated in this explicitly enabled experiment." if allow_initial_hint_mutations else "Preserve all existing .nodeset, .ic, device IC= and OFF hints exactly. Do not add new hints."}

Measured parent feedback (observations only; do not fabricate solver outcomes):
{solver_feedback or "No previous solver feedback."}

Produce the single mutated netlist now using the required BEGIN_NETLIST and END_NETLIST lines.
"""


def build_pta_error_feedback(error_message):
    return f"""The previous candidate was rejected by the syntax or simulation evaluator.

Diagnostic:
{error_message}

Treat this diagnostic only as a constraint for the next mutation. Do not add solver-specific directives to suppress it; instead produce a valid, physically well-posed netlist that follows the current mutation strategy and common-netlist contract.
"""

"""Standard-library-only checks for the offline replay entry point."""

import ast
import runpy
import subprocess
import sys
import typing
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.dc_evaluation import DcEvaluation, SolverOutcome


ROOT = Path(__file__).resolve().parents[1]


class OfflineCompatibilityTests(unittest.TestCase):
    def test_optional_outcome_annotation_is_runtime_compatible(self):
        hints = typing.get_type_hints(DcEvaluation)
        self.assertEqual(hints["ngspice"], typing.Optional[SolverOutcome])

    def test_offline_import_chain_accepts_python38_grammar(self):
        for name in ("evaluate_pta.py", "scripts/dc_evaluation.py", "scripts/pta_pipeline.py",
                     "scripts/pta_testing.py", "scripts/solver_validation.py",
                     "scripts/ngspice_testing.py", "scripts/pta_reward.py"):
            with self.subTest(module=name):
                tree = ast.parse((ROOT / name).read_text(encoding="utf-8"), feature_version=8)
                # PEP 604 is syntactically a binary expression even on older
                # parsers, so grammar validation alone cannot catch this bug.
                annotations = [node.annotation for node in ast.walk(tree)
                               if isinstance(node, ast.AnnAssign)]
                self.assertFalse(any(isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr)
                                     for annotation in annotations for node in ast.walk(annotation)))

    def test_cli_help_needs_no_solver_or_model_credentials(self):
        result = subprocess.run([sys.executable, "-B", str(ROOT / "evaluate_pta.py"), "--help"],
                                cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--ngspice_bin", result.stdout)

    def test_old_interpreter_gets_clear_error_before_project_imports(self):
        with patch.object(sys, "version_info", (3, 7, 0)):
            with self.assertRaisesRegex(SystemExit, "Python 3.8"):
                runpy.run_path(str(ROOT / "evaluate_pta.py"), run_name="__main__")


if __name__ == "__main__":
    unittest.main()

"""Black-box launcher tests: no planner, model weights or tmux daemon needed."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_depots_extrapolation_45_50.sh"
LAUNCHER = ROOT / "scripts" / "start_depots_extrapolation_45_50_tmux.sh"


def bash_path(path: Path) -> str:
    value = path.resolve().as_posix()
    if os.name == "nt":
        return "/" + value[0].lower() + value[2:]
    return value


def find_bash() -> str | None:
    if os.name == "nt":
        candidate = Path("C:/Program Files/Git/bin/bash.exe")
        return str(candidate) if candidate.exists() else None
    return shutil.which("bash")


@unittest.skipUnless(find_bash(), "Bash is required to exercise the Linux entry points")
class ExtrapolationShellTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="count-shell-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.capture = self.work / "arguments.txt"
        self.cwd_capture = self.work / "cwd.txt"
        self.status_capture = self.work / "status.txt"
        self.interpreter_capture = self.work / "interpreter.txt"
        self.bash = find_bash()
        self.env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("COUNT_", "CONDA_", "NLM_", "LD_"))
        }
        self.env.update(
            COUNT_PROJECT_ROOT=bash_path(ROOT),
            COUNT_DATASET_ROOT="/data/depots 45 and 50",
            COUNT_MODEL_PATH="/weights/frozen checkpoint",
            COUNT_RESULTS_DIR=bash_path(self.work / "results with spaces"),
            COUNT_PLANNER_PYTHON=bash_path(self.bin / "fake-python"),
            COUNT_RUNTIME_ENV_PREFIX="/missing/runtime",
            COUNT_RUNTIME_LIB_DIR="/missing/libraries",
            COUNT_LLM_PARTIAL_ONLY="1",
        )
        # Git Bash's command search expects Unix separators when PATH is set
        # from Python. Its /usr/bin contains bash, tee, env and the other tools.
        if os.name == "nt":
            self.env["PATH"] = bash_path(self.bin) + ":/usr/bin:/bin"
        else:
            self.env["PATH"] = str(self.bin) + os.pathsep + self.env["PATH"]
        self.write_python_stub()

    def executable(self, name: str, source: str) -> Path:
        path = self.bin / name
        path.write_text(source, encoding="utf-8", newline="\n")
        path.chmod(0o755)
        return path

    def write_python_stub(self, exit_status: int = 0) -> None:
        self.executable(
            "fake-python",
            "#!/usr/bin/env bash\n"
            f"printf '%s\\n' \"$@\" > {shlex.quote(bash_path(self.capture))}\n"
            f"pwd > {shlex.quote(bash_path(self.cwd_capture))}\n"
            f"printf '%s\\n' \"$0\" > {shlex.quote(bash_path(self.interpreter_capture))}\n"
            f"exit {exit_status}\n",
        )

    def run_script(self, path: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [self.bash, bash_path(path), *arguments],
            env=self.env,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    def captured_arguments(self) -> list[str]:
        return self.capture.read_text(encoding="utf-8").splitlines()

    def argument_value(self, arguments: list[str], flag: str) -> str:
        return arguments[arguments.index(flag) + 1]

    def test_foreground_prepare_preserves_paths_and_fixed_policy(self) -> None:
        self.env.update(COUNT_EXPERIMENT_SEED="202", COUNT_SAMPLE_SEED="303")
        result = self.run_script(RUNNER, "--prepare-only", "--parallelism", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.captured_arguments()
        self.assertEqual(args[:2], ["-m", "hybrid_planner.extrapolation"])
        self.assertEqual(self.argument_value(args, "--dataset-root"), "/data/depots 45 and 50")
        self.assertEqual(self.argument_value(args, "--model-path"), "/weights/frozen checkpoint")
        self.assertEqual(self.argument_value(args, "--experiment-seed"), "202")
        self.assertEqual(self.argument_value(args, "--sample-seed"), "303")
        self.assertEqual(args[-2:], ["--parallelism", "6"])
        self.assertIn("--prepare-only", args)
        self.assertIn("llm_partial_only=0", result.stdout)
        self.assertNotIn("--llm-partial-only", args)

    def test_tmux_forwards_current_environment_and_records_failure(self) -> None:
        self.env.update(
            COUNT_TMUX_SESSION="test-extrapolation",
            COUNT_EXTRAPOLATION_STAGE="live",
            COUNT_SAMPLE_PER_SCALE="7",
            COUNT_SAMPLE_SEED="606",
            COUNT_EXPERIMENT_SEED="505",
            COUNT_VLLM_GPUS="2,3",
            COUNT_LLM_MODEL_NAME="frozen source model",
        )
        self.write_python_stub(exit_status=7)
        self.executable(
            "tmux",
            "#!/usr/bin/env bash\n"
            "case \"$1\" in\n"
            "  has-session) exit 1 ;;\n"
            "  new-session)\n"
            # Simulate a stale tmux server: all current COUNT/Conda settings
            # disappear unless the wrapper embeds them in its launch command.
            "    env -i PATH=/usr/bin:/bin bash -c \"${@: -1}\"\n"
            "    run_status=$?\n"
            f"    printf '%s\\n' \"$run_status\" > {shlex.quote(bash_path(self.status_capture))}\n"
            "    exit 0 ;;\n"
            "  *) exit 9 ;;\n"
            "esac\n",
        )
        result = self.run_script(LAUNCHER, "--prepare-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.captured_arguments()
        self.assertEqual(self.argument_value(args, "--stage"), "live")
        self.assertEqual(self.argument_value(args, "--sample-per-scale"), "7")
        self.assertEqual(self.argument_value(args, "--sample-seed"), "606")
        self.assertEqual(self.argument_value(args, "--experiment-seed"), "505")
        self.assertEqual(self.argument_value(args, "--vllm-gpus"), "2,3")
        self.assertEqual(self.argument_value(args, "--llm-model"), "frozen source model")
        self.assertEqual(self.status_capture.read_text().strip(), "7")
        transcript = (self.work / "results with spaces" / "tmux-run.log").read_text()
        self.assertIn("status=7", transcript)

    def test_prepare_and_run_select_the_same_auto_detected_python(self) -> None:
        prefix = self.work / "model environment"
        prefix_bin = prefix / "bin"
        prefix_bin.mkdir(parents=True)
        runtime_lib = prefix / "lib"
        runtime_lib.mkdir()
        (runtime_lib / "libstdc++.so.6").write_text("CXXABI_1.3.15", encoding="utf-8")
        for name, source in (
            ("python3", (self.bin / "fake-python").read_text(encoding="utf-8")),
            ("vllm", "#!/usr/bin/env bash\nexit 0\n"),
        ):
            target = prefix_bin / name
            target.write_text(source, encoding="utf-8", newline="\n")
            target.chmod(0o755)
        for variable in ("COUNT_RUNTIME_ENV_PREFIX", "COUNT_RUNTIME_LIB_DIR", "COUNT_PLANNER_PYTHON"):
            self.env.pop(variable, None)
        self.env["CONDA_PREFIX"] = bash_path(prefix)

        prepared = self.run_script(RUNNER, "--prepare-only")
        self.assertEqual(prepared.returncode, 0, prepared.stderr)
        prepared_interpreter = self.interpreter_capture.read_text().strip()
        prepared_arguments = self.captured_arguments()
        self.assertEqual(prepared_interpreter, bash_path(prefix_bin / "python3"))
        normal = self.run_script(RUNNER)
        self.assertEqual(normal.returncode, 0, normal.stderr)
        self.assertEqual(self.interpreter_capture.read_text().strip(), prepared_interpreter)
        self.assertEqual(self.captured_arguments(), [arg for arg in prepared_arguments if arg != "--prepare-only"])

    def test_shell_syntax_and_lf(self) -> None:
        for path in (RUNNER, LAUNCHER):
            with self.subTest(path=path.name):
                self.assertNotIn(b"\r", path.read_bytes())
                result = subprocess.run(
                    [self.bash, "-n", bash_path(path)], capture_output=True, text=True, check=False
                )
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()

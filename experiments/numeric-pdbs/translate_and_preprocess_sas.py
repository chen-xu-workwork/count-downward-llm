import os
import subprocess
import sys
import time
import lab

from lab.experiment import Experiment
from lab.environments import TetralithEnvironment


def run_command_with_timeout(command, timeout_seconds):
    """
    Runs a shell command with a specified timeout.

    Args:
        command (str): The shell command to execute.
        timeout_seconds (int): The maximum time (in seconds) to wait for the command to complete.

    Returns:
        subprocess.CompletedProcess: An object containing information about the completed process
                                     (return code, stdout, stderr).
        None: If the command times out.
    """
    try:
        result = subprocess.run(
            command,
            shell=True,  # Be cautious when shell=True with untrusted input
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
        )
        return result
    except subprocess.TimeoutExpired:
        print(
            f"Command '{command}' timed out after {timeout_seconds} seconds.",
            flush=True,
        )
        return None
    except FileNotFoundError:
        print(f"Error: Command '{command}' not found.", flush=True)
        return None
    except Exception as e:
        print(f"An unexpected error occurred: {e}", flush=True)
        return None


def main():
    ENV = TetralithEnvironment(
        email="markus.fritzsche@liu.se",
        memory_per_cpu="8300M",
        extra_options="#SBATCH -A naiss2024-5-404",  # parground
    )
    exp = Experiment(environment=ENV)
    benchmark_dirs = []
    benchmark_dirs.append(os.environ["NUMERIC_BENCHMARKS_IPC2023"])
    benchmark_dirs.append(os.environ["NUMERIC_BENCHMARKS_OTHERS"])
    fd_path = "/proj/parground/users/x_mfrit/numeric-fd/fast-downward.py"

    for benchmark_dir in benchmark_dirs:
        pddl_benchmarks = os.listdir(benchmark_dir)
        pddl_benchmarks = [
            os.path.join(benchmark_dir, benchmark)
            for benchmark in pddl_benchmarks
            if "_sas" not in benchmark
        ]
        sas_benchmarks = [b + "_sas" for b in pddl_benchmarks]
        for pddl_benchmark, sas_benchmark in zip(pddl_benchmarks, sas_benchmarks):
            if not os.path.exists(sas_benchmark):
                os.makedirs(sas_benchmark, exist_ok=True)
                print(f"Creating {sas_benchmark}", flush=True)
            else:
                print(
                    f"{sas_benchmark} already exists, removing it recursively",
                    flush=True,
                )
                os.system(f"rm -rf {sas_benchmark}")
                os.makedirs(sas_benchmark, exist_ok=True)

            domain_file = os.path.join(pddl_benchmark, "domain.pddl")
            for file in os.listdir(pddl_benchmark):
                if file.endswith(".pddl") and file != "domain.pddl":
                    pddl_file = os.path.join(pddl_benchmark, file)
                    sas_file = os.path.join(
                        sas_benchmark, file.replace(".pddl", ".sas")
                    )
                    print(pddl_file, flush=True)
                    print(sas_file, flush=True)
                    print()

                    run = exp.add_run()
                    run.add_resource("domain", domain_file)
                    run.add_resource("problem", pddl_file)

                    run.set_property("time_limit", 1800)  # 30 minutes
                    run.set_property("memory_limit", 3072)

                    run.set_property("sas_output", sas_file)

                    translate_command = [
                        sys.executable,
                        fd_path,
                        "--build",
                        "release64",
                        "--translate",
                        "{domain}",
                        "{problem}",
                    ]

                    run.add_command(
                        "translate",
                        translate_command,
                        time_limit=1800,
                        memory_limit=3072,
                    )

                    preprocess_command = [
                        sys.executable,
                        fd_path,
                        "--build",
                        "release64",
                        "--preprocess",
                        "output.sas",
                    ]
                    print(preprocess_command)
                    run.add_command(
                        "preprocess",
                        preprocess_command,
                        time_limit=1800,
                        memory_limit=3072,
                    )

                    mv_command = [
                        "mv",
                        "output",
                        sas_file,
                    ]
                    run.add_command(
                        "mv",
                        mv_command,
                        time_limit=1800,
                        memory_limit=3072,
                    )

                    run.set_property("id", [sas_file])

    exp.add_step("build", exp.build)

    # Add step that executes all runs.
    exp.add_step("start", exp.start_runs)

    exp.run_steps()

    print("Done!", flush=True)


if __name__ == "__main__":
    main()

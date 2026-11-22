#!/usr/bin/env python3
"""Run the stable, isolated header and module consumer checks for CMake, Bazel and xmake."""

import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
TOOLS = ("cmake", "bazel", "xmake")
BACKENDS = {"Darwin": ("kqueue",), "Linux": ("epoll", "io_uring")}


def brew_driver(formula, binary):
    if shutil.which("brew"):
        found = subprocess.run(["brew", "--prefix", formula], text=True, capture_output=True)
        candidate = Path(found.stdout.strip()) / "bin" / binary
        if found.returncode == 0 and candidate.is_file():
            return str(candidate)
    return binary


def tool_command(tool, backend, args, system):
    env = {}
    # The Fedora dev image pins a second libstdc++ for GCC 16.1 reflection.
    # Its distribution Clang and installed stable dependencies use /usr/lib64.
    library_path = os.getenv("LD_LIBRARY_PATH", "")
    if system == "Linux" and "/opt/gcc-16.1/usr/lib64" in library_path.split(":"):
        env["LD_LIBRARY_PATH"] = "/usr/lib64:" + library_path
    command = [sys.executable, str(ROOT / "scripts" / f"verify_{tool}_consumers.py")]
    if tool == "bazel":
        compiler = args.bazel_cc or (
            brew_driver("llvm@22", "clang") if system == "Darwin" else "clang"
        )
        compiler = shutil.which(compiler) or compiler
        env["CC"] = compiler
        driver = args.bazel or ("bazel" if shutil.which("bazel") else "bazelisk")
        command.extend(["--bazel", driver, "--io-uring", "on" if backend == "io_uring" else "off"])
    else:
        compiler = args.compiler or (
            brew_driver("llvm", "clang++") if system == "Darwin" else "clang++"
        )
        compiler = shutil.which(compiler) or compiler
        command.extend(["--compiler", compiler])
        if backend == "io_uring":
            command.append("--io-uring")
        if tool == "xmake" and args.xmake_offline:
            command.append("--offline")
    # Bazel and xmake find companion drivers through PATH, including clang-scan-deps.
    if os.path.isabs(compiler):
        env["PATH"] = str(Path(compiler).parent) + os.pathsep + os.environ.get("PATH", "")
    return command, env


def container_command(backend, tools, args):
    command = ["docker", "run", "--rm", "--cap-drop=ALL"]
    if backend == "io_uring":
        # The default Docker seccomp profile rejects io_uring_setup.
        command.extend(["--security-opt", "seccomp=unconfined"])
    command.extend([
        "--volume", f"{ROOT}:/src/faio:ro",
        "--mount", "type=volume,target=/src/faio/target",
        "--workdir", "/src/faio",
        "--env", "XMAKE_ROOT=y", "--entrypoint", "python3", args.docker_image,
        "scripts/verify_build_organization.py", "--backend", backend,
    ])
    for tool in tools:
        command.extend(["--tool", tool])
    for flag, value in [("--compiler", args.compiler), ("--bazel-cc", args.bazel_cc),
                        ("--bazel", args.bazel)]:
        if value:
            command.extend([flag, value])
    if args.xmake_offline:
        command.append("--xmake-offline")
    return command


def execute(label, command, env, dry_run):
    # The children stream their configure, build and test diagnostics directly to the terminal.
    display_env = [f"{key}={value}" for key, value in env.items() if key != "PATH"]
    print(f"[{label}] {shlex.join(display_env + command)}", flush=True)
    if dry_run:
        return 0
    try:
        return subprocess.run(command, env={**os.environ, **env}, cwd=ROOT).returncode
    except FileNotFoundError as error:
        print(error, file=sys.stderr, flush=True)
        return 127


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Each child runs in fresh temporary consumer projects. A failing cell makes the "
               "driver exit nonzero after running the other requested cells. Docker images must "
               "supply the selected tools, their dependencies and registry access or caches.\n\n"
               "  python3 scripts/verify_build_organization.py --backend kqueue\n"
               "  python3 scripts/verify_build_organization.py --docker-image faio:dev "
               "--tool cmake --backend all --compiler /usr/bin/clang++")
    parser.add_argument("--tool", action="append", choices=TOOLS,
                        help="Select a tool (repeatable, default all)")
    parser.add_argument("--backend", choices=("all", "epoll", "io_uring", "kqueue"), default="all",
                        help="Default all runs both Linux backends or macOS kqueue")
    parser.add_argument("--docker-image", metavar="IMAGE", help="Run inside a prepared Linux image")
    parser.add_argument("--compiler",
                        help="clang++ for CMake and xmake, inside the image when using Docker")
    parser.add_argument("--bazel-cc", help="clang for the Bazel local toolchain")
    parser.add_argument("--bazel", help="Bazel or Bazelisk executable")
    parser.add_argument("--xmake-offline", action="store_true", help="Use xmake's private network mode")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without launching builds")
    args = parser.parse_args()
    system = "Linux" if args.docker_image else platform.system()
    if system not in BACKENDS:
        parser.error("select --docker-image for a Linux run or run natively on macOS or Linux")
    available = BACKENDS[system]
    if args.backend != "all" and args.backend not in available:
        parser.error(f"{system} supports these stable verification backends: {', '.join(available)}")
    tools = tuple(dict.fromkeys(args.tool or TOOLS))
    backends = available if args.backend == "all" else (args.backend,)
    if args.docker_image and not args.dry_run:
        # Docker needs the mount point to exist in the read-only source bind.
        (ROOT / "target").mkdir(exist_ok=True)
    statuses = {}
    for backend in backends:
        if args.docker_image:
            label = f"docker/{backend}"
            statuses[label] = execute(label, container_command(backend, tools, args), {}, args.dry_run)
        else:
            for tool in tools:
                label = f"{tool}/{backend}"
                command, env = tool_command(tool, backend, args, system)
                statuses[label] = execute(label, command, env, args.dry_run)
    if not args.dry_run:
        for label, status in statuses.items():
            print(f"{label}: {'PASS' if status == 0 else f'FAIL ({status})'}")
    return int(any(statuses.values()))


if __name__ == "__main__":
    sys.exit(main())

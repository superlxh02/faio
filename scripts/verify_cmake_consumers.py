#!/usr/bin/env python3
"""Build, relocate and consume isolated faio CMake packages."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[1]


def run(*command):
    subprocess.run(command, check=True)


def configure(source, build, options):
    run("cmake", "-S", source, "-B", build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", *options)


def install_isolated(form, prefix, options):
    with tempfile.TemporaryDirectory(prefix=f"{form}-producer-", dir=prefix.parent) as scratch:
        root = Path(scratch)
        source = root / "source"
        source.mkdir()
        for filename in ("CMakeLists.txt", "LICENSE"):
            shutil.copy2(REPO / filename, source)
        shutil.copytree(REPO / "cmake", source / "cmake")
        shutil.copytree(REPO / "include", source / "include")
        if form == "cxx-module":
            shutil.copytree(REPO / form, source / form)

        build = source / "target" / "cmake-build-release"
        is_module = form == "cxx-module"
        configure(source, build, options + [
            f"-DFAIO_BUILD_MODULES={'ON' if is_module else 'OFF'}",
            f"-DFAIO_BUILD_HEADERS={'OFF' if is_module else 'ON'}",
            "-DFAIO_ENABLE_TLS=ON",
            "-DFAIO_ENABLE_PROCESS=ON",
            "-DFAIO_ENABLE_EXPERIMENTAL=OFF",
            "-DFAIO_BUILD_TESTS=OFF",
            "-DFAIO_BUILD_EXAMPLES=OFF",
            "-DFAIO_BUILD_BENCHMARKS=OFF",
        ])
        run("cmake", "--build", build, "--parallel", "4")
        run("cmake", "--install", build, "--prefix", root / "original")
        shutil.move(root / "original", prefix)


def consume(prefix, options, lookup, targets, sources, *args):
    with tempfile.TemporaryDirectory(prefix="downstream-", dir=prefix.parent) as scratch:
        root = Path(scratch)
        source = root / "source"
        source.mkdir()
        file_list = []
        for path in sources:
            shutil.copy2(path, source)
            file_list.append(path.name)
        use_modules = lookup.startswith("faioModules ")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.28)\n"
            "project(faioDownstream LANGUAGES CXX)\n"
            "set(CMAKE_CXX_EXTENSIONS OFF)\n"
            f"find_package({lookup})\n"
            f"add_executable(consumer {' '.join(file_list)})\n"
            f"target_link_libraries(consumer PRIVATE {' '.join(targets)})\n"
            f"target_compile_definitions(consumer PRIVATE FAIO_USE_HEADERS={int(not use_modules)})\n"
            "set_property(TARGET consumer PROPERTY CXX_SCAN_FOR_MODULES "
            f"{'ON' if use_modules else 'OFF'})\n",
            encoding="utf-8",
        )
        build = source / "target" / "cmake-build-release"
        configure(source, build, options + [f"-DCMAKE_PREFIX_PATH={prefix}"])
        run("cmake", "--build", build, "--parallel", "4")
        run(build / "consumer", *args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="C++ compiler executable")
    parser.add_argument("--io-uring", action="store_true", help="Use io_uring instead of epoll on Linux")
    args = parser.parse_args()
    flags = [f"-DFAIO_ENABLE_IO_URING={'ON' if args.io_uring else 'OFF'}"]
    if args.compiler:
        flags.append(f"-DCMAKE_CXX_COMPILER={args.compiler}")

    verification = REPO / "target" / "verification" / "cmake"
    verification.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="consumers-", dir=verification) as scratch:
        root = Path(scratch)
        header_prefix = root / "headers"
        module_prefix = root / "modules"
        install_isolated("include", header_prefix, flags)
        install_isolated("cxx-module", module_prefix, flags)
        if (header_prefix / "share/faio/cxx-module").exists():
            raise RuntimeError("The header-only package contains module interfaces")
        if not (module_prefix / "include/faio/faio.hpp").is_file():
            raise RuntimeError("The module package is missing its canonical headers")

        module_tests = REPO / "tests/modules"
        certificate_dir = REPO / "tests/fixtures"
        certificates = [certificate_dir / name for name in ("server.pem", "server.key", "ca.pem")]
        for prefix, package, core_target, component_prefix in (
            (header_prefix, "faio", "faio::faio", "faio::"),
            (module_prefix, "faioModules", "faio::module", "faio::module_"),
        ):
            lookup = f"{package} CONFIG REQUIRED"
            consume(prefix, flags, lookup, [core_target], [module_tests / "quickstart.cpp"])
            consume(prefix, flags, lookup, [core_target],
                    [module_tests / "foundation_main.cpp", module_tests / "foundation_peer.cpp"])
            consume(prefix, flags, lookup, [core_target], [module_tests / "core_contract.cpp"])
            consume(prefix, flags, lookup, [core_target],
                    [module_tests / "tls_contract.cpp"], *certificates)
            consume(prefix, flags, lookup, [core_target],
                    [module_tests / "process_contract.cpp"])
            consume(prefix, flags, lookup, [core_target],
                    [module_tests / "both_contract.cpp"])
            consume(prefix, flags, f"{lookup} COMPONENTS process tls",
                    [component_prefix + "process", component_prefix + "tls"],
                    [module_tests / "both_contract.cpp"])


if __name__ == "__main__":
    main()

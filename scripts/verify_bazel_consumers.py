#!/usr/bin/env python3
"""Build independent Bzlmod consumers against the shared headers and optional module interfaces."""

import argparse
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CONTRACTS = {
    "quickstart": ["quickstart.cpp"],
    "core": ["foundation_main.cpp", "foundation_peer.cpp"],
    "runtime": ["core_contract.cpp"],
    "tls": ["tls_contract.cpp"],
    "process": ["process_contract.cpp"],
    "both": ["both_contract.cpp"],
}


def prepare_provider(directory, form):
    directory.mkdir()
    for name in ("MODULE.bazel", "BUILD.bazel", "REPO.bazel"):
        shutil.copy2(ROOT / name, directory / name)
    shutil.copytree(ROOT / "include", directory / "include")
    (directory / "tools").mkdir()
    shutil.copy2(ROOT / "tools/experimental_compiler.cpp", directory / "tools")
    if form == "modules":
        shutil.copytree(ROOT / "cxx-module", directory / "cxx-module")


def configure_consumer(directory, form):
    modules = form == "modules"
    dependency = "@faio//:faio_module" if modules else "@faio//:faio"
    dependencies = {
        "quickstart": [dependency],
        "core": [dependency],
        "runtime": [dependency],
        "tls": [dependency + "_tls"],
        "process": [dependency + "_process"],
        "both": [dependency + "_tls", dependency + "_process"],
    }
    if modules:
        # Exercise the module package directly as well as the public root aliases.
        dependencies["quickstart"] = ["@faio//cxx-module:faio"]
        dependencies["tls"] = ["@faio//cxx-module:tls"]
        dependencies["process"] = ["@faio//cxx-module:process"]
    (directory / "MODULE.bazel").write_text(
        'module(name = "consumer")\n'
        'bazel_dep(name = "faio", version = "0.0.0")\n'
        f'local_path_override(module_name = "faio", path = "../{form}")\n'
        'bazel_dep(name = "rules_cc", version = "0.2.25")\n'
        'bazel_dep(name = "spdlog", version = "1.17.0")\n'
        'cc = use_extension("@rules_cc//cc:extensions.bzl", "cc_configure_extension")\n'
        'use_repo(cc, "local_config_cc_toolchains")\n'
    )
    rules = ['load("@rules_cc//cc:cc_test.bzl", "cc_test")\n']
    for name, sources in CONTRACTS.items():
        attrs = {
            "name": name,
            "size": "small",
            "srcs": sources,
            "deps": dependencies[name],
        }
        if not modules:
            attrs["cxxopts"] = ["-DFAIO_USE_HEADERS=1"]
        if name == "tls":
            fixtures = ["fixtures/server.pem", "fixtures/server.key", "fixtures/ca.pem"]
            attrs.update(data=fixtures, args=["$(rootpath " + path + ")" for path in fixtures])
        rules.append("cc_test(\n" + "".join(
            f"    {key} = {json.dumps(value)},\n" for key, value in attrs.items()
        ) + ")\n")
    (directory / "BUILD.bazel").write_text("\n".join(rules))


def verify_incremental(command, consumer, provider):
    """Check actions in an isolated module graph and execute both importing TUs."""
    with (consumer / "BUILD.bazel").open("a") as stream:
        stream.write('cc_test(name="incremental", srcs=["incremental_main.cpp", "incremental_peer.cpp"], '
                     'deps=["@faio//:faio_module"])\n')
    main = consumer / "incremental_main.cpp"
    peer = consumer / "incremental_peer.cpp"
    main.write_text(
        "#include <cstdlib>\nimport faio;\nint peer_value();\n"
        "int main(int argc, char **argv) {\n"
        "  return argc == 2 && faio::bazel_probe_value == std::atoi(argv[1])\n"
        "      && peer_value() == faio::bazel_probe_value ? 0 : 1;\n}\n"
    )
    peer.write_text("import faio;\nint peer_value() { return faio::bazel_probe_value; }\n")
    interface = provider / "cxx-module/faio.cppm"
    original = interface.read_text()
    marker = ("\nexport namespace faio { inline constexpr int bazel_probe_value = "
              "static_cast<int>(error_domain::process) + %d; }\n")
    interface.write_text(original + marker % 1)

    def check_actions(label, expected_value, required, *, exact=False):
        result = subprocess.run(
            [*command, "--subcommands", "--cache_test_results=no",
             f"--test_arg={expected_value}", "//:incremental"],
            cwd=consumer, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        sources = set(re.findall(r"\[action 'Compiling ([^']+)'", result.stdout))
        files = {Path(source).name for source in sources}
        if result.returncode or not required <= files or (exact and files != required):
            print(result.stdout, file=sys.stderr)
            raise RuntimeError(f"{label}: unexpected compiled files {sorted(files)}")
        print(f"{label}: {', '.join(sorted(files)) or 'no C++ compilation'}", flush=True)

    consumers = {main.name, peer.name}
    check_actions("cold consumer", 8, consumers | {interface.name})
    check_actions("unchanged consumer", 8, set(), exact=True)
    implementation = provider / "cxx-module/duplex.cpp"
    with implementation.open("a") as stream:
        stream.write("\nint faio_bazel_incremental_probe() { return 1; }\n")
    check_actions("changed duplex implementation TU", 8, {implementation.name}, exact=True)
    # Only the staged provider is mutated. Both importing TUs must observe the
    # new value from the canonical header without touching the façade itself.
    header = provider / "include/faio/detail/common/error.hpp"
    before = header.read_text()
    after, changed = re.subn(r"(\bprocess\s*=\s*)7\b", r"\g<1>17", before)
    if changed != 1:
        raise RuntimeError("Cannot identify the public process error-domain value")
    header.write_text(after)
    check_actions("changed shared error domain", 18, consumers | {interface.name})
    interface.write_text(original + marker % 2)
    check_actions("changed exported value", 19, consumers | {interface.name})
    with peer.open("a") as stream:
        stream.write("int faio_bazel_probe_peer() { return 2; }\n")
    check_actions("changed consumer TU", 19, {peer.name}, exact=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bazel", default="bazel")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--form", choices=["headers", "modules", "both"], default="both")
    parser.add_argument("--io-uring", choices=["auto", "on", "off"],
                        default="off" if sys.platform.startswith("linux") else "auto")
    parser.add_argument("--suite", choices=["all", "components", "incremental"], default="all")
    args = parser.parse_args()
    if args.form == "headers" and args.suite == "incremental":
        parser.error("the incremental suite requires modules")
    bazel = shutil.which(args.bazel)
    if not bazel:
        parser.error("Bazel or Bazelisk executable could not be found")
    if args.jobs <= 0:
        parser.error("--jobs must be positive")

    verification = ROOT / "target" / "verification" / "bazel"
    verification.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="consumers-", dir=verification) as temporary:
        work = Path(temporary)
        consumer = work / "consumer"
        consumer.mkdir()
        for name in {name for group in CONTRACTS.values() for name in group}:
            shutil.copy2(ROOT / "tests/modules" / name, consumer / name)
        shutil.copytree(ROOT / "tests/fixtures", consumer / "fixtures")
        shutil.copy2(ROOT / ".bazelversion", consumer)
        # A dependency's .bazelrc is never loaded. The downstream root explicitly
        # adopts the toolchain profile and qualifies faio's configuration flags.
        (consumer / ".bazelrc").write_text(
            (ROOT / ".bazelrc").read_text().replace("--//:", "--@faio//:")
        )
        forms = ("headers", "modules") if args.form == "both" else (args.form,)
        for form in forms:
            prepare_provider(work / form, form)
            configure_consumer(consumer, form)
            command = [bazel, "--batch", "--output_user_root=" + str(ROOT / "target/bazel/state"),
                       "--output_base=" + str(work / "output"), "test",
                       "--config=faio_" + form, "--test_output=errors", f"--jobs={args.jobs}",
                       "--@faio//:io_uring=" + args.io_uring]
            if args.suite == "incremental":
                if form == "modules":
                    verify_incremental(command, consumer, work / form)
                continue
            for names, switches in [
                (["quickstart", "core", "runtime"], []),
                (["tls"], ["tls"]),
                (["process"], ["process"]),
                (["both"], ["tls", "process"]),
            ]:
                subprocess.run(
                    [*command, *["--@faio//:" + switch + "=true" for switch in switches],
                     *["//:" + name for name in names]],
                    cwd=consumer, check=True,
                )
            if form == "modules" and args.suite == "all":
                verify_incremental(command, consumer, work / form)


if __name__ == "__main__":
    main()

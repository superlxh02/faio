#!/usr/bin/env python3
"""Exercise isolated xmake source consumers and a relocated module installation."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid


REPO = Path(__file__).resolve().parents[1]
MODULE_TESTS = REPO / "tests/modules"
CERTIFICATES = tuple(REPO / "tests/fixtures" / name for name in ("server.pem", "server.key", "ca.pem"))
MODULE_TARGETS = ("faio_module",)


def run_xmake(directory, action, *arguments, env=None):
    # These standalone projects are nested below faio/target. Prevent xmake
    # from walking up to the enclosing checkout and selecting its xmake.lua.
    environment = {**os.environ, "XMAKE_CONFIGDIR": str(directory / "target" / "xmake"),
                   **(env or {})}
    subprocess.run(["xmake", action, "-P", str(directory), *arguments],
                   cwd=directory, check=True, env=environment)


def configure(directory, flags, *options, env=None, builddir="target/xmake"):
    output = ["-o", builddir] if builddir else []
    run_xmake(directory, "f", "-c", "-y", "-m", "release", *output,
        *flags, *options, env=env)


def copy_shared_rules(destination):
    rules = Path("tools/xmake/common.lua")
    (destination / rules.parent).mkdir(parents=True)
    shutil.copy2(REPO / rules, destination / rules)


def install_module(working, prefix, flags):
    with tempfile.TemporaryDirectory(prefix="provider-", dir=working) as temporary:
        source = Path(temporary)
        copy_shared_rules(source)
        for name in ("cxx-module", "include"):
            shutil.copytree(REPO / name, source / name)
        project = source / "cxx-module"
        env = {"XMAKE_CONFIGDIR": str(source / "target/xmake/config-module")}
        configure(project, flags, "--faio_tls=y", "--faio_process=y", builddir=None, env=env)
        run_xmake(project, "build", "-j", "4", env=env)
        if not any((source / "target/xmake/cxx-module").rglob("libfaio_module.a")):
            raise RuntimeError("The standalone module build did not use target/xmake/cxx-module")
        installed = working / "original"
        for component in MODULE_TARGETS:
            run_xmake(project, "install", "-o", installed, component, env=env)
        for header in (REPO / "include" / "faio").rglob("*"):
            relative = header.relative_to(REPO)
            if header.is_file() and not (installed / relative).is_file():
                raise RuntimeError(f"The module package is missing {relative}")
        shutil.move(installed, prefix)


def build_source_consumer(directory, flags, form, components, cases):
    directory.mkdir()
    vendor = directory / "third_party/faio"
    copy_shared_rules(vendor)
    shutil.copytree(REPO / "include", vendor / "include")
    module = form == "cxx-module"
    if module:
        shutil.copytree(REPO / form, vendor / form)
    else:
        shutil.copy2(REPO / "xmake.lua", vendor)
    form_settings = ('set_policy("build.c++.modules", true)\n    set_policy("build.c++.modules.std", false)'
                     if module else 'add_defines("FAIO_USE_HEADERS=1")')
    target = "faio_module" if module else "faio"
    dependencies = f'"{target}"'
    provider = "third_party/faio/cxx-module" if module else "third_party/faio"
    project = f'''set_project("faio-source-consumer")
set_languages("c++23")
includes("{provider}")
'''
    for name, sources, _ in cases:
        for source in sources:
            shutil.copy2(MODULE_TESTS / source, directory)
        source_list = ", ".join(f'"{source}"' for source in sources)
        project += f'''target("{name}")
    set_kind("binary")
    add_deps({dependencies})
    add_files({source_list})
    {form_settings}
target_end()
'''
    (directory / "xmake.lua").write_text(project, encoding="utf-8")
    options = [] if module else ["--faio_tests=n", "--faio_examples=n"]
    configure(directory, flags, *options,
              f"--faio_tls={'y' if 'tls' in components else 'n'}",
              f"--faio_process={'y' if 'process' in components else 'n'}")
    run_xmake(directory, "build", "-j", "4")
    for name, _, arguments in cases:
        run_xmake(directory, "run", name, *arguments)


def build_package_consumer(directory, repository, flags, use_uring):
    directory.mkdir()
    component = "faio_installed_" + uuid.uuid4().hex[:12]
    recipe = repository / "packages" / "f" / component
    recipe.mkdir(parents=True)
    (recipe / "xmake.lua").write_text(f'''package("{component}")
    set_kind("library")
    set_description("Locally installed faio modules")
    add_linkdirs("lib")
    add_links("faio_module")
    add_defines("FAIO_HAS_IO_URING={int(use_uring)}", "FAIO_ENABLE_TLS=1", "FAIO_ENABLE_PROCESS=1")
    if is_plat("windows", "mingw") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_WIN32_WINNT=0x0A00")
        add_syslinks("ws2_32", "mswsock", "iphlpapi", "bcrypt", "advapi32")
    elseif is_plat("linux") then
        add_defines("_GNU_SOURCE")
        add_cxxflags("-pthread")
        add_ldflags("-pthread")
    end
    on_install(function (package)
        os.cp(path.join(os.scriptdir(), "../../../prefix/*"), package:installdir())
    end)
''', encoding="utf-8")

    uring_require = 'add_requires("pkgconfig::liburing", {alias = "uring", system = true})' if use_uring else ""
    uring_package = 'add_packages("uring")' if use_uring else ""
    (directory / "xmake.lua").write_text(f'''set_project("faio-installed-consumer")
set_languages("c++23")
add_repositories("faio-verification {repository.as_posix()}")
add_requires("{component}", {{system = false}})
add_requires("pkgconfig::spdlog", {{alias = "spdlog", system = true}})
add_requires("pkgconfig::openssl >=3.5", {{alias = "openssl", system = true}})
{uring_require}
target("consumer")
    set_kind("binary")
    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.std", false)
    add_packages("{component}", "spdlog", "openssl")
    {uring_package}
    add_files("both_contract.cpp")
target_end()
target("import_only")
    set_kind("binary")
    set_policy("build.c++.modules", true)
    set_policy("build.c++.modules.std", false)
    add_packages("{component}", "spdlog", "openssl")
    {uring_package}
    add_defines("FAIO_IMPORT_HAS_TASK=1", "FAIO_IMPORT_HAS_RUNTIME=1")
    add_files("import_only.cpp")
''', encoding="utf-8")
    shutil.copy2(MODULE_TESTS / "both_contract.cpp", directory)
    shutil.copy2(MODULE_TESTS / "import_only.cpp", directory)
    package_cache = directory / "target" / "xmake" / "package-cache"
    package_install = directory / "target" / "xmake" / "package-install"
    package_cache.mkdir(parents=True)
    package_install.mkdir(parents=True)
    environment = {"XMAKE_PKG_CACHEDIR": str(package_cache),
                   "XMAKE_PKG_INSTALLDIR": str(package_install)}
    configure(directory, flags, env=environment)
    run_xmake(directory, "build", "-j", "4", env=environment)
    run_xmake(directory, "run", "consumer", env=environment)
    run_xmake(directory, "run", "import_only", env=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, help="Path to clang++")
    parser.add_argument("--io-uring", action="store_true")
    parser.add_argument("--offline", action="store_true", help="Use xmake private network mode")
    args = parser.parse_args()
    flags = ["--toolchain=clang", f"--cxx={args.compiler}", f"--ld={args.compiler}"]
    if args.offline:
        flags.append("--policies=network.mode:private")

    verification = REPO / "target" / "verification" / "xmake"
    verification.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="consumers-", dir=verification) as temporary:
        root = Path(temporary)
        repository = root / "repository"
        repository.mkdir()
        provider_flags = [
            *flags, f"--faio_io_uring={'y' if args.io_uring else 'n'}", "--faio_allocator=system",
        ]
        groups = (
            ("core", (), (("quickstart", ("quickstart.cpp",), ()),
                          ("foundation", ("foundation_main.cpp", "foundation_peer.cpp"), ()),
                          ("core", ("core_contract.cpp",), ()))),
            ("tls", ("tls",), (("tls", ("tls_contract.cpp",), CERTIFICATES),)),
            ("process", ("process",), (("process", ("process_contract.cpp",), ()),)),
            ("both", ("tls", "process"), (("both", ("both_contract.cpp",), ()),)),
        )
        for form in ("include", "cxx-module"):
            for group, components, cases in groups:
                build_source_consumer(root / f"{form}-{group}", provider_flags, form, components, cases)
        install_module(root, repository / "prefix", provider_flags)
        build_package_consumer(root / "installed", repository, flags, args.io_uring)


if __name__ == "__main__":
    main()

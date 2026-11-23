"""Expose the C++23 standard library module that ships with GCC to Bazel.

Bazel has no built-in `std` module. GCC 16.1 and newer install
`libstdc++.modules.json`, which names the interface unit (`bits/std.cc`) used for
`import std;`. The repository rule below turns that interface into a regular
`cc_library`, so a Bazel target can depend on `@gcc_std_module//:std` to make
the standard library module available to the rules_cc module map.

The repository always materializes and always exports `supported.bzl`, whose
`SUPPORTED` flag says whether the selected compiler ships a usable manifest.
`examples/BUILD.bazel` reads that flag, so the standard-library example is built
on GCC 16.1+ and is marked incompatible everywhere else instead of failing the
whole package.
"""

visibility("public")

_STD_BUILD = """load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "std",
    module_interfaces = ["std.cc"],
)
"""

_UNSUPPORTED_BUILD = """# The selected compiler has no C++23 std module manifest.
# Read `supported.bzl` in this repository to test for support without a load error.
package(default_visibility = ["//visibility:public"])
"""

_SUPPORTED_BZL = "SUPPORTED = {}\n"


def _find_std_source(rctx, manifest):
    """Return the interface file for the `std` module, or None."""
    content = rctx.read(manifest)
    modules = json.decode(content).get("modules", [])
    for entry in modules:
        if entry.get("logical-name") != "std":
            continue
        source = entry.get("source-path")
        if not source:
            continue
        directory = manifest[:manifest.rfind("/") + 1]
        interface = rctx.path(directory + source)
        if interface.exists:
            return interface
    return None


def _gcc_std_module_impl(rctx):
    cc = rctx.os.environ.get("CC", "cc")
    result = rctx.execute([cc, "-print-file-name=libstdc++.modules.json"], timeout = 60)
    manifest = result.stdout.strip() if result.return_code == 0 else ""
    # Clang answers with the literal file name instead of an installed path.
    if not manifest or "/" not in manifest or not rctx.path(manifest).exists:
        rctx.file("supported.bzl", _SUPPORTED_BZL.format("False"))
        rctx.file("BUILD.bazel", _UNSUPPORTED_BUILD)
        return
    interface = _find_std_source(rctx, manifest)
    if not interface:
        rctx.file("supported.bzl", _SUPPORTED_BZL.format("False"))
        rctx.file("BUILD.bazel", _UNSUPPORTED_BUILD)
        return
    rctx.file("std.cc", rctx.read(interface))
    rctx.file("supported.bzl", _SUPPORTED_BZL.format("True"))
    rctx.file("BUILD.bazel", _STD_BUILD)


_gcc_std_module = repository_rule(
    implementation = _gcc_std_module_impl,
    environ = ["CC"],
)


def _gcc_std_module_extension(module_ctx):
    _gcc_std_module(name = "gcc_std_module")


gcc_std_modules = module_extension(
    implementation = _gcc_std_module_extension,
)

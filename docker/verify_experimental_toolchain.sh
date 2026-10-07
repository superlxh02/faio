#!/usr/bin/env bash
# A real compile/run check of the isolated Fedora GCC and libstdc++ pair.
set -euo pipefail

readonly faio_toolchain_cxx="${CXX:-/opt/gcc-16.1/usr/bin/g++}"
readonly faio_toolchain_version="$("${faio_toolchain_cxx}" -dumpfullversion -dumpversion)"
case "${faio_toolchain_version}" in
    16.1.*) ;;
    *) printf 'faio experimental requires GCC 16.1.x; found %s\n' "${faio_toolchain_version}" >&2; exit 1 ;;
esac

readonly faio_toolchain_library="$(readlink -f "$("${faio_toolchain_cxx}" -print-file-name=libstdc++.so)")"
case "${faio_toolchain_library}" in
    /opt/gcc-16.1/usr/lib64/*) ;;
    *) printf 'faio experimental: incorrect libstdc++: %s\n' "${faio_toolchain_library}" >&2; exit 1 ;;
esac

python3 - <<'PY'
import re
import subprocess
version = subprocess.check_output(['cmake', '--version'], text=True).splitlines()[0]
numbers = tuple(map(int, re.search(r'(\d+)\.(\d+)\.(\d+)', version).groups()))
if numbers < (4, 1, 0):
    raise SystemExit(f'faio experimental requires CMake >= 4.1; found {version}')
print(version)
PY
ninja --version

faio_toolchain_tmp="$(mktemp -d)"
trap 'rm -rf "${faio_toolchain_tmp}"' EXIT
cat > "${faio_toolchain_tmp}/reflection.cpp" <<'CPP'
#include <meta>
#include <cstdio>
#include <string>

struct options { int value; };
[[=options{42}]] int target() { return 42; }
constexpr auto annotation = std::meta::extract<options>(
    std::meta::annotations_of_with_type(^^target, ^^options)[0]);
static_assert(annotation.value == 42);
static_assert(__linux__ && __GNUC__ == 16 && __GNUC_MINOR__ == 1);
static_assert(_GLIBCXX_RELEASE == 16);
int main() {
    const std::string value = std::to_string([: ^^target :]());
    std::printf("GCC %s; libstdc++ release %d date %d; reflection result %s\n",
                __VERSION__, _GLIBCXX_RELEASE, __GLIBCXX__, value.c_str());
    return value == "42" ? 0 : 1;
}
CPP
"${faio_toolchain_cxx}" -std=c++26 -freflection -Wall -Wextra -Wpedantic \
    "${faio_toolchain_tmp}/reflection.cpp" -o "${faio_toolchain_tmp}/reflection"
"${faio_toolchain_tmp}/reflection"
faio_toolchain_loaded="$(ldd "${faio_toolchain_tmp}/reflection")"
printf '%s\n' "${faio_toolchain_loaded}"
case "${faio_toolchain_loaded}" in
    *'libstdc++.so.6 => /opt/gcc-16.1/usr/lib64/libstdc++.so.6'*) ;;
    *) printf 'faio experimental: the probe loaded an unpaired libstdc++\n' >&2; exit 1 ;;
esac
case "${faio_toolchain_loaded}" in
    *'libgcc_s.so.1 => /opt/gcc-16.1/lib64/libgcc_s.so.1'*) ;;
    *) printf 'faio experimental: the probe loaded an unpaired libgcc\n' >&2; exit 1 ;;
esac
# Linking catches missing/mismatched sanitizer development libraries. Runtime
# sanitizer tests belong to separate faio ASan+UBSan and TSan build directories.
"${faio_toolchain_cxx}" -std=c++26 -freflection -fsanitize=address,undefined \
    "${faio_toolchain_tmp}/reflection.cpp" -o "${faio_toolchain_tmp}/asan-ubsan"
"${faio_toolchain_cxx}" -std=c++26 -freflection -fsanitize=thread \
    "${faio_toolchain_tmp}/reflection.cpp" -o "${faio_toolchain_tmp}/tsan"
printf 'paired libstdc++: %s\n' "${faio_toolchain_library}"

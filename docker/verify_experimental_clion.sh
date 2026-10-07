#!/usr/bin/env bash
# Verify the exact default paths discovered by CLion, without an entrypoint.
set -euo pipefail

for faio_clion_driver in /usr/bin/gcc /usr/bin/g++; do
    case "$("${faio_clion_driver}" -dumpfullversion -dumpversion)" in
        16.1.*) ;;
        *) printf 'CLion default compiler is not GCC 16.1: %s\n' "${faio_clion_driver}" >&2; exit 1 ;;
    esac
    test "$(readlink -f "${faio_clion_driver}")" = "/opt/gcc-16.1/usr/bin/$(basename "${faio_clion_driver}")"
done
case "$(readlink -f "$(/usr/bin/g++ -print-file-name=libstdc++.so)")" in
    /opt/gcc-16.1/usr/lib64/*) ;;
    *) printf 'CLion default compiler uses an unpaired libstdc++\n' >&2; exit 1 ;;
esac
test "${FAIO_ENABLE_EXPERIMENTAL}" = ON
test "${FAIO_EXPERIMENTAL_REFLECTION}" = ON
test "${FAIO_EXPERIMENTAL_EXECUTION}" = ON
test "${FAIO_EXPERIMENTAL_ENTRY_MODE}" = NORMAL
test "${FAIO_STDEXEC_SOURCE_DIR}" = /opt/faio-deps/src/stdexec
python3 /opt/faio-build/scripts/bootstrap_experimental_deps.py \
    --workspace-root /opt/faio-deps --verify-only
/usr/bin/gdb --version

faio_clion_tmp="$(mktemp -d)"
trap 'rm -rf "${faio_clion_tmp}"' EXIT
cat > "${faio_clion_tmp}/backend.cpp" <<'CPP'
#include <exec/task.hpp>
#include <stdexec/execution.hpp>
#include <tuple>
#include <version>
static_assert(__GNUC__ == 16 && __GNUC_MINOR__ == 1 && _GLIBCXX_RELEASE == 16);
exec::task<int> backend_task() {
    co_return co_await stdexec::just(42);
}
int main() {
    const auto value = stdexec::sync_wait(backend_task());
    return value && std::get<0>(*value) == 42 ? 0 : 1;
}
CPP
# No -I, reflection flag, CPATH, or global CMake compiler flags are needed.
/usr/bin/g++ -std=c++26 -g "${faio_clion_tmp}/backend.cpp" -o "${faio_clion_tmp}/backend"
"${faio_clion_tmp}/backend"
printf 'CLion GCC 16.1 default driver, bare exec/task.hpp, and fixed backend: passed\n'

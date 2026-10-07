#!/usr/bin/env bash
# Install the pinned Fedora compiler beside Rawhide's system compiler.
# All RPM payloads and runtime libraries come from the same Fedora build.
set -euo pipefail

readonly faio_gcc_nvr='16.1.1-2.fc44'
readonly faio_gcc_prefix='/opt/gcc-16.1'
readonly faio_gcc_arch="$(uname -m)"
readonly faio_gcc_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly faio_gcc_cache="${FAIO_GCC_RPM_CACHE:-/tmp/faio-gcc16.1-rpms}"

case "${faio_gcc_arch}" in
    aarch64|x86_64) ;;
    *) printf 'faio experimental: no pinned GCC RPMs for %s\n' "${faio_gcc_arch}" >&2; exit 1 ;;
esac

faio_gcc_curl_args=(--fail --silent --show-error --location --retry 3
                    --retry-all-errors --connect-timeout 20 --max-time 240)
# An optional DNS override keeps HTTPS hostname/certificate verification intact.
# This is useful behind a local DNS proxy; normal builds use public DNS.
if [[ -n "${KOJI_ADDRESS:-}" ]]; then
    faio_gcc_curl_args+=(--resolve "kojipkgs.fedoraproject.org:443:${KOJI_ADDRESS}")
fi

mkdir -p "${faio_gcc_cache}/${faio_gcc_arch}" "${faio_gcc_prefix}"
faio_gcc_selected_manifest="${faio_gcc_cache}/gcc16.1-${faio_gcc_arch}.sha256"
awk -v arch="${faio_gcc_arch}/" '$2 ~ "^" arch {print}' \
    "${faio_gcc_script_dir}/gcc16.1.sha256" > "${faio_gcc_selected_manifest}"
test "$(wc -l < "${faio_gcc_selected_manifest}")" -eq 12

while read -r faio_gcc_sha faio_gcc_relative; do
    faio_gcc_file="${faio_gcc_cache}/${faio_gcc_relative}"
    if [[ ! -f "${faio_gcc_file}" ]]; then
        if [[ -n "${FAIO_GCC_RPM_IMPORT:-}" \
              && -f "${FAIO_GCC_RPM_IMPORT}/${faio_gcc_relative}" ]]; then
            cp "${FAIO_GCC_RPM_IMPORT}/${faio_gcc_relative}" "${faio_gcc_file}"
        else
            curl "${faio_gcc_curl_args[@]}" \
                "https://kojipkgs.fedoraproject.org/packages/gcc/16.1.1/2.fc44/${faio_gcc_relative}" \
                --output "${faio_gcc_file}.partial"
            mv "${faio_gcc_file}.partial" "${faio_gcc_file}"
        fi
    fi
done < "${faio_gcc_selected_manifest}"

(cd "${faio_gcc_cache}" && sha256sum --check "${faio_gcc_selected_manifest}")
while read -r faio_gcc_sha faio_gcc_relative; do
    faio_gcc_file="${faio_gcc_cache}/${faio_gcc_relative}"
    test "$(rpm -qp --queryformat '%{VERSION}-%{RELEASE}' "${faio_gcc_file}")" = "${faio_gcc_nvr}"
    test "$(rpm -qp --queryformat '%{ARCH}' "${faio_gcc_file}")" = "${faio_gcc_arch}"
    # Avoid an rpm2cpio SIGPIPE when cpio stops at the archive trailer before
    # reading all padded bytes; verify both commands through a temporary file.
    rpm2cpio "${faio_gcc_file}" > "${faio_gcc_cache}/payload.cpio"
    (cd "${faio_gcc_prefix}" && cpio --extract --make-directories --unconditional --quiet \
        < "${faio_gcc_cache}/payload.cpio")
    rm "${faio_gcc_cache}/payload.cpio"
done < "${faio_gcc_selected_manifest}"

# Fedora's libgcc/TSan linker scripts embed absolute distro paths. Relocate
# those local packaging paths so the isolated compiler never links Rawhide's
# libraries. Relative RPM symlinks already resolve within the isolated tree.
python3 - <<'PY'
import pathlib
import re
prefix = pathlib.Path('/opt/gcc-16.1')
for path in (prefix / 'usr/lib/gcc').glob('*/16/*.so'):
    if path.is_symlink():
        continue
    payload = path.read_bytes()
    if payload.startswith(b'\x7fELF'):
        continue
    text = payload.decode('utf-8')
    text = re.sub(r'(?<![\w/])(/(?:usr/)?lib64/)', str(prefix) + r'\1', text)
    path.write_text(text)
PY

mkdir -p /usr/local/share/faio-experimental
cp "${faio_gcc_selected_manifest}" /usr/local/share/faio-experimental/gcc16.1.sha256
printf 'Fedora GCC build: %s\nArchitecture: %s\nSource: %s\n' \
    "${faio_gcc_nvr}" "${faio_gcc_arch}" \
    'https://kojipkgs.fedoraproject.org/packages/gcc/16.1.1/2.fc44/' \
    > /usr/local/share/faio-experimental/toolchain.txt

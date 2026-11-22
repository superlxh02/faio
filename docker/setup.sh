#!/usr/bin/env bash
## @file docker/setup.sh
## @brief 安装 Docker 工具链、系统依赖和备用包管理器。
set -euo pipefail

faio_features="${1:-all}"
case "$faio_features" in base|vcpkg|all|main) ;; *) echo 'Usage: setup.sh base|vcpkg|all|main' >&2; exit 2 ;; esac

if [[ "$faio_features" == vcpkg ]]; then
    git clone --depth 1 --branch 2026.10.16 https://github.com/microsoft/vcpkg /opt/vcpkg
    test "$(git -C /opt/vcpkg rev-parse HEAD)" = 9eec002df1cd109f8f104cd966a763daf9751489
    CC=/usr/bin/gcc CXX=/usr/bin/g++ LD_LIBRARY_PATH=/opt/gcc-16.1/usr/lib64:/opt/gcc-16.1/lib64 VCPKG_DISABLE_METRICS=1 /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
    /opt/vcpkg/vcpkg version
    exit 0
fi

if [[ "$faio_features" != base ]]; then
    if [[ "$faio_features" == main ]]; then
        dnf install -y --setopt=install_weak_deps=False --setopt=tsflags=nodocs --setopt=keepcache=True --setopt=max_parallel_downloads=10 clang
        test "$(rpm -q --qf '%{VERSION}' clang | cut -d. -f1)" = 23
        for faio_driver in clang clang++; do
            printf '#!/bin/sh\nexec /usr/bin/%s-23 --gcc-toolchain=/opt/gcc-16.1/usr "$@"\n' "$faio_driver" > "/usr/local/bin/$faio_driver-23"
            chmod 0755 "/usr/local/bin/$faio_driver-23"
        done
        ln -s /usr/local/bin/clang-23 /usr/local/bin/faio-cc
        ln -s /usr/local/bin/clang++-23 /usr/local/bin/faio-cxx
        clang++-23 --version
    else
        python3 /opt/faio-build/scripts/bootstrap_experimental_deps.py --workspace-root /opt/faio-deps
        git config --system --add safe.directory /opt/faio-deps/src/stdexec
        ln -s /opt/faio-deps/src/stdexec/include/stdexec /usr/local/include/stdexec
        ln -s /opt/faio-deps/src/stdexec/include/exec /usr/local/include/exec
        ln -s /usr/bin/gcc /usr/local/bin/faio-cc
        ln -s /usr/bin/g++ /usr/local/bin/faio-cxx
        for faio_tool in clang clang++ clang-format clang-tidy llvm-config lld lldb; do
            if command -v "$faio_tool" >/dev/null; then
                echo "Unexpected LLVM tool in experimental image: $faio_tool" >&2
                exit 1
            fi
        done
    fi

    exit 0
fi

dnf install -y --setopt=install_weak_deps=False --setopt=tsflags=nodocs --setopt=keepcache=True --setopt=max_parallel_downloads=10 \
    asio-devel binutils ca-certificates cargo ccache cmake cpio curl \
    doxygen gcc gcc-c++ gdb gdb-gdbserver git gperftools-devel graphviz gtest-devel liburing-devel make \
    ninja-build openssl-devel openssl3-libs pkgconf-pkg-config python3 rust \
    spdlog-devel tar unzip zip clang-libs llvm-libs
# openssl3-libs：Fedora 45 只带 OpenSSL 4，而 CLion 的 .NET (Rider/radler) 后端
# 仍链接 libssl.so.3，缺了它 CLion devcontainer 的 C++ 索引后端会直接崩溃。

faio_clangd_tmp="$(mktemp -d)"
trap 'rm -rf "$faio_clangd_tmp"' EXIT
dnf download --destdir "$faio_clangd_tmp" clang-tools-extra
for faio_rpm in "$faio_clangd_tmp"/*.rpm; do
    rpm --checksig "$faio_rpm"
    rpm2cpio "$faio_rpm" > "$faio_clangd_tmp/payload.cpio"
    mkdir -p "$faio_clangd_tmp/extracted"
    (cd "$faio_clangd_tmp/extracted" && cpio --extract --make-directories --quiet < ../payload.cpio)
done
faio_clangd="$(find "$faio_clangd_tmp/extracted/usr" -type f -name clangd -print -quit)"
test -n "$faio_clangd"
install -m 0755 "$faio_clangd" /usr/local/bin/clangd
mkdir -p /usr/local/share/licenses/faio-clangd
if [[ -d "$faio_clangd_tmp/extracted/usr/share/licenses" ]]; then
    find "$faio_clangd_tmp/extracted/usr/share/licenses" -type f -exec cp -t /usr/local/share/licenses/faio-clangd {} +
fi
clangd --version

bash /opt/faio-toolchain/bootstrap_gcc16.1.sh
for faio_driver in gcc g++; do
    mv "/usr/bin/$faio_driver" "/usr/bin/$faio_driver.system"
    ln -s "/opt/gcc-16.1/usr/bin/$faio_driver" "/usr/bin/$faio_driver"
done
ln -s /opt/gcc-16.1/usr/bin/gcc /usr/local/bin/gcc-16
ln -s /opt/gcc-16.1/usr/bin/g++ /usr/local/bin/g++-16
export LD_LIBRARY_PATH=/opt/gcc-16.1/usr/lib64:/opt/gcc-16.1/lib64
export CC=/usr/bin/gcc CXX=/usr/bin/g++
bash /opt/faio-toolchain/verify_experimental_toolchain.sh

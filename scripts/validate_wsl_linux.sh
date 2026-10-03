#!/usr/bin/env bash
# WSL Linux 双后端的可复现构建/运行入口；不改动系统选择的 faio 后端。
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dependency_root="${FAIO_DEPENDENCY_ROOT:-$(dirname "$repo")/faio-deps}"
build="${FAIO_WSL_BUILD:-$repo/build/wsl-linux-clang22-dual}"
jobs="${FAIO_WSL_JOBS:-4}"
phase="${1:-verify}"
logs="$build/logs"
apt_root="$dependency_root/wsl/apt"
mkdir -p "$logs"

# 所有归档与 apt 索引保留在 Windows 的 E 盘共享 workspace，便于复用和审计。
apt_configuration() {
    if [[ $(id -u) != 0 ]]; then
        echo "apt 阶段请用 wsl -d Ubuntu -u root --exec bash $0 $phase" >&2
        return 1
    fi
    mkdir -p "$apt_root/archives/partial" "$apt_root/lists/partial"
    cat > "$apt_root/apt.conf" <<EOF
Dir::Cache::archives "$apt_root/archives";
Dir::State::lists "$apt_root/lists";
APT::Keep-Downloaded-Packages "true";
Binary::apt-get::APT::Keep-Downloaded-Packages "true";
# DrvFS 未启用 metadata 时新文件呈现 devloper 所有者；_apt 无权 fchmod 临时索引。
# 只在此工作区缓存配置中由 root 下载，仍保留 apt 的签名/哈希验证，不更改系统挂载。
APT::Sandbox::User "root";
EOF
}

# 每个阶段有独立完整日志；失败同时打印末尾诊断，不用额外终端才能定位错误。
run_logged() {
    local name="$1"
    shift
    printf '%s\n' "Running $name; log=$logs/$name.log"
    if "$@" > "$logs/$name.log" 2>&1; then
        tail -n 8 "$logs/$name.log"
    else
        local result=$?
        tail -n 100 "$logs/$name.log" >&2
        return "$result"
    fi
}

packages=(clang-22 g++-15 cmake ninja-build pkg-config liburing-dev)
download() {
    apt_configuration
    run_logged apt-update apt-get -c "$apt_root/apt.conf" update
    run_logged apt-download apt-get -c "$apt_root/apt.conf" install --download-only --yes "${packages[@]}"
}
install() {
    apt_configuration
    # 索引和归档可由 download 阶段事先准备；这里不重复更新/拉取已缓存文件。
    run_logged apt-install env DEBIAN_FRONTEND=noninteractive apt-get -c "$apt_root/apt.conf" install --yes "${packages[@]}"
}
environment() {
    {
        date --iso-8601=seconds
        cat /etc/os-release
        uname -a
        id
        printf 'io_uring_disabled='; cat /proc/sys/kernel/io_uring_disabled
        clang++-22 --version
        clang++-22 -v
        cmake --version
        ninja --version
        pkg-config --modversion liburing
        dpkg-query -W clang-22 g++-15 libstdc++-15-dev libstdc++6 cmake ninja-build liburing-dev
        printf 'dependency_root=%s\nbuild=%s\njobs=%s\n' "$dependency_root" "$build" "$jobs"
    } > "$logs/environment.log" 2>&1
    # 保留实际消费的头文件指纹，配合新构建目录证明验证来自本次 Windows 改动后的源码。
    find "$repo/include/faio" -type f \( -name '*.hpp' -o -name '*.h' \) -print0 |
        sort -z | xargs -0 sha256sum > "$logs/source-headers.sha256"
    dpkg-query -W -f='${Package}\t${Version}\n' > "$logs/installed-packages.tsv"
    cat "$logs/environment.log"
}
configure() {
    environment
    run_logged configure cmake -S "$repo" -B "$build" -G Ninja \
        -DCMAKE_CXX_COMPILER=clang++-22 -DCMAKE_BUILD_TYPE=Release \
        '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG' \
        -DFAIO_ENABLE_IO_URING=ON -DFAIO_BUILD_TESTS=ON -DFAIO_BUILD_EXAMPLES=ON \
        -DFAIO_BUILD_BENCHMARKS=ON -DFAIO_INSTALL=ON \
        -DFAIO_USE_WORKSPACE_DEPS=ON -DFAIO_DEPENDENCY_ROOT="$dependency_root"
}
compile() {
    run_logged build cmake --build "$build" --parallel "$jobs"
}
test_backend() {
    local backend="$1"
    run_logged "ctest-$backend" env FAIO_TEST_IO_BACKEND="$backend" \
        ctest --test-dir "$build" --verbose --output-on-failure --timeout 120 --parallel 1
    cp "$build/Testing/Temporary/LastTest.log" "$logs/ctest-$backend-full.log"
}
examples() {
    local backend
    for backend in epoll uring; do
        mkdir -p "$build/examples/manual-$backend"
        (
            cd "$build/examples/manual-$backend"
            run_logged "file-example-$backend" "$build/examples/file_and_directory" "--io-backend=$backend"
        )
        # 现有 Python 回包脚本本身跨平台；环境变量实际选择示例的 Linux 后端。
        run_logged "tcp-example-$backend" env FAIO_TEST_IO_BACKEND="$backend" python3 \
            "$repo/scripts/check_windows_servers.py" --executable "$build/examples/tcp_echo_server" --protocol tcp --port 8080
        run_logged "tcp-single-example-$backend" env FAIO_TEST_IO_BACKEND="$backend" python3 \
            "$repo/scripts/check_windows_servers.py" --executable "$build/examples/tcp_echo_server_single_thread" --protocol tcp --port 8080
        run_logged "udp-example-$backend" env FAIO_TEST_IO_BACKEND="$backend" python3 \
            "$repo/scripts/check_windows_servers.py" --executable "$build/examples/udp_echo_server" --protocol udp --port 9090
    done
}
consumer() {
    run_logged installed-consumer python3 "$repo/scripts/check_installed_headers.py" --build "$build" --compiler clang++-22
}
summary() {
    # GoogleTest 汇总重复列出 skipped 名称，先按完整用例名去重，再报告真实数量。
    python3 - "$logs" <<'PY'
import json
from pathlib import Path
import re
import sys

logs = Path(sys.argv[1])
result = {}
for backend in ("epoll", "uring"):
    detail = (logs / f"ctest-{backend}-full.log").read_text()
    transcript = (logs / f"ctest-{backend}.log").read_text()
    skipped = sorted(set(re.findall(r"^\[  SKIPPED \] ([^ \r\n]+\.[^ \r\n]+)", detail, re.M)))
    cases = sum(int(value) for value in re.findall(r"\[==========\] (\d+) tests? from .* ran\.", detail))
    result[backend] = {
        "ctest_targets": int(re.search(r"0 tests failed out of (\d+)", transcript).group(1)),
        "gtest_cases": cases,
        "passed_cases": cases - len(skipped),
        "skipped_cases": skipped,
    }
consumer_log = (logs / "installed-consumer.log").read_text()
result["installed_headers"] = int(re.search(r"PASS: (\d+) self-contained", consumer_log).group(1))
result["manual_backend_examples"] = len(list(logs.glob("*example-*.log")))
text = json.dumps(result, indent=2, ensure_ascii=False) + "\n"
(logs / "validation-summary.json").write_text(text)
print(text, end="")
PY
}

case "$phase" in
    download) download ;;
    install) install ;;
    bootstrap) download; install ;;
    environment) environment ;;
    configure) configure ;;
    build) compile ;;
    epoll|uring) test_backend "$phase" ;;
    examples) examples ;;
    consumer) consumer ;;
    summary) summary ;;
    verify) configure; compile; test_backend epoll; test_backend uring; examples; consumer; summary ;;
    *) echo "phase must be download|install|bootstrap|environment|configure|build|epoll|uring|examples|consumer|summary|verify" >&2; exit 2 ;;
esac

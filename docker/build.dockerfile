ARG DEV_IMAGE=faio:dev
FROM ${DEV_IMAGE}

WORKDIR /workspace/faio
COPY . .

ARG CMAKE_BUILD_TYPE=Release
ARG BUILD_JOBS=4

# 容器复用自身工具链，显式配置 Release/Debug，不再维护重复的 Docker 预设。
# 此处只编译测试；运行测试时仍需容器 seccomp 策略允许 io_uring。
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    && cmake --build build --parallel "${BUILD_JOBS}" \
    && cargo build --manifest-path benchmark/tcp/tokio-benchmark/Cargo.toml --release --locked --offline \
    && cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked --offline

CMD ["/bin/bash"]

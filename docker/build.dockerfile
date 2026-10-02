ARG DEV_IMAGE=faio:dev
FROM ${DEV_IMAGE}

WORKDIR /workspace/faio
COPY . .

ARG CMAKE_PRESET=docker-release
ARG BUILD_JOBS=4

# Tests are compiled here; run them in a container whose seccomp policy permits io_uring.
RUN cmake --preset "${CMAKE_PRESET}" \
    && cmake --build --preset "${CMAKE_PRESET}" --parallel "${BUILD_JOBS}" \
    && cargo build --manifest-path benchmark/tcp/tokio-benchmark/Cargo.toml --release --locked --offline \
    && cargo build --manifest-path benchmark/coro/tokio-benchmark/Cargo.toml --release --locked --offline

CMD ["/bin/bash"]

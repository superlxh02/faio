# syntax=docker/dockerfile:1.7
ARG BASE_IMAGE=fedora:45
FROM ${BASE_IMAGE}

COPY docker/setup.sh docker/bootstrap_gcc16.1.sh docker/verify_experimental_toolchain.sh docker/gcc16.1.sha256 /opt/faio-toolchain/
COPY scripts/bootstrap_experimental_deps.py scripts/experimental_dependencies.lock.json /opt/faio-build/scripts/
ARG KOJI_ADDRESS=
RUN --mount=type=cache,target=/var/cache/faio-gcc16.1,sharing=locked \
    --mount=type=cache,target=/var/cache/libdnf5,sharing=locked \
    FAIO_GCC_RPM_CACHE=/var/cache/faio-gcc16.1 bash /opt/faio-toolchain/setup.sh base

RUN bash /opt/faio-toolchain/setup.sh vcpkg

ENV PATH=/opt/gcc-16.1/usr/bin:/opt/vcpkg:$PATH \
    LD_LIBRARY_PATH=/opt/gcc-16.1/usr/lib64:/opt/gcc-16.1/lib64 \
    VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_FORCE_SYSTEM_BINARIES=1 \
    CC=faio-cc CXX=faio-cxx \
    CMAKE_GENERATOR=Ninja \
    CMAKE_EXPORT_COMPILE_COMMANDS=ON \
    FAIO_STDEXEC_SOURCE_DIR=/opt/faio-deps/src/stdexec

RUN --mount=type=cache,target=/var/cache/libdnf5,sharing=locked bash /opt/faio-toolchain/setup.sh main

WORKDIR /workspace/faio
COPY . .
ARG BUILD_TYPE=Release
ARG BUILD_JOBS=4
RUN case "${BUILD_TYPE}" in Release) faio_preset=linux-main-release ;; Debug) faio_preset=linux-main-debug ;; *) exit 2 ;; esac \
    && cmake --preset "$faio_preset" -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build --preset "$faio_preset" --parallel "${BUILD_JOBS}" \
    && cmake --install "target/cmake-build-${faio_preset}" \
    && for faio_manifest in benchmark/coro/tokio-benchmark/Cargo.toml benchmark/io/tokio-benchmark/Cargo.toml benchmark/tcp/tokio-benchmark/Cargo.toml; do \
        if [ "${BUILD_TYPE}" = Release ]; then \
            cargo build --manifest-path "$faio_manifest" --locked --release --jobs "${BUILD_JOBS}"; \
        else \
            cargo build --manifest-path "$faio_manifest" --locked --jobs "${BUILD_JOBS}"; \
        fi || exit; \
    done

CMD ["/bin/bash"]

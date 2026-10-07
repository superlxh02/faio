ARG DEV_IMAGE=faio:experimental-dev
FROM ${DEV_IMAGE}

WORKDIR /workspace/faio
COPY . .

ARG CMAKE_BUILD_TYPE=Debug
ARG BUILD_JOBS=2
ARG FAIO_ENABLE_IO_URING=OFF
ARG FAIO_SANITIZERS=
ARG FAIO_RUN_EXPERIMENTAL_TESTS=ON
ARG FAIO_INCLUDE_ORIGINAL_P0_EVIDENCE=OFF

# The development image already contains the clean, locked dependency checkout.
RUN --network=none python3 /opt/faio-build/scripts/bootstrap_experimental_deps.py \
    --workspace-root /opt/faio-deps --verify-only
RUN --network=none bash /opt/faio-toolchain/verify_experimental_toolchain.sh \
    && cmake -S . -B build/experimental-base -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DFAIO_ENABLE_IO_URING="${FAIO_ENABLE_IO_URING}" \
        -DFAIO_SANITIZERS="${FAIO_SANITIZERS}" \
        -DFAIO_ENABLE_EXPERIMENTAL=OFF \
        -DFAIO_BUILD_TESTS=ON \
        -DFAIO_BUILD_BENCHMARKS=OFF \
    && cmake --build build/experimental-base --parallel "${BUILD_JOBS}" \
    && cmake -S . -B build/experimental-enabled -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DFAIO_SANITIZERS="${FAIO_SANITIZERS}" \
        -DFAIO_ENABLE_EXPERIMENTAL=ON \
        -DFAIO_EXPERIMENTAL_REFLECTION=ON \
        -DFAIO_EXPERIMENTAL_EXECUTION=ON \
        -DFAIO_ENABLE_IO_URING="${FAIO_ENABLE_IO_URING}" \
        -DFAIO_BUILD_TESTS=ON \
        -DFAIO_BUILD_EXAMPLES=ON \
        -DFAIO_BUILD_BENCHMARKS=OFF \
        -DFAIO_P0_ORIGINAL_STOP_TOKEN_EVIDENCE="${FAIO_INCLUDE_ORIGINAL_P0_EVIDENCE}" \
        -DFAIO_STDEXEC_SOURCE_DIR="${FAIO_STDEXEC_SOURCE_DIR}" \
    && cmake --build build/experimental-enabled --parallel "${BUILD_JOBS}" \
    && if [ "${FAIO_RUN_EXPERIMENTAL_TESTS}" = ON ]; then \
        FAIO_TEST_IO_BACKEND=epoll ctest --test-dir build/experimental-enabled \
            -L experimental -LE experimental-original-contract --output-on-failure; \
    fi

# Original raw std::stop_token failures remain optional compatibility evidence.
# Successful acceptance uses the approved minimal token wrapper instead. Root
# configure/build/tests and installed consumption execute without networking.
# io_uring tests additionally need an appropriate seccomp profile at runtime.
CMD ["/bin/bash"]

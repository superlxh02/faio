FROM ubuntu:26.04 AS toolchain

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        g++-15 \
        libspdlog-dev \
        liburing-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

ENV CXX=g++-15

FROM toolchain AS build

ARG BUILD_TYPE=Release
ARG BUILD_JOBS=4

WORKDIR /src/faio
COPY CMakeLists.txt LICENSE ./
COPY cmake/ cmake/
COPY include/ include/
COPY examples/ examples/

# Compile the examples to validate the headers, then install the header-only package.
RUN cmake -S . -B /tmp/faio-build -G Ninja \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DFAIO_BUILD_EXAMPLES=ON \
        -DFAIO_BUILD_TESTS=OFF \
        -DFAIO_BUILD_BENCHMARKS=OFF \
        -DFAIO_INSTALL=ON \
    && cmake --build /tmp/faio-build --parallel "${BUILD_JOBS}" \
    && cmake --install /tmp/faio-build

FROM toolchain AS installed

# Keep the toolchain and dependencies so downstream applications can compile against faio.
COPY --from=build /usr/local/ /usr/local/

WORKDIR /workspace
CMD ["/bin/bash"]

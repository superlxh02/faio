FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive \
    CARGO_HOME=/opt/cargo \
    MPLBACKEND=Agg

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        build-essential \
        ca-certificates \
        cargo \
        cmake \
        curl \
        gdb \
        git \
        libasio-dev \
        libgtest-dev \
        libspdlog-dev \
        liburing-dev \
        libtool \
        lsof \
        ninja-build \
        pkg-config \
        psmisc \
        python3 \
        python3-matplotlib \
        python3-numpy \
        python3-pandas \
        rustc \
        tar \
        unzip \
        wrk \
        zip \
    && rm -rf /var/lib/apt/lists/*

# Fetch each locked Rust benchmark dependency set without copying application sources.
WORKDIR /opt/faio-rust-deps/tcp
COPY benchmark/tcp/tokio-benchmark/Cargo.toml benchmark/tcp/tokio-benchmark/Cargo.lock ./
RUN mkdir -p src && printf 'fn main() {}\n' > src/main.rs && cargo fetch --locked

WORKDIR /opt/faio-rust-deps/coro
COPY benchmark/coro/tokio-benchmark/Cargo.toml benchmark/coro/tokio-benchmark/Cargo.lock ./
RUN mkdir -p src && printf 'fn main() {}\n' > src/main.rs && cargo fetch --locked

WORKDIR /workspace/faio
CMD ["/bin/bash"]

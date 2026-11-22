# syntax=docker/dockerfile:1.7
ARG BASE_IMAGE=fedora:45
FROM ${BASE_IMAGE}

## clice LSP，按架构固定版本与 SHA256。tarball 内为 clice/{bin,lib}，
## 解压到 /root/.local 后 clice 通过 ../lib/clang/23 定位内置头文件。
ARG CLICE_VERSION=1.0.0
ARG CLICE_SHA256_X86_64=9dd181aac9c81cae76b255e953da21c9e603fc5b406447eb26c95eaf67ed969a
ARG CLICE_SHA256_AARCH64=b7eb6ce65afb9cdc5229dadf1b1a561485dcdb292a06d68857cd6097716f08ba

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

RUN --mount=type=cache,target=/var/cache/libdnf5,sharing=locked \
    bash /opt/faio-toolchain/setup.sh main \
    && dnf install -y --setopt=install_weak_deps=False --setopt=tsflags=nodocs \
        --setopt=keepcache=True procps-ng
RUN python3 /opt/faio-build/scripts/bootstrap_experimental_deps.py --workspace-root /opt/faio-deps \
    && git config --system --add safe.directory /opt/faio-deps/src/stdexec \
    && ln -s /opt/faio-deps/src/stdexec/include/stdexec /usr/local/include/stdexec \
    && ln -s /opt/faio-deps/src/stdexec/include/exec /usr/local/include/exec

RUN --mount=type=cache,target=/var/cache/faio-clice,sharing=locked \
    set -eux; \
    clice_arch="$(uname -m)"; \
    case "$clice_arch" in \
        x86_64) clice_target="x86_64-unknown-linux-gnu"; clice_sha256="$CLICE_SHA256_X86_64" ;; \
        aarch64) clice_target="aarch64-unknown-linux-gnu"; clice_sha256="$CLICE_SHA256_AARCH64" ;; \
        *) echo "unsupported clice architecture: $clice_arch" >&2; exit 1 ;; \
    esac; \
    clice_archive="/var/cache/faio-clice/clice-${CLICE_VERSION}-${clice_target}.tar.gz"; \
    curl -fsSL --retry 3 -o "$clice_archive" \
        "https://github.com/clice-io/clice/releases/download/v${CLICE_VERSION}/clice-${CLICE_VERSION}.${clice_target}.tar.gz"; \
    echo "${clice_sha256}  ${clice_archive}" | sha256sum -c -; \
    tar -xzf "$clice_archive" -C /tmp; \
    mkdir -p /root/.local; \
    cp -a /tmp/clice/bin /tmp/clice/lib /root/.local/; \
    mkdir -p /usr/local/share/licenses/faio-clice; \
    cp /tmp/clice/clice.toml /usr/local/share/licenses/faio-clice/clice.toml.example; \
    cp /tmp/clice/LICENSE /usr/local/share/licenses/faio-clice/LICENSE; \
    rm -rf /tmp/clice; \
    /root/.local/bin/clice --version

# Install the Bazel release selected by this workspace, including its embedded JDK.
COPY --chmod=0755 docker/bazel-dispatch /usr/local/bin/bazel
RUN --mount=type=bind,source=.bazelversion,target=/tmp/faio-bazel-version \
    set -eu; \
    bazel_version="$(cat /tmp/faio-bazel-version)"; \
    case "${bazel_version}/$(uname -m)" in \
        9.2.0/aarch64) bazel_arch=arm64; bazel_sha=049dd21f40ad979db11c3ee68c96a42ce75f1185e69ac61ab20de1501427a410 ;; \
        9.2.0/x86_64) bazel_arch=x86_64; bazel_sha=7668a95db1250f12c40407251e4e203b4ec8bf39bc495d2f485b2d8c99048694 ;; \
        *) echo "No Bazel checksum for ${bazel_version} on $(uname -m)" >&2; exit 1 ;; \
    esac; \
    curl -fsSL --retry 3 -o /tmp/bazel \
        "https://releases.bazel.build/${bazel_version}/release/bazel-${bazel_version}-linux-${bazel_arch}"; \
    echo "${bazel_sha}  /tmp/bazel" | sha256sum -c -; \
    install -D -m 0755 /tmp/bazel /usr/local/libexec/bazel-real; \
    rm /tmp/bazel; \
    test "$(bazel --version)" = "bazel ${bazel_version}"

# The release archive includes the sources needed to build xmake on both architectures.
ARG XMAKE_VERSION=3.0.9
ARG XMAKE_SHA256=45335220e83cee7ab9ad9e50900f9e751f32510e204719f89c5866c143ef3a5a
ENV XMAKE_ROOT=y
RUN set -eu; \
    curl -fsSL --retry 3 -o /tmp/xmake.tar.gz \
        "https://github.com/xmake-io/xmake/releases/download/v${XMAKE_VERSION}/xmake-v${XMAKE_VERSION}.tar.gz"; \
    echo "${XMAKE_SHA256}  /tmp/xmake.tar.gz" | sha256sum -c -; \
    mkdir /tmp/faio-xmake; \
    tar -xzf /tmp/xmake.tar.gz --strip-components=1 -C /tmp/faio-xmake; \
    cd /tmp/faio-xmake; \
    CC=/usr/bin/clang CXX=/usr/bin/clang++ ./configure --prefix=/usr/local; \
    make -j4; \
    make install; \
    cd /; \
    rm -rf /tmp/faio-xmake /tmp/xmake.tar.gz; \
    xmake --version | grep -F "v${XMAKE_VERSION}"

ENV PATH=/root/.local/bin:$PATH

WORKDIR /workspace/faio
CMD ["/bin/bash"]

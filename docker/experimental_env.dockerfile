ARG BASE_IMAGE=fedora:rawhide
ARG GCC_RPM_SOURCE=scratch
FROM ${GCC_RPM_SOURCE} AS gcc_rpm_input

FROM ${BASE_IMAGE}

LABEL org.opencontainers.image.title="faio-experimental-gcc16.1" \
      org.opencontainers.image.description="Fedora Rawhide with isolated, checksum-pinned GCC/libstdc++ 16.1.1-2.fc44"

# Rawhide supplies the tools and faio dependencies. Its moving GCC is kept only
# for system packages; the experimental compiler is pinned independently below.
RUN dnf install -y --setopt=install_weak_deps=False --setopt=tsflags=nodocs \
        binutils \
        ca-certificates \
        ccache \
        cmake \
        cpio \
        curl \
        gcc \
        gcc-c++ \
        gdb \
        git \
        gtest-devel \
        liburing-devel \
        make \
        ninja-build \
        pkgconf-pkg-config \
        python3 \
        spdlog-devel \
    && dnf clean all \
    && rm -rf /var/cache/dnf

COPY docker/bootstrap_gcc16.1.sh docker/verify_experimental_toolchain.sh docker/gcc16.1.sha256 /opt/faio-toolchain/
ARG KOJI_ADDRESS=
RUN --mount=type=bind,from=gcc_rpm_input,target=/opt/faio-rpm-input,readonly \
    --mount=type=cache,target=/var/cache/faio-gcc16.1,sharing=locked \
    FAIO_GCC_RPM_CACHE=/var/cache/faio-gcc16.1 KOJI_ADDRESS="${KOJI_ADDRESS}" \
    FAIO_GCC_RPM_IMPORT=/opt/faio-rpm-input \
    bash /opt/faio-toolchain/bootstrap_gcc16.1.sh

ENV CC=/opt/gcc-16.1/usr/bin/gcc \
    CXX=/opt/gcc-16.1/usr/bin/g++ \
    PATH=/opt/gcc-16.1/usr/bin:$PATH \
    LD_LIBRARY_PATH=/opt/gcc-16.1/usr/lib64:/opt/gcc-16.1/lib64 \
    CMAKE_GENERATOR=Ninja \
    CMAKE_EXPORT_COMPILE_COMMANDS=ON

# Keep the large tool/dependency layer reusable when the optional benchmark
# dependency changes. CLion's default project configuration includes benchmarks.
RUN dnf install -y --setopt=install_weak_deps=False --setopt=tsflags=nodocs asio-devel \
    && dnf clean all \
    && rm -rf /var/cache/dnf

# The dependency lives outside /workspace, which IDEs replace with a bind mount.
# Configure remains offline; all fetching takes place while building this image.
COPY scripts/bootstrap_experimental_deps.py scripts/experimental_dependencies.lock.json /opt/faio-build/scripts/
RUN python3 /opt/faio-build/scripts/bootstrap_experimental_deps.py --workspace-root /opt/faio-deps \
    && git config --system --add safe.directory /opt/faio-deps/src/stdexec \
    && ln -s /opt/faio-deps/src/stdexec/include/stdexec /usr/local/include/stdexec \
    && ln -s /opt/faio-deps/src/stdexec/include/exec /usr/local/include/exec \
    && mv /usr/bin/gcc /usr/bin/gcc.rawhide \
    && mv /usr/bin/g++ /usr/bin/g++.rawhide \
    && ln -s /opt/gcc-16.1/usr/bin/gcc /usr/bin/gcc \
    && ln -s /opt/gcc-16.1/usr/bin/g++ /usr/bin/g++

ENV FAIO_STDEXEC_SOURCE_DIR=/opt/faio-deps/src/stdexec \
    FAIO_ENABLE_EXPERIMENTAL=ON \
    FAIO_EXPERIMENTAL_REFLECTION=ON \
    FAIO_EXPERIMENTAL_EXECUTION=ON \
    FAIO_EXPERIMENTAL_ENTRY_MODE=NORMAL

COPY docker/verify_experimental_clion.sh /opt/faio-toolchain/
# This verifies <meta>, annotations, extract and function splice on real GCC
# 16.1, and links each sanitizer configuration. It does not certify faio P0.
RUN bash /opt/faio-toolchain/verify_experimental_toolchain.sh
RUN bash /opt/faio-toolchain/verify_experimental_clion.sh

WORKDIR /workspace/faio
CMD ["/bin/bash"]

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

RUN --mount=type=cache,target=/var/cache/libdnf5,sharing=locked \
    bash /opt/faio-toolchain/setup.sh main \
    && dnf install -y --setopt=install_weak_deps=False --setopt=tsflags=nodocs \
        --setopt=keepcache=True procps-ng
RUN python3 /opt/faio-build/scripts/bootstrap_experimental_deps.py --workspace-root /opt/faio-deps \
    && git config --system --add safe.directory /opt/faio-deps/src/stdexec \
    && ln -s /opt/faio-deps/src/stdexec/include/stdexec /usr/local/include/stdexec \
    && ln -s /opt/faio-deps/src/stdexec/include/exec /usr/local/include/exec

WORKDIR /workspace/faio
CMD ["/bin/bash"]

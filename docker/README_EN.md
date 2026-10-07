# Docker Development Environment

[简体中文](README.md) | **English**

This directory supports faio development, debugging, and benchmarking. Run all commands from the repository root; the
build context is `.`.

| File                                 | Purpose                                                                                                                                                   |
|--------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------|
| [env.dockerfile](env.dockerfile)     | Installs GCC, CMake, debuggers, test and benchmark dependencies, and caches the locked dependencies of both Tokio projects; does not compile faio sources |
| [build.dockerfile](build.dockerfile) | Copies the sources onto the environment image and builds the C++ examples, tests, benchmarks, and both Rust benchmarks                                    |

Create the environment image and develop with the working directory mounted:

```bash
docker build -f docker/env.dockerfile -t faio:dev .
docker run --rm -it --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" -w /workspace/faio faio:dev

# Inside the container
cmake -S . -B build/docker-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/docker-debug -j4
ctest --test-dir build/docker-debug --output-on-failure
```

Build a development image containing the build artifacts and run the tests:

```bash
docker build -f docker/build.dockerfile -t faio:build .
docker run --rm --security-opt seccomp=unconfined faio:build \
  ctest --test-dir build --output-on-failure
```

The build image accepts the `DEV_IMAGE`, `CMAKE_BUILD_TYPE`, and `BUILD_JOBS` build arguments, for example
`--build-arg CMAKE_BUILD_TYPE=Debug`. Rebuild the environment image when dependencies change; for source changes,
rebuilding the build image is enough, since Rust compilation uses the cached dependencies offline.

`seccomp=unconfined` allows tests and examples to call io_uring; the Docker build stage only compiles and never runs
io_uring-dependent programs. Actual execution depends on host kernel support for the corresponding operations.

## GCC 16.1 experimental environment and P0

[experimental_env.dockerfile](experimental_env.dockerfile) uses Fedora Rawhide for tools and system dependencies,
then isolates the pinned Fedora Koji GCC build `16.1.1-2.fc44` under `/opt/gcc-16.1`.
The compiler, libstdc++, libgcc, and sanitizer RPMs share that build and are checked against
[gcc16.1.sha256](gcc16.1.sha256). It requires CMake >= 4.1 and compiles/runs a real reflection probe,
then separately checks ASan+UBSan and TSan linking. Rawhide's changing system GCC is not the experimental baseline.
The image includes the locked stdexec checkout, spdlog, GoogleTest, liburing, Asio, CMake, Ninja, and gdb.
stdexec lives at `/opt/faio-deps/src/stdexec`, outside the workspace bind mounts used by IDEs.

```bash
docker build -f docker/experimental_env.dockerfile -t faio:experimental-dev .
docker run --rm -it --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" \
  -w /workspace/faio faio:experimental-dev

# Inside the container: ENV supplies the experimental defaults and local dependency.
cmake -S . -B build/docker-experimental -DCMAKE_BUILD_TYPE=Debug
cmake --build build/docker-experimental -j2 \
  --target faio_experimental_execution_current_thread faio_experimental_async_main_execution
```

Image ENV defaults enable `FAIO_ENABLE_EXPERIMENTAL`, `FAIO_EXPERIMENTAL_REFLECTION`, and
`FAIO_EXPERIMENTAL_EXECUTION`, with `FAIO_STDEXEC_SOURCE_DIR=/opt/faio-deps/src/stdexec`.
The entry default is `NORMAL`. ASYNC sources opt into the one-shot `main(entry_name)` marker macro;
`faio_configure_entry(... MODE ASYNC)` only configures target properties and does not generate main. Sanitizers and original failing compatibility evidence remain opt-in.
Explicit `-D`, presets, and existing CMake cache values override ENV. Use `-DFAIO_ENABLE_EXPERIMENTAL=OFF`
for a base-only project.
Targets that include execution headers must still link `faio::experimental_execution`
to receive the capability definition and stdexec include path. The
`faio_experimental_async_main` example now links this component when execution is enabled.

In a CLion Docker toolchain, select `faio:experimental-dev`; `/usr/bin/gcc` and `/usr/bin/g++`
both point to the paired GCC 16.1 compiler. Use `/usr/bin/gdb`, `/usr/bin/cmake`, and `/usr/bin/ninja`
for the remaining tools. Original Rawhide drivers are preserved as `/usr/bin/gcc.rawhide` and
`/usr/bin/g++.rawhide`. After updating the image, recreate or redetect the toolchain container and use
**Reset Cache and Reload Project**, since an old container or cached experimental OFF value takes precedence.
No entrypoint initialization, dependency bind mount, `CPATH`, or global compiler flags are needed.
Bare `#include <exec/task.hpp>` resolves through the image's default headers; CMake targets still supply
the language standard and capability definitions. The fixed dependency's scoped Git safe.directory supports
reading the checkout with a non-root UID.

[experimental_build.dockerfile](experimental_build.dockerfile) builds two independent directories:
`build/experimental-base` keeps C++23 and experimental OFF; `build/experimental-enabled`
enables reflection/execution and builds the base and experimental examples/tests.
Configuration, compilation, and experimental CTest run in `RUN --network=none`; dependency
the development image already contains the fixed dependency.

```bash
docker build -f docker/experimental_build.dockerfile -t faio:experimental-build .
docker run --rm --security-opt seccomp=unconfined faio:experimental-build \
  ctest --test-dir build/experimental-enabled --output-on-failure
```

Build arguments are `DEV_IMAGE`, `CMAKE_BUILD_TYPE`, `BUILD_JOBS`, `FAIO_ENABLE_IO_URING`
(default OFF), `FAIO_SANITIZERS`, `FAIO_RUN_EXPERIMENTAL_TESTS` (default ON), and
`FAIO_INCLUDE_ORIGINAL_P0_EVIDENCE` (default OFF). Use `address,undefined` for ASan+UBSan;
TSan needs a separate image with `thread`.

The original raw `std::stop_token` failures remain saved compatibility evidence. Acceptance
uses the approved minimal token wrapper; all three positive P0 probes passed on real GCC 16.1.
Set `FAIO_P0_ORIGINAL_STOP_TOKEN_EVIDENCE=ON` in the independent P0 configuration to rerun
those failures. They honestly produce failing CTest results.

On 2026-10-06 the aarch64 environment ran GCC `16.1.1 20260515 (Red Hat 16.1.1-2)`,
libstdc++ release `16`, header date `20260515`, runtime `libstdc++.so.6.0.35`, CMake `4.3.0`, and Ninja `1.13.2`.
Reflection compile/run and independent sanitizer link checks passed. The x86_64 RPMs have been downloaded and
checksummed, but have not received runtime validation. See the [experimental build guide](../docs/experimental/build.md)
for executed checks and outstanding environment restrictions.
This iteration validates the development image, offline default configuration, finite examples, and debugger;
it does not run the complete test suite or certify the complete build image.

`BASE_IMAGE` can reuse an existing Fedora image. An optional `KOJI_ADDRESS` supplies an IPv4 DNS override for
`kojipkgs.fedoraproject.org` while keeping HTTPS hostname/certificate verification. The local DNS proxy interrupted
Koji connections during verification; this run used `BASE_IMAGE=modern-cpp-rawhide:1.0` and
`KOJI_ADDRESS=38.145.32.21`. Normal networks do not need this override; check official DNS before reusing that address.

An existing RPM cache can be imported through a BuildKit named context. It must contain `aarch64/*.rpm` or
`x86_64/*.rpm`; SHA256 and Fedora NVR checks still apply. This was the successful environment image build:

```bash
docker build -f docker/experimental_env.dockerfile -t faio:experimental-dev \
  --build-arg BASE_IMAGE=modern-cpp-rawhide:1.0 \
  --build-context gcc-rpm-cache=/tmp/faio-gcc16.1-rpms \
  --build-arg GCC_RPM_SOURCE=gcc-rpm-cache .
```

Verified RPMs are retained in a separate BuildKit cache for subsequent builds.

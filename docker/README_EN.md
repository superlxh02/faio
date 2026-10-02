# Docker Development Environment

[简体中文](README.md) | **English**

This directory supports faio development, debugging, and benchmarking. Run all commands from the repository root; the build context is `.`.

| File | Purpose |
| --- | --- |
| [env.dockerfile](env.dockerfile) | Installs GCC, CMake, debuggers, test and benchmark dependencies, and caches the locked dependencies of both Tokio projects; does not compile faio sources |
| [build.dockerfile](build.dockerfile) | Copies the sources onto the environment image and builds the C++ examples, tests, benchmarks, and both Rust benchmarks |

Create the environment image and develop with the working directory mounted:

```bash
docker build -f docker/env.dockerfile -t faio:dev .
docker run --rm -it --security-opt seccomp=unconfined \
  -v "$PWD:/workspace/faio" -w /workspace/faio faio:dev

# Inside the container
cmake --preset docker-debug
cmake --build --preset docker-debug -j4
ctest --preset docker-debug
```

Build a development image containing the build artifacts and run the tests:

```bash
docker build -f docker/build.dockerfile -t faio:build .
docker run --rm --security-opt seccomp=unconfined faio:build \
  ctest --preset docker-release
```

The build image accepts the `DEV_IMAGE`, `CMAKE_PRESET`, and `BUILD_JOBS` build arguments, for example `--build-arg CMAKE_PRESET=docker-debug`. Rebuild the environment image when dependencies change; for source changes, rebuilding the build image is enough, since Rust compilation uses the cached dependencies offline.

`seccomp=unconfined` allows tests and examples to call io_uring; the Docker build stage only compiles and never runs io_uring-dependent programs. Actual execution depends on host kernel support for the corresponding operations.

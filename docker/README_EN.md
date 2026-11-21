# Docker toolchains

Run Docker commands from the repository root with `.` as the build context.

| Dockerfile | Purpose |
|---|---|
| `dev-image.dockerfile` | CLion environment with Clang 23, GCC 16.1, clangd and locked stdexec |
| `duild-all.dockerfile` | Build and install all components, including experimental features |
| `build-main.dockerfile` | Build and install stable components with Clang 23 and GCC 16.1 available |
| Root `Dockerfile` | Identical to `duild-all.dockerfile` |

Dependencies are installed globally by DNF. vcpkg is available at `/opt/vcpkg` as a fallback and is not used to install project dependencies. The locked experimental stdexec checkout is bootstrapped separately. The full-build image includes clangd and its runtime libraries without other LLVM command-line tools.

```sh
docker build -f docker/dev-image.dockerfile -t faio:dev .
docker build -f docker/build-main.dockerfile -t faio:main .
docker build -t faio:all .
```

Build images compile C++ examples, unit tests, benchmarks and three locked Tokio comparisons. `BUILD_TYPE=Release|Debug` and `BUILD_JOBS` control configuration. Runtime I/O tests depend on host kernel and container permissions.

Experimental components currently require Linux and GCC 16.1. macOS/Windows `all` presets request these features and report the platform restriction; `main` presets build stable components.

See the [Chinese guide](README.md) and [CLion setup](CLion.md).

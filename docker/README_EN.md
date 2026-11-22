# Docker toolchains

These images solve one problem: the machine has neither GCC 16.1 nor Clang 23, and you would rather not install them along with a pile of system libraries. Use the development image for day-to-day work and one of the other two when you need a built and installed copy of faio. Run every command from the repository root with `.` as the build context. Both x86_64 and aarch64 are supported.

| Dockerfile | Compiler | What it is for |
|---|---|---|
| `dev-image.dockerfile` | Clang 23, plus GCC 16.1 | CLion development and debugging environment, usable as a dev container |
| `duild-all.dockerfile` | GCC 16.1 | Build and install every component, experimental ones included |
| `build-main.dockerfile` | Clang 23, plus GCC 16.1 | Build and install the stable components |

The `Dockerfile` in the repository root matches `duild-all.dockerfile`, so either one works.

System dependencies come from dnf: spdlog, GoogleTest, Asio, OpenSSL, liburing and gperftools are installed globally, and CMake needs no network access during configure. vcpkg is installed at `/opt/vcpkg` for callers who want it; the image build does not use it to fetch project dependencies. The locked experimental stdexec checkout comes from `scripts/experimental_dependencies.lock.json` and lands in `/opt/faio-deps/src/stdexec`, with symlinks under `/usr/local/include`.

GCC 16.1 and its runtime libraries come from the SHA256-pinned Fedora `16.1.1-2.fc44` RPMs, installed into `/opt/gcc-16.1`. The image then points `/usr/bin/gcc` and `/usr/bin/g++` at that toolchain and puts the matching libstdc++ on `LD_LIBRARY_PATH`. Clang is handled differently depending on the image. The development and stable-feature images install clang 23 and pass `--gcc-toolchain=/opt/gcc-16.1/usr`, so Clang compiles against the GCC 16.1 standard library. The all-features image and the root Dockerfile install no Clang compiler at all and only extract clangd plus its runtime libraries from clang-tools-extra. The experimental toolchain check fails if it finds clang, clang-tidy, LLD or LLDB in that image, which is why clangd is the only LLVM tool left there.

```sh
docker build -f docker/dev-image.dockerfile -t faio:dev .
docker build -f docker/duild-all.dockerfile -t faio:all .
docker build -f docker/build-main.dockerfile -t faio:main .
docker build -t faio:all .
```

The last command builds the root Dockerfile and produces the same image as `faio:all`. A build compiles every C++ example, the unit tests and the benchmarks, then builds the three locked Tokio comparison programs under `benchmark/{coro,io,tcp}/tokio-benchmark` and runs `cmake --install` into `/usr/local`. `BUILD_TYPE` takes `Release` or `Debug` and defaults to Release; `BUILD_JOBS` sets the `--parallel` count and defaults to 4. Output goes to `target/cmake-build-linux-{all,main}-<build-type>/`. Nothing that depends on the host kernel runs during the build.

```sh
docker build -f docker/build-main.dockerfile --build-arg BUILD_TYPE=Debug --build-arg BUILD_JOBS=2 -t faio:main-debug .
docker run --rm --security-opt seccomp=unconfined faio:all ctest --preset linux-all-release
docker run --rm -it -v "$PWD:/workspace/faio" -w /workspace/faio faio:dev
```

Whether io_uring works depends on the host kernel and on the container's security profile allowing the relevant system calls, which is why the second command passes `--security-opt seccomp=unconfined`. To use epoll under the default seccomp profile, configure with `-DFAIO_ENABLE_IO_URING=OFF`. `BASE_IMAGE` replaces the default `fedora:45`; a replacement must already provide Clang 23 and the dependencies listed above. `KOJI_ADDRESS` overrides DNS resolution for the official RPM repositories for a single build, and HTTPS certificates are still verified by hostname.

## Dev container

`.devcontainer/devcontainer.json` reuses `dev-image.dockerfile`. CLion tags the devcontainer image as `jb-devcontainer-<name>`, so `name` has to be a valid Docker repository name: lowercase letters, digits, `.`, `_` and `-` only. A space, `·`, `/` or `+` fails the build with `invalid tag ... invalid reference format`.

The development image ships Bazel and xmake, so the container is ready as soon as it starts. Bazel is pinned to 9.2.0 by `.bazelversion`, downloaded as the official release for the machine's architecture with a SHA256 check, and `/usr/local/bin/bazel` is a dispatch wrapper that locates the workspace root; xmake is 3.0.9, built from a checksummed source archive. CMake, Bazel and xmake share one output convention under `target/`:

| Build system | Output |
|---|---|
| CMake | `target/cmake-build-<preset>/` |
| Bazel | `target/bazel/`, including convenience symlinks |
| xmake | `target/xmake/` |

The devcontainer mounts `target/` as a named volume, so build output stays in the volume instead of scattering into your host checkout. `.bazelrc` keeps Bazel in the same place by setting both `--output_user_root` and `--symlink_prefix` to `target/bazel/`, which means a `bazel` invocation from any subdirectory still writes under the repository root. For xmake, `builddir` is set to `target/xmake` in `xmake.lua`, and the devcontainer also points `XMAKE_CONFIGDIR` there.

The image also unpacks SHA256-pinned [clice](https://clice.io) 1.0.0 into `/root/.local`, with the binary at `/root/.local/bin/clice`, its `lib/clang/23` resources kept in place, and the directory added to `PATH`. `postCreateCommand` configures `cmake --preset linux-all-debug` and then links `target/compile_commands.json` to `target/cmake-build-linux-all-debug/compile_commands.json`.

The repository's `clice.toml` points clice at the compilation database under `target`, which keeps it from picking up the `cmake-build-*` directories CLion writes. The index cache lives on local disk at `/tmp/faio-clice-cache` because the dev container's `/workspace` is a macOS shared mount and clice's index needs local-filesystem semantics. VS Code enables the editor through the `clice-io.clice` extension, and `clice.executable` in `.devcontainer/devcontainer.json` points at the binary above. Zed needs the clice extension installed from the marketplace; `.zed/settings.json` already points `lsp.clice.binary.path` at the same path. Run Zed on the host and that path has to become a local clice install, or the setting should be dropped so the extension uses its bundled or downloaded build.

`clice.toml` adds no compiler options of its own. Experimental targets get `-std=c++26` from CMake's compilation database, and clice consumes it as written. GCC 16.1's reflection extension (`-freflection` and `<meta>`) is not a dialect clice's bundled clang 23 supports, so reflection sources keep showing diagnostics in clice, while the real compilation still runs on GCC and the build is unaffected. clangd stays installed and configured but is disabled by default: `clangd.enable: false` in VS Code, and `language_servers: ["clice", "!clangd"]` in Zed.

The image additionally installs `openssl3-libs`. Fedora 45 ships OpenSSL 4 only, and CLion's .NET (Rider/radler) index backend still links `libssl.so.3`, so without the package the backend dies with `No usable version of libssl was found` and exit code 134.

## CMake presets

Preset names follow `{platform}-{features}-{build type}`, and the configure, build and test presets all use the same name. Linux uses `linux-{all,main}-{release,debug}`, macOS uses `mac-{all,main}-{release,debug}`, and Windows uses `windows-{clang,mingw,msvc}-{all,main}-{release,debug}`.

`all` asks explicitly for every experimental component. The experimental implementation requires Linux, GCC 16.1 and a matching libstdc++, so requesting `all` on macOS or Windows reports the platform restriction during configure, and those platforms build stable code with `main`. On macOS, `main` needs the Homebrew LLVM 23 `bin` directory on `PATH` and the Homebrew dependency prefix set. On Windows, `main` uses clang-cl, MinGW-w64 and MSVC respectively, each with dependencies built by that compiler.

CLion setup steps are in [CLion.md](CLion.md).

Toolchain sources: [Fedora GCC](https://packages.fedoraproject.org/pkgs/gcc/gcc/), [Fedora Clang 23](https://packages.fedoraproject.org/pkgs/llvm/clang/fedora-45.html), the [clangd package](https://packages.fedoraproject.org/pkgs/llvm/clang-tools-extra/), and the [pinned vcpkg release](https://github.com/microsoft/vcpkg/releases/tag/2026.10.16).

## Linux allocator

`FAIO_ALLOCATOR=AUTO|SYSTEM|TCMALLOC` selects the process allocator. `AUTO` uses tcmalloc on Linux when it is present and no sanitizer is enabled, and falls back to the system allocator everywhere else, including sanitizer builds. Setting `TCMALLOC` explicitly requires Linux and a usable gperftools, and cannot be combined with sanitizers.

Linking tcmalloc replaces malloc and new for the whole process. Coroutine frame caching and the large-frame pool keep working, and `FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW` stays OFF by default. When another project consumes the installed package, it resolves the allocator from the same option, so `SYSTEM` needs no gperftools. Sanitizer detection covers the shared CMake flags and the configuration-specific ones. A package import cannot see sanitizers you add later with `target_compile_options`, so when you attach sanitizers to a final target, either configure the consumer with `-DFAIO_ALLOCATOR=SYSTEM` or declare the sanitizer configuration through `FAIO_SANITIZERS`.

This page also exists in [Chinese](README.md), with the same content.

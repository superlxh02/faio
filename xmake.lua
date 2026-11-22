set_xmakever("3.0.1")
set_project("faio")
if os.scriptdir() == os.projectdir() then
    set_config("builddir", "target/xmake")
end
set_languages("c++23")
add_rules("mode.debug", "mode.release")

includes("tools/xmake/common.lua")

faio_xmake.feature("faio_headers", true, "Build the header-only library")
faio_xmake.feature("faio_modules", false, "Build the named C++ module")
faio_xmake.feature("faio_examples", true, "Build the faio examples")
faio_xmake.feature("faio_tests", true, "Build faio contract tests")
faio_xmake.feature("faio_benchmarks", false, "Build the header-only performance benchmarks")
faio_xmake.feature("faio_experimental", false, "Enable the Linux GCC 16.1 experimental headers")
faio_xmake.feature("faio_experimental_execution", true, "Enable locked stdexec support")

local has_headers = has_config("faio_headers")
local has_modules = has_config("faio_modules")
local use_tls = has_config("faio_tls")
local use_process = has_config("faio_process")
local use_header_experimental = has_headers and has_config("faio_experimental")
local use_experimental = has_modules and has_config("faio_module_experimental")
local use_reflection = has_config("faio_experimental_reflection")
local use_header_execution = has_config("faio_experimental_execution")
local use_module_execution = has_config("faio_module_experimental_execution")

if has_headers and not has_modules then
    faio_xmake.require_dependencies()
end

local function binary(name, dependency, ...)
    target(name)
        set_kind("binary")
        add_files(...)
        add_deps(dependency)
end

local function named_binary(name, dependency, ...)
    binary(name, dependency, ...)
        set_policy("build.c++.modules", true)
        set_policy("build.c++.modules.std", false)
        set_policy("build.c++.modules.culling", false)
        if use_experimental then
            set_languages("c++26")
        end
end

local function add_tls_test(name)
    local fixtures = path.join(os.scriptdir(), "tests", "fixtures")
    add_tests(name, {runargs = {
        path.join(fixtures, "server.pem"), path.join(fixtures, "server.key"),
        path.join(fixtures, "ca.pem")
    }})
end

if has_headers then
    target("faio")
        set_kind("headeronly")
        faio_xmake.shared_headers()
        faio_xmake.configure_core()
    target_end()

    if use_tls then
        target("faio_tls")
            set_kind("headeronly")
            add_deps("faio", {public = true})
            add_packages("openssl", {public = true})
            add_defines("FAIO_ENABLE_TLS=1", {public = true})
        target_end()
    end

    if use_process then
        target("faio_process")
            set_kind("headeronly")
            add_deps("faio", {public = true})
            add_defines("FAIO_ENABLE_PROCESS=1", {public = true})
            faio_xmake.configure_process()
        target_end()
    end

    if use_header_experimental then
        target("faio_experimental")
            set_kind("headeronly")
            add_deps("faio", {public = true})
            faio_xmake.experimental_variant(use_header_execution)
        target_end()
        if use_reflection and (has_config("faio_examples") or has_config("faio_tests")) then
            local function header_async_example(name, source)
                binary(name, "faio_experimental", "examples/experimental/" .. source)
                    set_languages("c++26")
                    if has_config("faio_tests") then
                        add_tests("run")
                    end
                target_end()
            end
            header_async_example("header_experimental_async_main", "entry/async_main.cpp")
            header_async_example("header_experimental_exit_cleanup", "entry/exit_cleanup.cpp")
            header_async_example("header_experimental_annotated_task", "lifecycle/annotated_task.cpp")
            if use_header_execution then
                header_async_example("header_experimental_sender_bridge", "execution/sender_bridge.cpp")
            end
        end
    end

    if has_config("faio_examples") or has_config("faio_tests") then
        binary("quickstart_header", "faio", "examples/quickstart.cpp")
            if has_config("faio_tests") then
                add_tests("header_contract")
            end
        target_end()
        if use_tls then
            binary("tls_echo_header", "faio_tls", "examples/tls/echo.cpp")
                if has_config("faio_tests") then
                    add_tls_test("tls")
                end
            target_end()
        end
        if use_process then
            binary("process_command_header", "faio_process", "examples/process/command.cpp")
                if has_config("faio_tests") then
                    add_tests("process")
                end
            target_end()
        end
    end
    if has_config("faio_examples") then
        for _, example in ipairs({
            {"coroutine_task", "coroutine/task.cpp"},
            {"composition_basics", "coroutine/composition_basics.cpp"},
            {"structured_concurrency", "coroutine/structured_concurrency.cpp"},
            {"batch_results", "coroutine/batch_results.cpp"},
            {"race_strategies", "coroutine/race_strategies.cpp"},
            {"coroutine_lifecycle", "coroutine/lifecycle.cpp"},
            {"cancellation_deadline", "coroutine/cancellation_deadline.cpp"},
            {"coroutine_sequence", "coroutine/sequence.cpp"},
            {"custom_awaiter", "coroutine/custom_awaiter.cpp"},
            {"serial_domain", "coroutine/serial_domain.cpp"},
            {"blocking_work", "runtime/execution.cpp"},
            {"shared_state", "sync/shared_state.cpp"},
            {"coordination", "sync/coordination.cpp"},
            {"io_stream", "io/stream.cpp"},
            {"file_and_directory", "fs/file_and_directory.cpp"},
            {"tcp_echo", "net/tcp_echo.cpp"},
            {"udp_echo", "net/udp_echo.cpp"},
        }) do
            binary(example[1], "faio", "examples/" .. example[2])
            target_end()
        end
    end
    if has_config("faio_tests") then
        local function header_contract(name, library, ...)
            binary(name, library, ...)
                add_defines("FAIO_USE_HEADERS=1")
        end
        header_contract("header_foundation_contract", "faio",
                        "tests/modules/foundation_main.cpp", "tests/modules/foundation_peer.cpp")
            add_tests("foundation")
        target_end()
        header_contract("header_core_contract", "faio", "tests/modules/core_contract.cpp")
            add_tests("core")
        target_end()
        if use_tls then
            header_contract("header_tls_contract", "faio_tls", "tests/modules/tls_contract.cpp")
                add_tls_test("tls")
            target_end()
        end
        if use_process then
            header_contract("header_process_contract", "faio_process", "tests/modules/process_contract.cpp")
                add_tests("process")
            target_end()
        end
        if use_tls and use_process then
            header_contract("header_both_contract", "faio_tls", "tests/modules/both_contract.cpp")
                add_deps("faio_process")
                add_tests("both")
            target_end()
        end
    end
end

if has_headers and has_config("faio_benchmarks") then
    for _, benchmark in ipairs({
        {"faio_tcp_benchmark", "tcp/faio_tcp_benchmark.cpp"},
        {"faio_coro_benchmark", "coro/faio_coro_benchmark.cpp"},
        {"faio_io_cpu_benchmark", "io/faio_io_cpu_benchmark.cpp"},
        {"faio_blocking_pool_benchmark", "io/blocking_pool_benchmark.cpp"},
        {"faio_mpsc_ready_queue_benchmark", "coro/mpsc_ready_queue_benchmark.cpp"},
    }) do
        binary(benchmark[1], "faio", "benchmark/" .. benchmark[2])
        target_end()
    end
    for _, name in ipairs({"faio_frame_allocation_benchmark", "faio_frame_allocation_new_benchmark"}) do
        binary(name, "faio", "benchmark/coro/frame_allocation_benchmark.cpp",
                             "benchmark/coro/frame_allocation_workloads.cpp")
            if name == "faio_frame_allocation_new_benchmark" then
                add_defines("FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW=1")
            end
        target_end()
    end
    if not is_plat("windows", "mingw") then
        binary("faio_current_thread_benchmark", "faio", "benchmark/coro/current_thread_benchmark.cpp")
        target_end()
        target("tcp_connect_load")
            set_kind("binary")
            add_files("benchmark/tcp/tcp_connect_load.cpp")
            add_syslinks("pthread")
        target_end()
    end
    target("tcp_echo_load")
        set_kind("binary")
        add_files("benchmark/tcp/tcp_echo_load.cpp")
        if is_plat("windows", "mingw") then
            add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_WIN32_WINNT=0x0A00")
            add_syslinks("ws2_32")
        else
            add_syslinks("pthread")
        end
    target_end()
    local asio = os.getenv("FAIO_ASIO_INCLUDE_DIR")
    if asio then
        binary("asio_tcp_benchmark", "faio", "benchmark/tcp/asio_tcp_benchmark.cpp")
            add_includedirs(asio)
            add_defines("ASIO_STANDALONE")
        target_end()
    end
end

if has_modules then
    includes("cxx-module")

    if has_config("faio_examples") or has_config("faio_tests") then
        named_binary("quickstart_module", "faio_module", "examples/module/quickstart.cpp")
            if has_config("faio_tests") then
                add_tests("quickstart")
            end
        target_end()
        if use_tls then
            named_binary("tls_echo_module", "faio_module_tls", "examples/tls/echo.cpp")
                add_defines("FAIO_USE_MODULES=1")
                if has_config("faio_tests") then
                    add_tls_test("tls")
                end
            target_end()
        end
        if use_process then
            named_binary("process_command_module", "faio_module_process", "examples/process/command.cpp")
                add_defines("FAIO_USE_MODULES=1")
                if has_config("faio_tests") then
                    add_tests("process")
                end
            target_end()
        end
    end
    if has_config("faio_tests") then
        named_binary("module_contract", "faio_module",
                     "tests/modules/foundation_main.cpp", "tests/modules/foundation_peer.cpp")
            add_tests("foundation")
        target_end()
        named_binary("module_core_contract", "faio_module", "tests/modules/core_contract.cpp")
            add_tests("core")
        target_end()
        if use_tls then
            named_binary("module_tls_contract", "faio_module_tls", "tests/modules/tls_contract.cpp")
                add_tls_test("tls")
            target_end()
        end
        if use_process then
            named_binary("module_process_contract", "faio_module_process", "tests/modules/process_contract.cpp")
                add_tests("process")
            target_end()
        end
        if use_tls and use_process then
            named_binary("module_both_contract", "faio_module_tls", "tests/modules/both_contract.cpp")
                add_deps("faio_module_process")
                add_tests("both")
            target_end()
        end
    end

    if use_experimental and has_config("faio_tests") then
        named_binary("module_experimental_contract", "faio_module_experimental",
                     "tests/modules/experimental_contract.cpp")
            add_tests("runtime")
        target_end()
        if use_reflection then
            local function async_test(name, source)
                named_binary(name, "faio_module_experimental", "examples/experimental/" .. source)
                    add_defines("FAIO_USE_MODULES=1")
                    add_tests("run")
                target_end()
            end
            async_test("module_experimental_async_main", "entry/async_main.cpp")
            async_test("module_experimental_exit_cleanup", "entry/exit_cleanup.cpp")
            async_test("module_experimental_annotated_task", "lifecycle/annotated_task.cpp")
            if use_module_execution then
                async_test("module_experimental_sender_bridge", "execution/sender_bridge.cpp")
            end
        end
    end
end

-- Platform settings and dependencies shared by the two faio xmake entry points.
faio_xmake = {}
local root = path.directory(path.directory(os.scriptdir()))

function faio_xmake.feature(name, default, description)
    option(name)
        set_default(default)
        set_showmenu(true)
        set_description(description)
    option_end()
end

faio_xmake.feature("faio_tls", false, "Enable the OpenSSL TLS component")
faio_xmake.feature("faio_process", false, "Enable the asynchronous process component")
faio_xmake.feature("faio_coroutine_frame_use_global_new", false, "Use global new/delete for task frames")
faio_xmake.feature("faio_module_experimental", false, "Enable the Linux GCC 16.1 experimental module")
faio_xmake.feature("faio_experimental_reflection", true, "Enable reflection, lifecycle and async entry")
faio_xmake.feature("faio_module_experimental_execution", false, "Enable locked stdexec support in the named module")

option("faio_io_uring")
    set_default("auto")
    set_values("auto", "y", "n")
    set_showmenu(true)
    set_description("Use Linux io_uring (auto enables it for Linux targets)")
option_end()

option("faio_allocator")
    set_default("auto")
    set_values("auto", "system", "tcmalloc")
    set_showmenu(true)
    set_description("Process allocator, selected at link time")
option_end()

option("faio_sanitizers")
    set_default("none")
    set_values("none", "address", "undefined", "address,undefined", "thread")
    set_showmenu(true)
    set_description("Sanitizers applied to both faio forms and their consumers")
option_end()

local function require_dependency(name, version, optional)
    local provider = is_plat("windows") and "vcpkg::" or "pkgconfig::"
    add_requires(provider .. name .. (version or ""), {alias = name, system = true, optional = optional})
end

local sanitizer = get_config("faio_sanitizers") or "none"
local allocator = get_config("faio_allocator") or "auto"
local uring_option = get_config("faio_io_uring")
local auto_uring = uring_option == nil or uring_option == "auto"
local use_uring = uring_option == true or uring_option == "y" or (auto_uring and is_plat("linux"))

function faio_xmake.require_dependencies()
    require_dependency("spdlog")
    if use_uring and is_plat("linux") then
        require_dependency("liburing")
    end
    if has_config("faio_tls") then
        require_dependency("openssl", " >=3.5")
    end
    if is_plat("linux") and sanitizer == "none" and allocator ~= "system" then
        require_dependency("libtcmalloc", nil, allocator == "auto")
    end
end

function faio_xmake.shared_headers()
    add_headerfiles(path.join(root, "include/(faio/**.hpp)"), path.join(root, "include/(faio/**.h)"),
                    path.join(root, "include/(faio/**.inc)"))
    add_includedirs(path.join(root, "include"), {public = true})
    if is_plat("windows", "mingw") then
        add_includedirs(path.join(root, "include/faio/detail/io/platform/windows/compat"), {public = true})
    end
end

function faio_xmake.configure_core()
    add_packages("spdlog", {public = true})
    add_defines(use_uring and "FAIO_HAS_IO_URING=1" or "FAIO_HAS_IO_URING=0", {public = true})
    if use_uring and is_plat("linux") then
        add_packages("liburing", {public = true})
    elseif use_uring then
        on_load(function ()
            raise("faio_io_uring requires a Linux target")
        end)
    end
    if has_config("faio_coroutine_frame_use_global_new") then
        add_defines("FAIO_COROUTINE_FRAME_USE_GLOBAL_NEW=1", {public = true})
    end
    if has_config("faio_tls") then
        add_packages("openssl", {public = true})
        add_defines("FAIO_ENABLE_TLS=1", {public = true})
    end
    if has_config("faio_process") then
        add_defines("FAIO_ENABLE_PROCESS=1", {public = true})
        faio_xmake.configure_process()
    end
    if is_plat("windows", "mingw") then
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN", "_WIN32_WINNT=0x0A00", {public = true})
        add_syslinks("ws2_32", "mswsock", "iphlpapi", {public = true})
    elseif is_plat("linux") then
        add_cxxflags("-pthread", {public = true})
        add_ldflags("-pthread", {public = true})
    end
    if sanitizer ~= "none" then
        add_cxxflags("-fsanitize=" .. sanitizer, "-fno-omit-frame-pointer", {force = true, public = true})
        add_ldflags("-fsanitize=" .. sanitizer, {force = true, public = true})
    end
    if is_plat("linux") and sanitizer == "none" and allocator ~= "system" then
        add_packages("libtcmalloc", {public = true})
        after_load(function (target)
            local package = target:pkg("libtcmalloc")
            if package then
                target:add("ldflags", "-Wl,--push-state,--no-as-needed,-ltcmalloc,--pop-state",
                           {force = true, public = true})
            end
        end)
    elseif allocator == "tcmalloc" then
        before_build(function ()
            raise("faio_allocator=tcmalloc requires Linux without sanitizers")
        end)
    end
end

function faio_xmake.configure_process()
    if is_plat("windows", "mingw") then
        add_syslinks("bcrypt", "advapi32", {public = true})
    elseif is_plat("linux") then
        add_defines("_GNU_SOURCE", {public = true})
    end
end

function faio_xmake.experimental_variant(use_execution)
    local use_reflection = has_config("faio_experimental_reflection")
    local source = os.getenv("FAIO_STDEXEC_SOURCE_DIR") or path.join(root, "../faio-deps/src/stdexec")
    set_languages("c++26")
    add_defines("FAIO_ENABLE_EXPERIMENTAL=1",
        "FAIO_EXPERIMENTAL_HAS_REFLECTION=" .. (use_reflection and "1" or "0"),
        "FAIO_EXPERIMENTAL_TASK_LIFECYCLE=" .. (use_reflection and "1" or "0"),
        "FAIO_EXPERIMENTAL_HAS_EXECUTION=" .. (use_execution and "1" or "0"), {public = true})
    if use_reflection then
        add_cxxflags("-freflection", {force = true, public = true})
    end
    if use_execution then
        add_sysincludedirs(path.join(source, "include"), {public = true})
    end
    on_config(function (target)
        if not target:is_plat("linux") then
            raise("faio experimental requires Linux and GCC 16.1 with matching libstdc++")
        end
        local compiler = target:tool("cxx")
        local version = os.iorunv(compiler, {"-dumpfullversion"}):trim()
        if not version:match("^16%.1%.") then
            raise("faio experimental requires Linux and GCC 16.1 with matching libstdc++")
        end
        if use_execution then
            local json = import("core.base.json")
            local lock = json.loadfile(path.join(root, "scripts", "experimental_dependencies.lock.json"))
            local head = os.iorunv("git", {"-C", source, "rev-parse", "HEAD"}):trim()
            local status = os.iorunv("git", {"-C", source, "status", "--porcelain", "--untracked-files=all"}):trim()
            if head ~= lock.stdexec.commit or #status ~= 0 then
                raise("FAIO_STDEXEC_SOURCE_DIR must be a clean checkout of the locked stdexec revision")
            end
        end
    end)
end

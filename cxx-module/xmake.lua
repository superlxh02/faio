set_xmakever("3.0.1")

if os.scriptdir() == os.projectdir() then
    set_project("faio-module")
    -- xmake resolves -o relative to the invoking directory when -P is used.
    set_config("builddir", "../target/xmake/cxx-module")
end
set_languages("c++23")
add_rules("mode.debug", "mode.release")
includes("../tools/xmake/common.lua")
faio_xmake.require_dependencies()

local use_experimental = has_config("faio_module_experimental")
local function named_library(name, source)
    target(name)
        set_kind("static")
        set_policy("build.c++.modules.std", false)
        set_policy("build.c++.modules.culling", false)
        add_files(source, {public = true})
        if use_experimental then
            faio_xmake.experimental_variant(has_config("faio_module_experimental_execution"))
        end
end

named_library("faio_module", "faio.cppm")
    add_files("duplex.cpp")
    faio_xmake.shared_headers()
    faio_xmake.configure_core()
target_end()

if has_config("faio_tls") then
    named_library("faio_module_tls", "extensions/tls.cppm")
        add_deps("faio_module", {public = true})
        add_packages("openssl", {public = true})
    target_end()
end
if has_config("faio_process") then
    named_library("faio_module_process", "extensions/process.cppm")
        add_deps("faio_module", {public = true})
        faio_xmake.configure_process()
    target_end()
end

if use_experimental then
    named_library("faio_module_experimental", "experimental/experimental.cppm")
        add_deps("faio_module", {public = true})
        add_includedirs("experimental/include", {public = true})
        add_headerfiles("experimental/include/(faio-module/*.hpp)")
    target_end()
end

#[[ @brief 仅打包头文件源码，不运行上游需要联网的 CMake 构建。 ]]
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../scripts/experimental_dependencies.lock.json" lock)
string(JSON commit GET "${lock}" stdexec commit)
if(NOT commit STREQUAL "6d7ad689f4d4831c5136e4abe1c601f9a3b64e43")
    message(FATAL_ERROR "Update the faio-stdexec archive hash when changing the lock")
endif()
vcpkg_from_github(
    OUT_SOURCE_PATH source
    REPO NVIDIA/stdexec
    REF "${commit}"
    SHA512 5d33df0a2b391de914cd51b2dab0bcd2c05e18473de5fe112d78372ebff7d56ba1b3fc5b8796df21f8e84ba3c855435e33b5240b99b5eb094baea1b6eb431e86
)
file(INSTALL "${source}/include" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${source}/LICENSE.txt" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/experimental_dependencies.lock.json" "${lock}")
vcpkg_install_copyright(FILE_LIST "${source}/LICENSE.txt")
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

#[[
@file cmake/FindTCMalloc.cmake
@brief 查找 gperftools 的 tcmalloc 库及公共头文件。
]]
find_path(TCMalloc_INCLUDE_DIR NAMES gperftools/tcmalloc.h)
find_library(TCMalloc_LIBRARY NAMES tcmalloc_minimal tcmalloc)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TCMalloc REQUIRED_VARS TCMalloc_LIBRARY TCMalloc_INCLUDE_DIR)

if(TCMalloc_FOUND AND NOT TARGET TCMalloc::TCMalloc)
    find_package(Threads REQUIRED)
    add_library(TCMalloc::TCMalloc UNKNOWN IMPORTED GLOBAL)
    set_target_properties(TCMalloc::TCMalloc PROPERTIES
        IMPORTED_LOCATION "${TCMalloc_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${TCMalloc_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
endif()
mark_as_advanced(TCMalloc_INCLUDE_DIR TCMalloc_LIBRARY)

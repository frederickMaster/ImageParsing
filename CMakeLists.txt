cmake_minimum_required(VERSION 3.20)
project(uife VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(UIFE_ENABLE_LIBTSK "Enable filesystem parsing through The Sleuth Kit (libtsk)" ON)
option(UIFE_ENABLE_LIBEWF "Enable EWF/E01 reading through libewf" ON)
option(UIFE_ENABLE_LIBWIM "Enable WIM/ESD browsing through wimlib" ON)
option(UIFE_ENABLE_LIBFSAPFS "Enable APFS parsing through libfsapfs" ON)
option(UIFE_ENABLE_LIBFSREFS "Enable ReFS browsing through libfsrefs" ON)
# zlib 用于 DMG/GHO 压缩块解码，属于核心能力，缺失时给出可操作的安装提示。
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    message(FATAL_ERROR
        "zlib not found. DMG/GHO decompression requires zlib.\n"
        "  Debian/Ubuntu: sudo apt-get install zlib1g-dev\n"
        "  macOS:         zlib is provided by the system SDK\n"
        "  Windows:       vcpkg install zlib:x64-windows, then configure with\n"
        "                 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
endif()
set(UIFE_HAS_LIBTSK OFF)
if(UIFE_ENABLE_LIBTSK)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBTSK QUIET IMPORTED_TARGET libtsk)
    endif()
    if(NOT TARGET PkgConfig::LIBTSK)
        find_path(LIBTSK_INCLUDE_DIR tsk/libtsk.h)
        find_library(LIBTSK_LIBRARY NAMES tsk libtsk)
    endif()
endif()

set(UIFE_HAS_LIBEWF OFF)
if(UIFE_ENABLE_LIBEWF)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig QUIET)
    endif()
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBEWF QUIET IMPORTED_TARGET libewf)
    endif()
    if(NOT TARGET PkgConfig::LIBEWF)
        find_path(LIBEWF_INCLUDE_DIR libewf.h)
        find_library(LIBEWF_LIBRARY NAMES ewf libewf)
    endif()
    if(TARGET PkgConfig::LIBEWF OR (LIBEWF_INCLUDE_DIR AND LIBEWF_LIBRARY))
        set(ZLIB_USE_STATIC_LIBS ON)
        set(BZip2_USE_STATIC_LIBS ON)
        find_package(ZLIB QUIET)
        find_package(BZip2 QUIET)
        set(UIFE_LIBEWF_DEPENDENCIES)
        if(TARGET ZLIB::ZLIB)
            list(APPEND UIFE_LIBEWF_DEPENDENCIES ZLIB::ZLIB)
        endif()
        if(TARGET BZip2::BZip2)
            list(APPEND UIFE_LIBEWF_DEPENDENCIES BZip2::BZip2)
        endif()

        include(CheckCXXSourceCompiles)
        if(TARGET PkgConfig::LIBEWF)
            set(CMAKE_REQUIRED_LIBRARIES
                PkgConfig::LIBEWF ${UIFE_LIBEWF_DEPENDENCIES})
        else()
            set(CMAKE_REQUIRED_INCLUDES "${LIBEWF_INCLUDE_DIR}")
            set(CMAKE_REQUIRED_LIBRARIES
                "${LIBEWF_LIBRARY}" ${UIFE_LIBEWF_DEPENDENCIES})
        endif()
        set(UIFE_LIBEWF_API_CHECK_SOURCE
            "#if defined(_WIN32)\n#include <windows.h>\n#endif\n#include <libewf.h>\nint main() { libewf_handle_t *handle = nullptr; libewf_error_t *error = nullptr; size64_t size = 0; uint32_t sector_size = 0;\n#if defined(_WIN32)\n#if !defined(LIBEWF_HAVE_WIDE_CHARACTER_TYPE)\n#error libewf wide-character APIs are required for Windows path support\n#endif\n(void)&libewf_glob_wide; (void)&libewf_glob_wide_free; (void)&libewf_handle_open_wide;\n#endif\nreturn libewf_set_codepage(0, &error) + libewf_glob(\"sample.E01\", 10, LIBEWF_FORMAT_UNKNOWN, nullptr, nullptr, &error) + libewf_handle_initialize(&handle, &error) + libewf_handle_open(handle, nullptr, 0, LIBEWF_ACCESS_FLAG_READ, &error) + libewf_handle_get_media_size(handle, &size, &error) + libewf_handle_get_bytes_per_sector(handle, &sector_size, &error) + static_cast<int>(libewf_handle_read_buffer_at_offset(handle, nullptr, 0, 0, &error)); }")
        check_cxx_source_compiles(
            "${UIFE_LIBEWF_API_CHECK_SOURCE}"
            UIFE_LIBEWF_HAS_REQUIRED_API)
        unset(UIFE_LIBEWF_API_CHECK_SOURCE)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(UIFE_LIBEWF_HAS_REQUIRED_API)
            set(UIFE_HAS_LIBEWF ON)
        else()
            message(WARNING "libewf was found but does not expose all APIs required by UIFE; EWF remains detection-only")
        endif()
    endif()
endif()

set(UIFE_HAS_LIBWIM OFF)
if(UIFE_ENABLE_LIBWIM)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig QUIET)
    endif()
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBWIM QUIET IMPORTED_TARGET wimlib)
    endif()
    if(NOT TARGET PkgConfig::LIBWIM)
        find_path(LIBWIM_INCLUDE_DIR wimlib.h
            HINTS "${UIFE_LIBWIM_ROOT}/devel" "${UIFE_LIBWIM_ROOT}/include")
        find_library(LIBWIM_LIBRARY
            NAMES wim libwim libwim-15
            HINTS "${UIFE_LIBWIM_ROOT}/devel" "${UIFE_LIBWIM_ROOT}/lib")
    endif()
    if(TARGET PkgConfig::LIBWIM OR (LIBWIM_INCLUDE_DIR AND LIBWIM_LIBRARY))
        include(CheckCXXSourceCompiles)
        if(TARGET PkgConfig::LIBWIM)
            set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBWIM)
        else()
            set(CMAKE_REQUIRED_INCLUDES "${LIBWIM_INCLUDE_DIR}")
            set(CMAKE_REQUIRED_LIBRARIES "${LIBWIM_LIBRARY}")
        endif()
        check_cxx_source_compiles(
            "#include <wimlib.h>\nstatic int callback(const wimlib_dir_entry *, void *) { return 0; }\nint main() { WIMStruct *wim = nullptr; wimlib_wim_info info{}; const wimlib_tchar *path = WIMLIB_WIM_ROOT_PATH; const wimlib_tchar *const *paths = &path; (void)wimlib_global_init(0); (void)wimlib_open_wim(path, 0, &wim); (void)wimlib_get_wim_info(wim, &info); (void)wimlib_reference_resource_files(wim, paths, 0, 0, 0); (void)wimlib_split(wim, path, 1, 0); (void)wimlib_iterate_dir_tree(wim, 1, path, WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE, callback, nullptr); (void)wimlib_extract_paths(wim, 1, path, paths, 1, WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES | WIMLIB_EXTRACT_FLAG_NO_PRESERVE_DIR_STRUCTURE); wimlib_free(wim); return 0; }"
            UIFE_LIBWIM_HAS_SPLIT_API)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(UIFE_LIBWIM_HAS_SPLIT_API)
            set(UIFE_HAS_LIBWIM ON)
        else()
            message(WARNING "wimlib was found but does not expose all APIs required by UIFE; WIM/ESD remains detection-only")
        endif()
    endif()
endif()

set(UIFE_HAS_LIBFSAPFS OFF)
if(UIFE_ENABLE_LIBFSAPFS)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig QUIET)
    endif()
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBFSAPFS QUIET IMPORTED_TARGET libfsapfs)
    endif()
    if(NOT TARGET PkgConfig::LIBFSAPFS)
        find_path(LIBFSAPFS_INCLUDE_DIR libfsapfs.h
            HINTS "${UIFE_LIBFSAPFS_ROOT}/include" "${UIFE_LIBFSAPFS_ROOT}")
        find_library(LIBFSAPFS_LIBRARY
            NAMES fsapfs libfsapfs
            HINTS "${UIFE_LIBFSAPFS_ROOT}/lib" "${UIFE_LIBFSAPFS_ROOT}")
    endif()
    if(TARGET PkgConfig::LIBFSAPFS OR (LIBFSAPFS_INCLUDE_DIR AND LIBFSAPFS_LIBRARY))
        include(CheckCXXSourceCompiles)
        # libfsapfs 的静态库需要 zlib（压缩数据流解压），.pc 里只写在 Libs.private，
        # 直接链接时必须自己补上，否则探测会因 uncompress 未定义而失败。
        if(TARGET PkgConfig::LIBFSAPFS)
            set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBFSAPFS ZLIB::ZLIB)
        else()
            set(CMAKE_REQUIRED_INCLUDES "${LIBFSAPFS_INCLUDE_DIR}")
            set(CMAKE_REQUIRED_LIBRARIES "${LIBFSAPFS_LIBRARY}" ZLIB::ZLIB)
        endif()
        # libyal 把 libbfio 静态打包进 libfsapfs，却不安装它的头文件；UIFE 在
        # ApfsFsParser.cpp 里自己声明这两个入口。这里连 libbfio 符号一起做链接探测，
        # 符号缺失就让 APFS 退回“仅识别”，避免编译期才炸。
        check_cxx_source_compiles(
"#include <libfsapfs.h>
extern \"C\" {
int libbfio_handle_initialize(void *, intptr_t *, void *, void *, void *, void *, void *, void *, void *, void *, void *, void *, unsigned char, void *);
int libbfio_handle_free(void *, void *);
}
int main() {
    libfsapfs_container_t *container = nullptr;
    libfsapfs_volume_t *volume = nullptr;
    libfsapfs_file_entry_t *entry = nullptr;
    libfsapfs_extended_attribute_t *attribute = nullptr;
    libfsapfs_error_t *error = nullptr;
    size64_t size = 0;
    size_t name_size = 0;
    int count = 0;
    uint64_t identifier = 0;
    if (libfsapfs_container_initialize(&container, &error) != 1) { return 1; }
    if (libfsapfs_container_get_number_of_volumes(container, &count, &error) != 1) { return 2; }
    if (libfsapfs_container_get_volume_by_index(container, 0, &volume, &error) != 1) { return 3; }
    if (libfsapfs_volume_is_locked(volume, &error) < 0) { return 4; }
    if (libfsapfs_volume_get_root_directory(volume, &entry, &error) != 1) { return 5; }
    if (libfsapfs_file_entry_get_number_of_sub_file_entries(entry, &count, &error) != 1) { return 6; }
    if (libfsapfs_file_entry_get_utf8_name_size(entry, &name_size, &error) != 1) { return 7; }
    if (libfsapfs_file_entry_get_identifier(entry, &identifier, &error) != 1) { return 8; }
    if (libfsapfs_file_entry_get_size(entry, &size, &error) != 1) { return 9; }
    if (libfsapfs_file_entry_get_number_of_extended_attributes(entry, &count, &error) != 1) { return 10; }
    if (libfsapfs_file_entry_get_extended_attribute_by_index(entry, 0, &attribute, &error) != 1) { return 11; }
    if (libfsapfs_extended_attribute_read_buffer_at_offset(attribute, nullptr, 0, 0, &error) < 0) { return 12; }
    if (libfsapfs_file_entry_read_buffer_at_offset(entry, nullptr, 0, 0, &error) < 0) { return 13; }
    (void) &libbfio_handle_initialize;
    (void) &libbfio_handle_free;
    return 0;
}"
            UIFE_LIBFSAPFS_HAS_REQUIRED_API)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(UIFE_LIBFSAPFS_HAS_REQUIRED_API)
            set(UIFE_HAS_LIBFSAPFS ON)
        else()
            message(WARNING "libfsapfs was found but does not expose all APIs required by UIFE; APFS remains detection-only")
        endif()
    endif()
endif()

set(UIFE_HAS_LIBFSREFS OFF)
if(UIFE_ENABLE_LIBFSREFS)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig QUIET)
    endif()
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBFSREFS QUIET IMPORTED_TARGET libfsrefs)
    endif()
    if(NOT TARGET PkgConfig::LIBFSREFS)
        find_path(LIBFSREFS_INCLUDE_DIR libfsrefs.h
            HINTS "${UIFE_LIBFSREFS_ROOT}/include" "${UIFE_LIBFSREFS_ROOT}")
        find_library(LIBFSREFS_LIBRARY
            NAMES fsrefs libfsrefs
            HINTS "${UIFE_LIBFSREFS_ROOT}/lib" "${UIFE_LIBFSREFS_ROOT}")
    endif()
    if(TARGET PkgConfig::LIBFSREFS OR (LIBFSREFS_INCLUDE_DIR AND LIBFSREFS_LIBRARY))
        include(CheckCXXSourceCompiles)
        if(TARGET PkgConfig::LIBFSREFS)
            set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBFSREFS)
        else()
            set(CMAKE_REQUIRED_INCLUDES "${LIBFSREFS_INCLUDE_DIR}")
            set(CMAKE_REQUIRED_LIBRARIES "${LIBFSREFS_LIBRARY}")
        endif()
        # 同 libfsapfs：捆绑的 libbfio 由 UIFE 自行声明，这里连符号一起探测。
        check_cxx_source_compiles(
"#include <libfsrefs.h>
extern \"C\" {
int libbfio_handle_initialize(void *, intptr_t *, void *, void *, void *, void *, void *, void *, void *, void *, void *, void *, unsigned char, void *);
int libbfio_handle_free(void *, void *);
int libfsrefs_volume_open_file_io_handle(libfsrefs_volume_t *, void *, int, libfsrefs_error_t **);
}
int main() {
    libfsrefs_volume_t *volume = nullptr;
    libfsrefs_file_entry_t *entry = nullptr;
    libfsrefs_error_t *error = nullptr;
    int count = 0;
    uint32_t flags = 0;
    if (libfsrefs_volume_initialize(&volume, &error) != 1) { return 1; }
    if (libfsrefs_volume_get_root_directory(volume, &entry, &error) != 1) { return 2; }
    if (libfsrefs_file_entry_get_number_of_sub_file_entries(entry, &count, &error) != 1) { return 3; }
    if (libfsrefs_file_entry_get_sub_file_entry_by_index(entry, 0, &entry, &error) != 1) { return 4; }
    if (libfsrefs_file_entry_get_file_attribute_flags(entry, &flags, &error) != 1) { return 5; }
    (void) &libbfio_handle_initialize;
    (void) &libbfio_handle_free;
    (void) &libfsrefs_volume_open_file_io_handle;
    return 0;
}"
            UIFE_LIBFSREFS_HAS_REQUIRED_API)
        unset(CMAKE_REQUIRED_INCLUDES)
        unset(CMAKE_REQUIRED_LIBRARIES)
        if(UIFE_LIBFSREFS_HAS_REQUIRED_API)
            set(UIFE_HAS_LIBFSREFS ON)
        else()
            message(WARNING "libfsrefs was found but does not expose all APIs required by UIFE; ReFS remains detection-only")
        endif()
    endif()
endif()

add_library(uife_core
    src/drivers/fs/ApfsFsParser.cpp
    src/drivers/fs/BtrfsSendFsParser.cpp
    src/DriverFactory.cpp
    src/Extractor.cpp
    src/FsParserFactory.cpp
    src/PartitionScanner.cpp
    src/drivers/image/DmgImageDriver.cpp
    src/drivers/fs/Iso9660.cpp
    src/drivers/fs/Iso9660FsParser.cpp
    src/drivers/fs/NtfsSecurity.cpp
    src/drivers/fs/RefsFsParser.cpp
    src/drivers/fs/TskCliFsParser.cpp
    src/drivers/fs/TskFsParser.cpp
    src/drivers/fs/WimFsParser.cpp
    src/drivers/image/CueBinImageDriver.cpp
    src/drivers/image/EwfImageDriver.cpp
    src/drivers/image/GhoImageDriver.cpp
    src/drivers/image/Qcow2ImageDriver.cpp
    src/drivers/image/QedImageDriver.cpp
    src/drivers/image/RawImageDriver.cpp
    src/drivers/image/VdiImageDriver.cpp
    src/drivers/image/VhdxImageDriver.cpp
    src/drivers/image/VmdkSparseImageDriver.cpp
    src/utils/Sha256.cpp
    src/vfs/VFS.cpp
)
target_include_directories(uife_core PUBLIC include)
target_compile_features(uife_core PUBLIC cxx_std_20)
target_compile_definitions(uife_core PRIVATE UIFE_WITH_ZLIB=1)

if(UIFE_HAS_LIBEWF)
    if(TARGET PkgConfig::LIBEWF)
        target_link_libraries(uife_core PRIVATE PkgConfig::LIBEWF)
    else()
        target_include_directories(uife_core PRIVATE "${LIBEWF_INCLUDE_DIR}")
        target_link_libraries(uife_core PRIVATE "${LIBEWF_LIBRARY}")
    endif()
    target_link_libraries(uife_core PRIVATE ${UIFE_LIBEWF_DEPENDENCIES})
    target_compile_definitions(uife_core PRIVATE UIFE_WITH_LIBEWF=1)
endif()

if(UIFE_HAS_LIBWIM)
    if(TARGET PkgConfig::LIBWIM)
        target_link_libraries(uife_core PRIVATE PkgConfig::LIBWIM)
    else()
        target_include_directories(uife_core PRIVATE "${LIBWIM_INCLUDE_DIR}")
        target_link_libraries(uife_core PRIVATE "${LIBWIM_LIBRARY}")
    endif()
    target_compile_definitions(uife_core PRIVATE UIFE_WITH_LIBWIM=1)
    if(WIN32 AND UIFE_LIBWIM_ROOT AND NOT UIFE_LIBWIM_RUNTIME)
        find_file(UIFE_LIBWIM_RUNTIME
            NAMES libwim-15.dll libwim.dll
            PATHS "${UIFE_LIBWIM_ROOT}"
            NO_DEFAULT_PATH)
    endif()
endif()

if(UIFE_HAS_LIBFSAPFS)
    if(TARGET PkgConfig::LIBFSAPFS)
        target_link_libraries(uife_core PRIVATE PkgConfig::LIBFSAPFS)
    else()
        target_include_directories(uife_core PRIVATE "${LIBFSAPFS_INCLUDE_DIR}")
        target_link_libraries(uife_core PRIVATE "${LIBFSAPFS_LIBRARY}")
    endif()
    target_compile_definitions(uife_core PRIVATE UIFE_WITH_LIBFSAPFS=1)
endif()

if(UIFE_HAS_LIBFSREFS)
    if(TARGET PkgConfig::LIBFSREFS)
        target_link_libraries(uife_core PRIVATE PkgConfig::LIBFSREFS)
    else()
        target_include_directories(uife_core PRIVATE "${LIBFSREFS_INCLUDE_DIR}")
        target_link_libraries(uife_core PRIVATE "${LIBFSREFS_LIBRARY}")
    endif()
    target_compile_definitions(uife_core PRIVATE UIFE_WITH_LIBFSREFS=1)
endif()

if(TARGET PkgConfig::LIBTSK)
    target_link_libraries(uife_core PRIVATE PkgConfig::LIBTSK)
    target_compile_definitions(uife_core PRIVATE UIFE_WITH_LIBTSK=1)
    set(UIFE_HAS_LIBTSK ON)
elseif(LIBTSK_INCLUDE_DIR AND LIBTSK_LIBRARY)
    target_include_directories(uife_core PRIVATE "${LIBTSK_INCLUDE_DIR}")
    target_link_libraries(uife_core PRIVATE "${LIBTSK_LIBRARY}")
    target_compile_definitions(uife_core PRIVATE UIFE_WITH_LIBTSK=1)
    set(UIFE_HAS_LIBTSK ON)
else()
    message(STATUS "libtsk development files not found; Windows builds may use Sleuth Kit CLI tools")
endif()

target_link_libraries(uife_core PRIVATE ZLIB::ZLIB)

if(UIFE_HAS_LIBTSK)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES ${LIBTSK_INCLUDE_DIRS} ${LIBTSK_INCLUDE_DIR})
    if(TARGET PkgConfig::LIBTSK)
        set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBTSK)
    else()
        set(CMAKE_REQUIRED_LIBRARIES "${LIBTSK_LIBRARY}")
    endif()
    check_cxx_source_compiles(
        "#include <tsk/libtsk.h>\nint main() { return static_cast<int>(TSK_FS_TYPE_XFS); }"
        UIFE_TSK_HAS_XFS)
    check_cxx_source_compiles(
        "#include <tsk/libtsk.h>\n#include <tsk/pool/tsk_pool.h>\nint main() { TSK_IMG_INFO *img = nullptr; const TSK_POOL_INFO *pool = nullptr; (void)tsk_pool_open_img_sing(img, 0, TSK_POOL_TYPE_APFS); (void)tsk_fs_open_pool_decrypt(pool, 0, TSK_FS_TYPE_APFS, \"password\"); return 0; }"
        UIFE_TSK_HAS_APFS_POOL_DECRYPT)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(UIFE_TSK_HAS_XFS)
        target_compile_definitions(uife_core PRIVATE UIFE_WITH_TSK_XFS=1)
    endif()
    if(UIFE_TSK_HAS_APFS_POOL_DECRYPT)
        target_compile_definitions(uife_core PRIVATE UIFE_WITH_TSK_APFS=1)
    endif()
endif()

add_executable(uife src/cli/main.cpp)
target_link_libraries(uife PRIVATE uife_core)
if(UIFE_HAS_LIBWIM AND WIN32 AND UIFE_LIBWIM_RUNTIME)
    add_custom_command(TARGET uife POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${UIFE_LIBWIM_RUNTIME}" "$<TARGET_FILE_DIR:uife>")
endif()
if(MINGW)
    target_link_options(uife PRIVATE
        -municode
        -static
        -static-libgcc
        -static-libstdc++)
endif()

if(MSVC)
    target_compile_options(uife_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(uife PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(uife_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    target_compile_options(uife PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(BUILD_TESTING)
    add_executable(uife_tests tests/uife_tests.cpp)
    target_link_libraries(uife_tests PRIVATE uife_core)
    if(UIFE_HAS_LIBTSK)
        target_compile_definitions(uife_tests PRIVATE UIFE_WITH_LIBTSK=1)
    endif()
    if(UIFE_HAS_LIBEWF)
        target_compile_definitions(uife_tests PRIVATE UIFE_WITH_LIBEWF=1)
    endif()
    if(UIFE_HAS_LIBWIM)
        target_compile_definitions(uife_tests PRIVATE UIFE_WITH_LIBWIM=1)
    endif()
    if(UIFE_HAS_LIBFSAPFS)
        target_compile_definitions(uife_tests PRIVATE UIFE_WITH_LIBFSAPFS=1)
    endif()
    if(UIFE_HAS_LIBFSREFS)
        target_compile_definitions(uife_tests PRIVATE UIFE_WITH_LIBFSREFS=1)
    endif()
    if(UIFE_TSK_HAS_XFS)
        target_compile_definitions(uife_tests PRIVATE UIFE_WITH_TSK_XFS=1)
    endif()
    if(UIFE_TSK_HAS_APFS_POOL_DECRYPT)
        target_compile_definitions(uife_tests PRIVATE UIFE_WITH_TSK_APFS=1)
    endif()
    if(MSVC)
        target_compile_options(uife_tests PRIVATE /utf-8)
    endif()
    add_test(NAME uife_core_tests COMMAND uife_tests)
    add_test(NAME uife_help COMMAND uife --help)
    set_tests_properties(uife_help PROPERTIES
        PASS_REGULAR_EXPRESSION "当前可读取的镜像格式")
    add_test(NAME uife_cli_argument_validation
        COMMAND ${CMAKE_COMMAND}
            "-DUIFE_EXECUTABLE=$<TARGET_FILE:uife>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_argument_tests.cmake")
    add_executable(uife_integration_tests tests/uife_integration_tests.cpp)
    target_link_libraries(uife_integration_tests PRIVATE uife_core)
    target_link_libraries(uife_integration_tests PRIVATE ZLIB::ZLIB)
    target_compile_definitions(uife_integration_tests PRIVATE UIFE_WITH_ZLIB=1)
    if(UIFE_HAS_LIBWIM)
        target_compile_definitions(uife_integration_tests PRIVATE UIFE_WITH_LIBWIM=1)
        if(TARGET PkgConfig::LIBWIM)
            target_link_libraries(uife_integration_tests PRIVATE PkgConfig::LIBWIM)
        else()
            target_include_directories(uife_integration_tests PRIVATE
                "${LIBWIM_INCLUDE_DIR}")
            target_link_libraries(uife_integration_tests PRIVATE
                "${LIBWIM_LIBRARY}")
        endif()
    endif()
    if(MSVC)
        target_compile_options(uife_integration_tests PRIVATE /utf-8)
    endif()
    if(UIFE_HAS_LIBFSAPFS)
        target_compile_definitions(uife_integration_tests PRIVATE UIFE_WITH_LIBFSAPFS=1)
    endif()
    if(UIFE_HAS_LIBFSREFS)
        target_compile_definitions(uife_integration_tests PRIVATE UIFE_WITH_LIBFSREFS=1)
    endif()
    if(UIFE_HAS_LIBWIM AND WIN32 AND UIFE_LIBWIM_RUNTIME)
        add_custom_command(TARGET uife_integration_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${UIFE_LIBWIM_RUNTIME}" "$<TARGET_FILE_DIR:uife_integration_tests>")
    endif()
    add_test(NAME uife_integration_tests COMMAND uife_integration_tests)

    # 可选：真实样本兼容性矩阵。样本不进入版本库，只有在显式设置
    # UIFE_CORPUS_DIR 时才注册该用例（约定见 tests/corpus/README.md）。
    if(UIFE_CORPUS_DIR)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(NOT Python3_Interpreter_FOUND)
            message(WARNING
                "UIFE_CORPUS_DIR is set but Python 3 was not found; the compatibility matrix test is disabled")
        else()
            add_test(NAME uife_compat_matrix
                COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/compat_matrix.py"
                    --exe "$<TARGET_FILE:uife>"
                    --corpus "${UIFE_CORPUS_DIR}"
                    --report "${CMAKE_CURRENT_BINARY_DIR}/compat-matrix.md"
                    --json "${CMAKE_CURRENT_BINARY_DIR}/compat-matrix.json")
            set_tests_properties(uife_compat_matrix PROPERTIES
                TIMEOUT 7200
                LABELS "compat")
        endif()
    endif()
endif()

install(TARGETS uife RUNTIME DESTINATION bin)

# 后端启用情况既打进配置日志，也落成文件，方便 CI 断言“装了依赖就真的接上了后端”，
# 而不是在被跳过的分支上显示绿色。
set(UIFE_BACKEND_REPORT
    "libtsk=${UIFE_HAS_LIBTSK} libewf=${UIFE_HAS_LIBEWF} libwim=${UIFE_HAS_LIBWIM} libfsapfs=${UIFE_HAS_LIBFSAPFS} libfsrefs=${UIFE_HAS_LIBFSREFS}")
message(STATUS "UIFE backends: ${UIFE_BACKEND_REPORT}")
file(WRITE "${CMAKE_BINARY_DIR}/uife-backends.txt" "${UIFE_BACKEND_REPORT}\n")

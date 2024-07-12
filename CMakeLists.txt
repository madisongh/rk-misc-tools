# SPDX-License-Identifier: MIT
# Copyright (c) 2024, Matthew Madison

cmake_minimum_required(VERSION 3.20)

project(rk_misc_tools LANGUAGES C VERSION 0.2.1)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)

# Settings for rk-bootinfo
set(LKDIRGROUP "root" CACHE STRING "Group owner for lock directory")
set(STORAGE_DEV "/dev/mmcblk0boot1" CACHE STRING "Device for variable storage")
set(STORAGE_OFFSET "0x80000" CACHE STRING "Offset to start of variable storage")
set(TARGET "RK3588" CACHE STRING "Target SoC model")

string(SUBSTRING "${TARGET}" 2 -1 TARGET_STRIPPED)
option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

find_package(PkgConfig REQUIRED)
# for rkvendor-tool
pkg_check_modules(LIBEDIT REQUIRED IMPORTED_TARGET libedit)
find_path(RK_UAPI_MISCDIR rkflash_vendor_storage.h PATH_SUFFIXES rockchip-uapi/misc misc REQUIRED)
cmake_path(GET RK_UAPI_MISCDIR PARENT_PATH RK_UAPI_INCDIR)
# for rk-bootinfo
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
pkg_get_variable(TMPFILESDIR systemd tmpfilesdir)

add_executable(rkvendor-tool rkvendor-tool.c)
target_include_directories(rkvendor-tool PUBLIC ${RK_UAPI_INCDIR})
target_link_libraries(rkvendor-tool PUBLIC PkgConfig::LIBEDIT)

add_executable(rk-otp-tool rk-otp-tool.c)
target_link_libraries(rk-otp-tool PUBLIC teec)

configure_file(config-files/rk-bootinfo.conf.in rk-bootinfo.conf @ONLY)
configure_file(librkbootinfo.pc.in librkbootinfo.pc @ONLY)
add_library(rkbootinfo SHARED bootinfo.c bootinfo.h util.c util.h)
set_target_properties(rkbootinfo PROPERTIES
    VERSION 1.0.0
    SOVERSION 1)
target_compile_definitions(rkbootinfo PUBLIC
  BOOTINFO_STORAGE_DEVICE="${STORAGE_DEV}"
  BOOTINFO_STORAGE_OFFSET_A=${STORAGE_OFFSET})
target_link_libraries(rkbootinfo PUBLIC PkgConfig::ZLIB)
add_executable(rk-bootinfo rk-bootinfo.c)
target_compile_definitions(rk-bootinfo PUBLIC
  VERSION="${PROJECT_VERSION}")
target_link_libraries(rk-bootinfo PUBLIC rkbootinfo)

add_executable(rk-update-bootloader rk-update-bootloader.c)
target_compile_definitions(rk-update-bootloader PUBLIC
        VERSION="${PROJECT_VERSION}"
        TARGET=${TARGET_STRIPPED}
)
install(TARGETS rkbootinfo LIBRARY)
install(FILES bootinfo.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/rkbootinfo")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/librkbootinfo.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(TARGETS rkvendor-tool rk-otp-tool rk-bootinfo rk-update-bootloader RUNTIME)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rk-bootinfo.conf DESTINATION ${TMPFILESDIR})

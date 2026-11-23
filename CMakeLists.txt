cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(picocalc_file_manager C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

set(FM_VERSION "0.0.0" CACHE STRING "Application version (x.y.z)")
set(FM_GITHUB_REPO "kicomoco/UF2PicoCalcFileManager" CACHE STRING "GitHub owner/repo used for update checks")

add_executable(picocalc_file_manager
    src/main.c
    src/lcd.c
    src/kbd.c
    src/sdcard.c
    src/ui.c
    src/panel.c
    src/fileops.c
    src/editor.c
    src/hexedit.c
    src/config.c
    src/net.c
    src/updater.c
    src/util.c
    lib/fatfs/ff.c
    lib/fatfs/ffsystem.c
    lib/fatfs/ffunicode.c
)

target_include_directories(picocalc_file_manager PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/lib/fatfs
)

target_compile_definitions(picocalc_file_manager PRIVATE
    FM_VERSION="${FM_VERSION}"
    FM_GITHUB_REPO="${FM_GITHUB_REPO}"
    ALTCP_MBEDTLS_AUTHMODE=MBEDTLS_SSL_VERIFY_REQUIRED
    CYW43_LWIP=1
    _GNU_SOURCE
)

target_compile_options(picocalc_file_manager PRIVATE -Wall -Wno-unused-function -Wno-format-truncation)

set_source_files_properties(
    lib/fatfs/ff.c lib/fatfs/ffsystem.c lib/fatfs/ffunicode.c
    PROPERTIES COMPILE_OPTIONS "-w"
)
set_source_files_properties(
    ${PICO_LWIP_PATH}/src/apps/altcp_tls/altcp_tls_mbedtls.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-result"
)

target_link_libraries(picocalc_file_manager
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_watchdog
    pico_multicore
    pico_cyw43_arch_lwip_poll
    pico_lwip_mbedtls
    pico_mbedtls
)

pico_set_program_name(picocalc_file_manager "PicoCalc File Manager")
pico_set_program_version(picocalc_file_manager "${FM_VERSION}")
pico_set_program_url(picocalc_file_manager "https://github.com/${FM_GITHUB_REPO}")

pico_enable_stdio_usb(picocalc_file_manager 0)
pico_enable_stdio_uart(picocalc_file_manager 0)

pico_add_extra_outputs(picocalc_file_manager)

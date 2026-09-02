cmake_minimum_required(VERSION 3.16)

get_property(SDK_LIBRARIES GLOBAL PROPERTY SDK_LIBRARIES)
get_property(SDK_STAGING_DIR GLOBAL PROPERTY SDK_STAGING_DIR)
set(SDK_LIB_DIR ${SDK_STAGING_DIR}/lib)
set(SDK_INCLUDE_DIR ${SDK_STAGING_DIR}/include)

set(APP_COMPILE_FLAGS
    -O2
    -fPIC
    -flto
    -fdata-sections
    -ffunction-sections
    -fno-builtin
    -fno-builtin-function
    -fno-jump-tables
    -fno-tree-switch-conversion
    -fstrict-volatile-bitfields
    -fvisibility=hidden
    -g3
    -mabi=ilp32f
    -march=rv32imafc_zicsr_zifencei
    -nostartfiles
    -nostdlib
    -shared
)

set(APP_LINK_FLAGS
    -Wl,--strip-debug
    -Wl,--gc-sections
    -e main
)

set(APP_ELF_DIR ${CMAKE_BINARY_DIR}/app_elfs)
file(MAKE_DIRECTORY ${APP_ELF_DIR})

function(build_app app_name)
    cmake_parse_arguments(APP "PREINSTALL" "" "SOURCES;LIBRARIES;COMPILE_DEFINITIONS" ${ARGN})

    if(NOT APP_SOURCES)
        message(FATAL_ERROR "build_app: SOURCES must be specified for ${app_name}")
    endif()

    set(APP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${app_name})

    # Process sources
    set(ABSOLUTE_SOURCES "")
    foreach(SOURCE ${APP_SOURCES})
        if(IS_ABSOLUTE ${SOURCE})
            list(APPEND ABSOLUTE_SOURCES ${SOURCE})
        else()
            list(APPEND ABSOLUTE_SOURCES ${APP_SOURCE_DIR}/${SOURCE})
        endif()
    endforeach()

    set(LIBRARY_FLAGS "")
    if(APP_LIBRARIES)
        list(APPEND LIBRARY_FLAGS -L${SDK_LIB_DIR})
        foreach(LIB ${APP_LIBRARIES})
            list(APPEND LIBRARY_FLAGS -l${LIB})
            list(APPEND LIBRARY_FLAGS -Wl,--exclude-libs,lib${LIB}.a)
        endforeach()
    endif()

    set(DEFINE_FLAGS "")
    if(APP_COMPILE_DEFINITIONS)
        foreach(DEF ${APP_COMPILE_DEFINITIONS})
            list(APPEND DEFINE_FLAGS -D${DEF})
        endforeach()
    endif()

    add_custom_command(
        OUTPUT ${APP_ELF_DIR}/${app_name}.elf
        COMMAND ${CMAKE_C_COMPILER}
            ${APP_COMPILE_FLAGS}
            ${APP_LINK_FLAGS}
            -isystem${SDK_INCLUDE_DIR}
            -o ${APP_ELF_DIR}/${app_name}.elf
            -I ${APP_SOURCE_DIR}
            ${ABSOLUTE_SOURCES}
            ${LIBRARY_FLAGS}
            ${DEFINE_FLAGS}
        DEPENDS
            ${ABSOLUTE_SOURCES}
            final_sdk_staging
        COMMENT "Building app ELF: ${app_name}"
        VERBATIM
    )
    add_custom_target(build_app_${app_name} ALL
        DEPENDS
            ${APP_ELF_DIR}/${app_name}.elf
    )

    if(APP_PREINSTALL)
        get_property(STORAGE_STAGING_DIR GLOBAL PROPERTY STORAGE_STAGING_DIR)
        set(STAGING_APPS_DIR ${STORAGE_STAGING_DIR}/BADGEVMS/APPS)

        # Does this app have a storage_skel directory?
        if(EXISTS ${APP_SOURCE_DIR}/storage_skel)
            add_custom_target(storage_staging_add_app_${app_name} ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGING_APPS_DIR}
                # manifest -> apps json
                COMMAND ${CMAKE_COMMAND} -E copy
                        ${APP_SOURCE_DIR}/manifest.json
                        ${STAGING_APPS_DIR}/${app_name}.json
                # copy the skeleton if present
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        ${APP_SOURCE_DIR}/storage_skel
                        ${STAGING_APPS_DIR}/${app_name}
                # make sure dest exists (in case skeleton didn’t create it)
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        ${STAGING_APPS_DIR}/${app_name}
                # copy elf and strip
                COMMAND ${CMAKE_COMMAND} -E copy
                        ${APP_ELF_DIR}/${app_name}.elf
                        ${STAGING_APPS_DIR}/${app_name}/${app_name}.elf
                COMMAND ${CMAKE_STRIP}
                        ${STAGING_APPS_DIR}/${app_name}/${app_name}.elf
                DEPENDS
                    build_app_${app_name}
                    init_storage_staging
                    ${APP_ELF_DIR}/${app_name}.elf
                BYPRODUCTS
                    ${STAGING_APPS_DIR}/${app_name}.json
                    ${STAGING_APPS_DIR}/${app_name}
                COMMENT "Copying ${app_name} manifest, skel (present) and stripped elf to storage staging"
                VERBATIM
            )
        else()
            # No storage_skel: just create the app folder and drop the elf + manifest
            add_custom_target(storage_staging_add_app_${app_name} ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGING_APPS_DIR}
                COMMAND ${CMAKE_COMMAND} -E copy
                        ${APP_SOURCE_DIR}/manifest.json
                        ${STAGING_APPS_DIR}/${app_name}.json
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        ${STAGING_APPS_DIR}/${app_name}
                COMMAND ${CMAKE_COMMAND} -E copy
                        ${APP_ELF_DIR}/${app_name}.elf
                        ${STAGING_APPS_DIR}/${app_name}/${app_name}.elf
                COMMAND ${CMAKE_STRIP}
                        ${STAGING_APPS_DIR}/${app_name}/${app_name}.elf
                DEPENDS
                    build_app_${app_name}
                    init_storage_staging
                    ${APP_ELF_DIR}/${app_name}.elf
                BYPRODUCTS
                    ${STAGING_APPS_DIR}/${app_name}.json
                    ${STAGING_APPS_DIR}/${app_name}
                COMMENT "Copying ${app_name} manifest and stripped elf to storage staging (no storage_skel)"
                VERBATIM
            )
        endif()

        add_dependencies(final_storage_staging storage_staging_add_app_${app_name})
    endif()

    set_property(GLOBAL APPEND PROPERTY SDK_APP_ELFS ${APP_ELF_DIR}/${app_name}.elf)
    set_property(GLOBAL APPEND PROPERTY SDK_APP_BUILD_TARGETS build_app_${app_name})
endfunction()

#
# Core apps
#

# Old OTA updater preinstalled at handout, doesn't build anymore due to api changes
#build_app(ota_wifi_update
#    SOURCES
#     ota_wifi_update.c
#     thirdparty/microui.c
#)


# MINIBROWSER
build_app(mini_browser PREINSTALL
    SOURCES
     mini_browser.c
    LIBRARIES
     sdl3
)


# Current updater app
build_app(why2025_ota PREINSTALL
    SOURCES
     main.c
     window.c
     ota_update.c
     thirdparty/cJSON.c
    LIBRARIES
     sdl3
)

# my first test app
build_app(sdl_serial_demo PREINSTALL
    SOURCES
     sdl_serial_demo.c
    LIBRARIES
     sdl3
)

build_app(badgevms_launcher PREINSTALL
    SOURCES
     main.c
)

build_app(badgevms_settings PREINSTALL
    SOURCES
     main.c
    LIBRARIES
     sdl3
)

#
# Regular apps
#


build_app(why2025_sponsors PREINSTALL
    SOURCES
     main.c
     image.c
)

build_app(why2025_namebadge PREINSTALL
    SOURCES
     main.c
     image.c
)

#
# Diagnostic apps
#

build_app(hardware_test PREINSTALL
    SOURCES
     thirdparty/microui.c
     test_badge.c
     test_drawing_helper.c
     test_keyboard.c
     run_tests.c
)

build_app(memtester PREINSTALL
    SOURCES
     memtester.c
     tests.c
     getopt.c
    COMPILE_DEFINITIONS
     "__badgevms__=(1)"
     "TEST_NARROW_WRITES=(1)"
)

build_app(bench_basic_a
    SOURCES 
     bench_basic_a.c
)

build_app(bench_basic_b
    SOURCES
     bench_basic_b.c
)

#
# Example apps
#

build_app(appdb_test
    SOURCES
     main.c
)

build_app(bmi270_test
    SOURCES
     bmi270_test.c
)

build_app(curl_test PREINSTALL
    SOURCES
     curl_test.c
)

build_app(framebuffer_test
    SOURCES
     framebuffer_test_a.c
)

build_app(hello PREINSTALL
    SOURCES 
     main.c
)

build_app(process_test PREINSTALL
    SOURCES
     main.c
)

build_app(readdir_test PREINSTALL
    SOURCES
     main.c
)

build_app(socket_test
    SOURCES
     socket_test.c
)

# AKA Snake
build_app(sdl_test PREINSTALL
    SOURCES
     sdl_test.c
    LIBRARIES
     sdl3
)

build_app(sdl2_test PREINSTALL
    SOURCES
     sdl2_test.c
    LIBRARIES
     sdl2
)

build_app(thread_test
    SOURCES
     main.c
)

build_app(wifi_test PREINSTALL
    SOURCES
     wifi_test.c
)

#build_app(http_screenshot_server PREINSTALL
#    SOURCES
#     http_screenshot_server.c
#)

get_property(SDK_APP_ELFS GLOBAL PROPERTY SDK_APP_ELFS)
add_custom_target(sdk_app_build_all ALL DEPENDS ${SDK_APP_ELFS})

cmake_minimum_required(VERSION 3.16)
project(ets2rpcmkii CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Output DLL name ──────────────────────────────────────────────
set(TARGET_NAME "ets2rpcmkii")

# ── Sources ──────────────────────────────────────────────────────
# Fully self-contained: SCS telemetry + built-in Discord IPC client +
# INI config/template engine. No Discord SDK, no lib/ folder, no
# external DLLs to ship.
add_library(${TARGET_NAME} SHARED
    src/plugin.cpp
    src/discord_ipc.cpp
    src/config.cpp
)

# ── Include dirs ─────────────────────────────────────────────────
# SCS SDK headers → https://modding.scssoft.com/wiki/SDK (headers only)
target_include_directories(${TARGET_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}/include/scs"
    "${CMAKE_SOURCE_DIR}/src"
)

# ── Windows specifics ────────────────────────────────────────────
if(WIN32)
    target_compile_definitions(${TARGET_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
    if(MSVC)
        # sources contain UTF-8 text (e.g. the middle dot in templates)
        target_compile_options(${TARGET_NAME} PRIVATE "/utf-8")
    endif()
    target_link_libraries(${TARGET_NAME} PRIVATE user32)

    if(MSVC)
        # Export scs_telemetry_init / scs_telemetry_shutdown via .def
        set_target_properties(${TARGET_NAME} PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS OFF
            LINK_FLAGS "/DEF:\"${CMAKE_SOURCE_DIR}/src/exports.def\""
        )
    else()
        # MinGW: .def files are passed directly as linker input
        target_link_options(${TARGET_NAME} PRIVATE
            "${CMAKE_SOURCE_DIR}/src/exports.def"
        )
    endif()
endif()

# ── Ini failsafe test ────────────────────────────────────────────
# Standalone exe that exercises the baked-in-defaults path (missing,
# garbage or deleted ets2rpcmkii.ini). Runs via ctest, so CI fails
# loudly if the failsafe ever breaks.
enable_testing()
add_executable(failsafe_test
    tests/failsafe_test.cpp
    src/config.cpp
)
target_include_directories(failsafe_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
if(WIN32)
    target_link_libraries(failsafe_test PRIVATE user32)
endif()
if(MSVC)
    # config.cpp carries UTF-8 text in DEFAULT_INI
    target_compile_options(failsafe_test PRIVATE "/utf-8")
endif()
add_test(NAME ini_failsafe COMMAND failsafe_test)

# ── Preset ini templates shipped with every build ────────────────
# templates/ets2rpcmkii.ini        the pre-given default config
# templates/presets/*.ini          style presets (distance, location,
#                                  most_info, ...)
# All of them carry a "Get on Steam" button out of the box.
file(GLOB ETS2RPCMKII_PRESETS "${CMAKE_SOURCE_DIR}/templates/presets/*.ini")

# ── Post-build: copy DLL + ini presets to the plugins folder ─────
# PLUGINS_DIR can be overridden:  cmake .. -DPLUGINS_DIR="D:/Games/..."
set(PLUGINS_DIR "C:/Program Files (x86)/Steam/steamapps/common/Euro Truck Simulator 2/bin/win_x64/plugins"
    CACHE PATH "ETS2 plugins directory")

if(EXISTS "${PLUGINS_DIR}")
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${TARGET_NAME}>
            "${PLUGINS_DIR}/${TARGET_NAME}.dll"
        # pre-given default ini, ready to edit (hot-reloaded in-game)
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/templates/ets2rpcmkii.ini"
            "${PLUGINS_DIR}/ets2rpcmkii.ini"
        COMMENT "Copying plugin + ets2rpcmkii.ini to ETS2 plugins folder"
    )

    # style presets land in a templates/ subfolder next to the DLL
    if(ETS2RPCMKII_PRESETS)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${PLUGINS_DIR}/templates"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ETS2RPCMKII_PRESETS}
                "${PLUGINS_DIR}/templates/"
            COMMENT "Copying ini presets into ${PLUGINS_DIR}/templates/"
        )
    endif()
endif()

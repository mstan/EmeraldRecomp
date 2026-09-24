cmake_minimum_required(VERSION 3.20)
project(EmeraldRecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─────────────────────────────────────────────────────────────────────
# EmeraldRecomp — static recompilation of Pokémon Emerald (GBA).
#
# Single-game repo, but built with the same per-variant scaffold as the
# FRLG / Ruby-Sapphire repos (Sonic3AndKnucklesRecomp pattern) so the
# layout is uniform across the Gen3 line. Per-variant data lives under
# variants/<name>/ (game.toml + config/ + symbols/ + generated/ + roms/).
#
#   Target          Game               ROM       Debug port
#   EmeraldRecomp   Pokémon Emerald    f3ae0881  19892
# ─────────────────────────────────────────────────────────────────────

set(GBARECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/gbarecomp"
    CACHE PATH "Path to the gbarecomp platform-core checkout")
set(RECOMP_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui"
    CACHE PATH "Path to the shared recomp-ui checkout")
set(GBARECOMP_RUNTIME_UI_ROOT "${RECOMP_UI_ROOT}"
    CACHE PATH "recomp-ui root used by the live GBA runtime adapter")
set(GBARECOMP_ENABLE_MODS ON CACHE BOOL
    "Build Emerald's built-in mod catalog and trusted-plugin runtime" FORCE)

if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "GBARECOMP_ROOT does not point to a gbarecomp checkout:\n"
        "  ${GBARECOMP_ROOT}\n"
        "Point it at the engine checkout (default ../gbarecomp) or pass\n"
        "-DGBARECOMP_ROOT=<path> at configure time.")
endif()

add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)

add_executable(verify_rom_hash tools/verify_rom_hash/main.cpp)

enable_testing()
add_executable(emerald_extended_view_test
    tests/extended_view_test.cpp src/emerald_extended_view.cpp src/emerald_object_view.cpp src/emerald_ui_view.cpp)
target_include_directories(emerald_extended_view_test PRIVATE src "${GBARECOMP_ROOT}/src/gba")
add_test(NAME emerald_extended_view_test COMMAND emerald_extended_view_test)
add_executable(emerald_view_capture_check
    tools/widescreen_capture_check.cpp src/emerald_extended_view.cpp src/emerald_object_view.cpp)
target_include_directories(emerald_view_capture_check PRIVATE src "${GBARECOMP_ROOT}/src/gba")
target_link_libraries(emerald_view_capture_check PRIVATE gbarecomp_gba)

# add_gba_variant(<target> <name> <port> <win_title> <game_name> <sha1>
#                  <crc32>)
function(add_gba_variant TARGET NAME PORT WIN_TITLE GAME_NAME ROM_SHA1 ROM_CRC32)
    set(VARIANT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/variants/${NAME}")
    if(EXISTS "${VARIANT_DIR}/generated/recompiled.cpp")
        message(FATAL_ERROR
            "${NAME}: stale monolithic generated/recompiled.cpp found. "
            "Regenerate with the current gba_recompile before building.")
    endif()
    file(GLOB VARIANT_GENERATED CONFIGURE_DEPENDS
        "${VARIANT_DIR}/generated/*.c"
        "${VARIANT_DIR}/generated/*.cpp")

    set(VARIANT_GAME_SOURCES src/main.cpp)
    if(GBARECOMP_ENABLE_MODS)
        list(APPEND VARIANT_GAME_SOURCES
            src/emerald_extended_view.cpp
            src/emerald_object_view.cpp
            src/mods/emerald_adaptive_view_plugin.cpp
            src/emerald_ui_view.cpp
            src/mods/emerald_absolute_rtc_plugin.cpp
            src/touch/emerald_scene.cpp
            src/touch/emerald_touch.cpp
            src/touch/emerald_touch_menus.cpp
            src/touch/emerald_touch_battle.cpp
            src/touch/emerald_touch_field.cpp)
    endif()
    if(ANDROID)
        # SDLActivity loads libmain.so; the shared gbarecomp Android shell
        # (gbarecomp/platform/android) builds this target as `main`.
        add_library(${TARGET} SHARED
            ${VARIANT_GAME_SOURCES}
            ${VARIANT_GENERATED})
    else()
        add_executable(${TARGET}
            ${VARIANT_GAME_SOURCES}
            ${VARIANT_GENERATED})
    endif()

    # Keep generated guest translation units at a bounded optimization and
    # debug-info cost. gba_recompile emits stable recompiled_NNN.cpp shards.
    set(VARIANT_RECOMPILED_SOURCES ${VARIANT_GENERATED})
    list(FILTER VARIANT_RECOMPILED_SOURCES INCLUDE REGEX
        "/recompiled_[0-9]+\\.cpp$")
    if(VARIANT_RECOMPILED_SOURCES)
        set_source_files_properties(${VARIANT_RECOMPILED_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-O1;-g0")
    endif()

    target_include_directories(${TARGET} PRIVATE
        "${GBARECOMP_ROOT}/src/armv4t"
        "${GBARECOMP_ROOT}/src/gba"
        "${GBARECOMP_ROOT}/src/runtime"
        "${GBARECOMP_ROOT}/src/debug"
        src)

    target_link_libraries(${TARGET} PRIVATE
        "-Wl,--start-group"
        gbarecomp_runtime
        gbarecomp_debug
        gbarecomp_gba
        gbarecomp_armv4t
        "-Wl,--end-group")

    target_compile_definitions(${TARGET} PRIVATE
        GBARECOMP_DEFAULT_DEBUG_PORT=${PORT}
        GBARECOMP_WINDOW_TITLE="${WIN_TITLE}"
        GBARECOMP_DEFAULT_GAME_CONFIG="variants/${NAME}/game.toml"
        GBARECOMP_BUILTIN_NAME="${GAME_NAME}"
        GBARECOMP_BUILTIN_SHA1="${ROM_SHA1}"
        GBARECOMP_BUILTIN_CRC32=${ROM_CRC32}
        GBARECOMP_BUILTIN_REGION="USA")

    if(GBAGAME_RECOMP_UI)
        target_link_libraries(${TARGET} PRIVATE game_launcher_ui)
        # Per-variant box art: variants share one build dir, so each stages
        # its own file and points its exe at it (RunOptions.launcher_boxart).
        set(VARIANT_BOXART "${VARIANT_DIR}/launcher/boxart.tga")
        if(EXISTS "${VARIANT_BOXART}")
            add_custom_command(TARGET ${TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    $<TARGET_FILE_DIR:${TARGET}>/assets/img
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VARIANT_BOXART}"
                    $<TARGET_FILE_DIR:${TARGET}>/assets/img/boxart_${NAME}.tga
                VERBATIM)
            target_compile_definitions(${TARGET} PRIVATE
                GBARECOMP_BOXART="assets/img/boxart_${NAME}.tga")
        endif()
    endif()

    # 256 MB stack reserve — see FireRedRecomp/CMakeLists.txt rationale
    # (Win64 SEH unwind table scales with ~90k recompiled functions).
    if(MSVC)
        target_link_options(${TARGET} PRIVATE /STACK:268435456)
    elseif(MINGW OR WIN32)
        target_link_options(${TARGET} PRIVATE -Wl,--stack,268435456)
    endif()
endfunction()

# ─────────────────────────────────────────────────────────────────────
# recomp-ui pre-boot launcher (Dear ImGui), consumed as a git submodule
# (junctioned locally to the shared checkout). Isolated in its own
# static lib so recomp-ui's sources + RECOMP_LAUNCHER define never
# touch the giant generated/*.c TU flags — only main.cpp (gated on
# GBAGAME_RECOMP_UI) and launcher_boot.cpp know the launcher exists.
# ─────────────────────────────────────────────────────────────────────
option(GBAGAME_RECOMP_UI "Build the recomp-ui pre-boot launcher" ON)
if(GBAGAME_RECOMP_UI)
    include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
    add_library(game_launcher_ui STATIC src/game_launcher_boot.cpp)
    target_include_directories(game_launcher_ui PRIVATE
        "${GBARECOMP_ROOT}/src/runtime"   # launcher_seam.h + runtime.h
        "${GBARECOMP_ROOT}/src/gba"
        src
    )
    if(GBARECOMP_ENABLE_MODS)
        target_compile_definitions(game_launcher_ui PRIVATE
            GBARECOMP_ENABLE_MODS=1)
    endif()
    recomp_target_launcher_ui(game_launcher_ui CONSOLE gba)
    set_target_properties(game_launcher_ui PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # Android builds provide SDL2 from the shared shell's SDL subproject.
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 REQUIRED)
    endif()
    target_link_libraries(game_launcher_ui PUBLIC SDL2::SDL2)
    if(WIN32)
        target_link_libraries(game_launcher_ui PUBLIC opengl32)
    endif()
    set_property(SOURCE src/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS GBAGAME_RECOMP_UI)
endif()

# Emerald — USA, sha1 f3ae0881…
if(ANDROID)
    set(EMERALD_TARGET main)
else()
    set(EMERALD_TARGET EmeraldRecomp)
endif()
add_gba_variant(${EMERALD_TARGET} emerald 19892 "EmeraldRecomp"
    "Pokemon Emerald (USA)" "f3ae088181bf583e55daf962a92bb46f4f1d07b7" 0x1F1C08FB)

# Ship the game-owned, disabled-by-default mod catalog beside the executable.
# (Android packages the catalog into the app payload instead.)
set(EMERALD_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")
if(NOT ANDROID AND EXISTS "${EMERALD_PRELOADED_MODS}/packages")
    add_custom_command(TARGET EmeraldRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EMERALD_PRELOADED_MODS}"
            "$<TARGET_FILE_DIR:EmeraldRecomp>/mods"
        COMMENT "Preloading the Emerald mod catalog")
endif()

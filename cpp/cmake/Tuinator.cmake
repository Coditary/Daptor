# Fetch Tuinator and expose tuinator::tuinator (requires wide ncurses).
#
# Resolution order:
#   1. -DTUI_DEBUG_TUINATOR_DIR=<path> or TUINATOR_DIR env var (local checkout,
#      used as-is; the mouse/text-input patches below are NOT applied).
#   2. Sibling checkout at ../../Tuinator (same semantics).
#   3. Pinned git fetch + local patches.

find_package(PkgConfig REQUIRED)
pkg_check_modules(TUI_DEBUG_NCURSES REQUIRED ncursesw)

include(FetchContent)

set(TUINATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TUINATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TUINATOR_INSTALL OFF CACHE BOOL "" FORCE)

set(TUI_DEBUG_TUINATOR_DIR "" CACHE PATH "Path to a local Tuinator checkout (skips git fetch)")
if(NOT TUI_DEBUG_TUINATOR_DIR AND DEFINED ENV{TUINATOR_DIR})
    set(TUI_DEBUG_TUINATOR_DIR "$ENV{TUINATOR_DIR}")
endif()

set(_tuinator_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../Tuinator")
if(TUI_DEBUG_TUINATOR_DIR)
    set(_tuinator_dir "${TUI_DEBUG_TUINATOR_DIR}")
endif()

set(_tuinator_mouse_patch "${CMAKE_CURRENT_LIST_DIR}/patches/tuinator-mouse-buttons.patch")
set(_tuinator_text_input_patch "${CMAKE_CURRENT_LIST_DIR}/patches/tuinator-text-input-cursor.patch")
set(_tuinator_pointer_hover_patch "${CMAKE_CURRENT_LIST_DIR}/patches/tuinator-pointer-hover.patch")
set(_tuinator_patch_cmd
    patch -p1 --forward -r - < "${_tuinator_mouse_patch}" || true
    COMMAND patch -p1 --forward -r - < "${_tuinator_text_input_patch}" || true
    COMMAND patch -p1 --forward -r - < "${_tuinator_pointer_hover_patch}" || true
)

if(EXISTS "${_tuinator_dir}/CMakeLists.txt")
    message(STATUS "Using local Tuinator checkout: ${_tuinator_dir}")
    FetchContent_Declare(
        tuinator
        SOURCE_DIR "${_tuinator_dir}"
    )
else()
    FetchContent_Declare(
        tuinator
        GIT_REPOSITORY https://github.com/Coditary/Tuinator.git
        GIT_TAG 5bad80fadcf0ae13e48b459dce0bd2a2d7df5df8
        GIT_SHALLOW TRUE
        PATCH_COMMAND ${_tuinator_patch_cmd}
    )
endif()

FetchContent_MakeAvailable(tuinator)

if(NOT TARGET tuinator::tuinator)
    message(FATAL_ERROR "Tuinator fetch succeeded but tuinator::tuinator target is missing")
endif()

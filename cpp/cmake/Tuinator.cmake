# Fetch Tuinator and expose tuinator::tuinator (requires wide ncurses).

find_package(PkgConfig REQUIRED)
pkg_check_modules(TUI_DEBUG_NCURSES REQUIRED ncursesw)

include(FetchContent)

set(TUINATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TUINATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TUINATOR_INSTALL OFF CACHE BOOL "" FORCE)

set(_tuinator_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../Tuinator")
set(_tuinator_mouse_patch "${CMAKE_CURRENT_LIST_DIR}/patches/tuinator-mouse-buttons.patch")
set(_tuinator_text_input_patch "${CMAKE_CURRENT_LIST_DIR}/patches/tuinator-text-input-cursor.patch")
set(_tuinator_patch_cmd
    patch -p1 --forward -r - < "${_tuinator_mouse_patch}" || true
    COMMAND patch -p1 --forward -r - < "${_tuinator_text_input_patch}" || true
)

if(EXISTS "${_tuinator_dir}/CMakeLists.txt")
    FetchContent_Declare(
        tuinator
        SOURCE_DIR "${_tuinator_dir}"
        PATCH_COMMAND ${_tuinator_patch_cmd}
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

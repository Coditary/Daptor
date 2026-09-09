# Fetch Tuinator and expose tuinator::tuinator (requires wide ncurses).

find_package(PkgConfig REQUIRED)
pkg_check_modules(TUI_DEBUG_NCURSES REQUIRED ncursesw)

include(FetchContent)

set(TUINATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TUINATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TUINATOR_INSTALL OFF CACHE BOOL "" FORCE)

set(_tuinator_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../Tuinator")
if(EXISTS "${_tuinator_dir}/CMakeLists.txt")
    FetchContent_Declare(
        tuinator
        SOURCE_DIR "${_tuinator_dir}"
    )
else()
    FetchContent_Declare(
        tuinator
        GIT_REPOSITORY https://github.com/Coditary/Tuinator.git
        GIT_TAG main
        GIT_SHALLOW TRUE
    )
endif()

FetchContent_MakeAvailable(tuinator)

if(NOT TARGET tuinator::tuinator)
    message(FATAL_ERROR "Tuinator fetch succeeded but tuinator::tuinator target is missing")
endif()

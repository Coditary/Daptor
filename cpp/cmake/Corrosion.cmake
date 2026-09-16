# Import daptor_core static library from the Rust workspace via Corrosion.

include(FetchContent)

FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Corrosion)

set(_tui_debug_root "${CMAKE_CURRENT_SOURCE_DIR}/..")

corrosion_import_crate(
    MANIFEST_PATH "${_tui_debug_root}/Cargo.toml"
    CRATES daptor-core
    PROFILE release
)

if(NOT TARGET daptor_core)
    message(FATAL_ERROR "corrosion_import_crate did not create target daptor_core")
endif()

cmake_minimum_required(VERSION 3.20)
project(daptor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# /tmp is often quota-limited; keep compiler temporaries in the repo tree.
set(DAPTOR_TMPDIR "${CMAKE_SOURCE_DIR}/.tmp")
file(MAKE_DIRECTORY "${DAPTOR_TMPDIR}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-pipe)
endif()



include(cmake/Tuinator.cmake)
include(cmake/Corrosion.cmake)

set(DAPTOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")

add_executable(daptor
    src/main.cpp
    src/ui/app.cpp
    src/ui/debug_ui_model.cpp
    src/ui/snapshot_parser.cpp
    src/ui/scopes_panel.cpp
    src/ui/stacks_panel.cpp
    src/ui/breakpoints_panel.cpp
    src/ui/context_menu.cpp
    src/ui/watches_panel.cpp
    src/ui/source_panel.cpp
    src/ui/source_tab_bar.cpp
    src/ui/step_in_selection.cpp
    src/ui/console_panel.cpp
    src/ui/repl_panel.cpp
    src/ui/tty_setup.cpp
    src/ui/session_backend.cpp
    src/ui/session_io_thread.cpp
    src/ui/rust_session_backend.cpp
    src/ui/mock_session_backend.cpp
    src/ui/resizable_split_pane.cpp
    src/ui/divider_paint.cpp
    src/ui/stacked_pane.cpp
    src/ui/layout_tree.cpp
    src/ui/layout_drag.cpp
    src/ui/sidebar_slot.cpp
    src/ui/memory_panel.cpp
    src/ui/disassembly_panel.cpp
    src/ui/runtime_source_panel.cpp
    src/ui/dap_view_formatters.cpp
    src/ui/panel_slot.cpp
    src/ui/shared_widget_host.cpp
    src/ui/background_widget.cpp
    src/ui/dap_ui_theme.cpp
    src/ui/syntax_theme.cpp
    src/ui/xdg_paths.cpp
    src/ui/app_config.cpp
    src/ui/launch_plan.cpp
    src/ui/theme_loader.cpp
    src/ui/yaml_config.cpp
    src/ui/keybindings.cpp
    src/ui/layout_config.cpp
    src/ui/clipboard.cpp
    src/ui/controls_bar.cpp
    src/ui/navigable_list_view.cpp
    src/ui/titled_scroll_pane.cpp
    src/ui/workspace_files.cpp
    src/ui/file_picker.cpp
    src/ui/file_tree_panel.cpp
    src/ui/resources_panel.cpp
    src/ui/process_metrics.cpp
    src/ui/network_panel.cpp
    src/ui/network_mock_data.cpp
    src/ui/compose_templates.cpp
)

target_include_directories(daptor
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${DAPTOR_INCLUDE_DIR}"
)

target_link_libraries(daptor
    PRIVATE
        tuinator::tuinator
        daptor_core
        ${DAPTOR_NCURSES_LIBRARIES}
)

target_include_directories(daptor
    PRIVATE
        ${DAPTOR_NCURSES_INCLUDE_DIRS}
)

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(daptor PRIVATE nlohmann_json::nlohmann_json)
endif()

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tui_debug_ui {

struct WorkspaceNode {
    std::filesystem::path path;
    std::string name;
    bool is_directory = false;
    bool expanded = false;
    std::vector<WorkspaceNode> children;
};

/// Collect source files under `root`, sorted by relative path (case-insensitive).
std::vector<std::filesystem::path> list_source_files(const std::filesystem::path& root);

/// Build a folder tree from a flat source-file list.
WorkspaceNode build_workspace_tree(const std::filesystem::path& root,
                                   const std::vector<std::filesystem::path>& files);

/// Keep only files located under `folder` (or all files when `folder` is empty / equals `root`).
std::vector<std::filesystem::path> filter_files_under_folder(const std::vector<std::filesystem::path>& files,
                                                             const std::filesystem::path& folder);

/// Relative path for display (falls back to absolute when not under `root`).
std::string relative_display(const std::filesystem::path& root, const std::filesystem::path& path);

struct PickerPathParts {
    std::string directory;
    std::string filename;
};

/// Split a workspace-relative path into directory prefix and filename for picker display.
PickerPathParts picker_path_parts(const std::filesystem::path& root, const std::filesystem::path& path);

/// Shortest unique labels for a picker list; always includes parent folders when needed to disambiguate.
std::vector<std::string> picker_display_labels(const std::filesystem::path& root,
                                               const std::vector<std::filesystem::path>& paths);

/// Fuzzy filter on relative paths; results are sorted by match quality (best first).
std::vector<std::filesystem::path> fuzzy_filter_files(const std::vector<std::filesystem::path>& files,
                                                      const std::filesystem::path& root, const std::string& query);

/// Workspace root for a debug target (parent directory of the program).
std::filesystem::path workspace_root_for_program(const std::filesystem::path& program_path);

} // namespace tui_debug_ui

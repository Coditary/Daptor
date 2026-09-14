#include "tui_debug_ui/workspace_files.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tui_debug_ui {

namespace {

constexpr std::size_t kMaxDepth = 8;
constexpr std::size_t kMaxFiles = 2000;

const std::unordered_set<std::string> kSkipDirs = {
    ".git", ".svn", "node_modules", "target", "__pycache__", ".venv", "venv", "dist", "build", ".cargo", "reference",
};

const char* kSourceExtensions[] = {
    "py",  "rs",  "go",  "js",  "ts",  "tsx", "jsx", "c",   "cc",  "cpp", "h",   "hpp", "java", "lua", "rb",
    "sh",  "bash", "zsh", "swift", "kt",  "cs",  "php", "ml",  "zig",
};

bool is_source_file(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    if (ext.size() < 2 || ext.front() != '.') {
        return false;
    }
    const std::string lowered = ext.substr(1);
    for (const char* candidate : kSourceExtensions) {
        if (lowered == candidate) {
            return true;
        }
    }
    return false;
}

std::string lowercase_copy(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string trim_copy(const std::string& value) {
    const auto start = value.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t");
    return value.substr(start, end - start + 1);
}

std::vector<std::string> fuzzy_query_tokens(const std::string& query) {
    std::vector<std::string> tokens;
    std::istringstream stream(query);
    std::string token;
    while (stream >> token) {
        tokens.push_back(lowercase_copy(token));
    }
    return tokens;
}

bool is_path_separator_char(char ch) { return ch == '/' || ch == '\\' || ch == '_' || ch == '-' || ch == '.'; }

std::optional<int> fuzzy_score_token(const std::string& haystack, const std::string& token) {
    if (token.empty()) {
        return 0;
    }
    if (haystack.empty()) {
        return std::nullopt;
    }

    int score = 0;
    std::size_t haystack_index = 0;
    int previous_match = -1;
    int consecutive = 0;

    for (const char pattern_char : token) {
        bool found = false;
        while (haystack_index < haystack.size()) {
            if (haystack[haystack_index] == pattern_char) {
                score += 1;
                if (previous_match >= 0 && static_cast<int>(haystack_index) == previous_match + 1) {
                    consecutive += 1;
                    score += consecutive * 4;
                } else {
                    consecutive = 0;
                }
                if (haystack_index == 0 || is_path_separator_char(haystack[haystack_index - 1])) {
                    score += 10;
                }
                if (haystack_index < 3) {
                    score += 4;
                }
                previous_match = static_cast<int>(haystack_index);
                ++haystack_index;
                found = true;
                break;
            }
            ++haystack_index;
        }
        if (!found) {
            return std::nullopt;
        }
    }

    score *= 100;
    score -= static_cast<int>(haystack.size());
    score -= previous_match;
    return score;
}

std::optional<int> fuzzy_score_path(const std::string& haystack, const std::vector<std::string>& tokens) {
    if (tokens.empty()) {
        return 0;
    }

    int total_score = 0;
    for (const std::string& token : tokens) {
        const std::optional<int> token_score = fuzzy_score_token(haystack, token);
        if (!token_score.has_value()) {
            return std::nullopt;
        }
        total_score += *token_score;
    }
    return total_score;
}

void collect_files(const std::filesystem::path& root, const std::filesystem::path& dir, std::size_t depth,
                   std::vector<std::filesystem::path>& out) {
    if (depth > kMaxDepth || out.size() >= kMaxFiles) {
        return;
    }

    std::error_code ec;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || out.size() >= kMaxFiles) {
            return;
        }

        const std::filesystem::path& path = entry.path();
        std::error_code type_ec;
        if (entry.is_directory(type_ec)) {
            if (type_ec) {
                continue;
            }
            const std::string name = path.filename().string();
            if (kSkipDirs.contains(name)) {
                continue;
            }
            collect_files(root, path, depth + 1, out);
        } else if (entry.is_regular_file(type_ec) && !type_ec && is_source_file(path)) {
            out.push_back(path);
        }
    }
}

} // namespace

std::vector<std::filesystem::path> list_source_files(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    if (root.empty()) {
        return files;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return files;
    }

    collect_files(root, root, 0, files);
    std::sort(files.begin(), files.end(), [&](const std::filesystem::path& left, const std::filesystem::path& right) {
        return lowercase_copy(relative_display(root, left)) < lowercase_copy(relative_display(root, right));
    });
    if (files.size() > kMaxFiles) {
        files.resize(kMaxFiles);
    }
    return files;
}

std::string relative_display(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    if (!ec && !relative.empty()) {
        return relative.generic_string();
    }
    return path.generic_string();
}

PickerPathParts picker_path_parts(const std::filesystem::path& root, const std::filesystem::path& path) {
    const std::string relative = relative_display(root, path);
    const std::size_t separator = relative.rfind('/');
    if (separator == std::string::npos) {
        return {"", relative};
    }
    return {relative.substr(0, separator + 1), relative.substr(separator + 1)};
}

std::vector<std::string> split_relative_path_parts(const std::string& relative) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < relative.size()) {
        const std::size_t end = relative.find('/', start);
        if (end == std::string::npos) {
            parts.push_back(relative.substr(start));
            break;
        }
        parts.push_back(relative.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string join_relative_suffix(const std::vector<std::string>& parts, int suffix_count) {
    if (suffix_count <= 0 || parts.empty()) {
        return {};
    }
    const int start = std::max(0, static_cast<int>(parts.size()) - suffix_count);
    std::string joined;
    for (int index = start; index < static_cast<int>(parts.size()); ++index) {
        if (!joined.empty()) {
            joined += '/';
        }
        joined += parts[index];
    }
    return joined;
}

std::vector<std::string> picker_display_labels(const std::filesystem::path& root,
                                               const std::vector<std::filesystem::path>& paths) {
    struct Entry {
        std::vector<std::string> parts;
        std::string full;
        std::string basename;
    };

    std::vector<Entry> entries;
    entries.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        const std::string full = relative_display(root, path);
        entries.push_back({split_relative_path_parts(full), full, path.filename().string()});
    }

    std::unordered_map<std::string, int> basename_counts;
    for (const Entry& entry : entries) {
        basename_counts[entry.basename] += 1;
    }

    std::vector<std::string> labels;
    labels.reserve(paths.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const Entry& entry = entries[index];
        if (basename_counts[entry.basename] <= 1) {
            labels.push_back(entry.full);
            continue;
        }

        std::string label = entry.full;
        for (int suffix_count = 1; suffix_count <= static_cast<int>(entry.parts.size()); ++suffix_count) {
            const std::string candidate = join_relative_suffix(entry.parts, suffix_count);
            bool unique = true;
            for (std::size_t other_index = 0; other_index < entries.size(); ++other_index) {
                if (other_index == index) {
                    continue;
                }
                const std::string other_candidate =
                    join_relative_suffix(entries[other_index].parts, suffix_count);
                if (other_candidate == candidate) {
                    unique = false;
                    break;
                }
            }
            if (unique) {
                label = candidate;
                break;
            }
        }
        labels.push_back(label);
    }
    return labels;
}

std::vector<std::filesystem::path> fuzzy_filter_files(const std::vector<std::filesystem::path>& files,
                                                      const std::filesystem::path& root, const std::string& query) {
    const std::vector<std::string> tokens = fuzzy_query_tokens(trim_copy(query));
    if (tokens.empty()) {
        return files;
    }

    struct ScoredPath {
        std::filesystem::path path;
        int score = 0;
        std::string label;
    };

    std::vector<ScoredPath> scored;
    for (const std::filesystem::path& path : files) {
        const std::string label = lowercase_copy(relative_display(root, path));
        const std::optional<int> score = fuzzy_score_path(label, tokens);
        if (!score.has_value()) {
            continue;
        }
        scored.push_back({path, *score, label});
    }

    std::sort(scored.begin(), scored.end(), [](const ScoredPath& left, const ScoredPath& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.label.size() != right.label.size()) {
            return left.label.size() < right.label.size();
        }
        return left.label < right.label;
    });

    std::vector<std::filesystem::path> filtered;
    filtered.reserve(scored.size());
    for (const ScoredPath& entry : scored) {
        filtered.push_back(entry.path);
    }
    return filtered;
}

WorkspaceNode* find_or_insert_child(WorkspaceNode& parent, const std::string& name, bool is_directory,
                                    const std::filesystem::path& path) {
    for (WorkspaceNode& child : parent.children) {
        if (child.name == name) {
            return &child;
        }
    }
    parent.children.push_back({path, name, is_directory, false, {}});
    return &parent.children.back();
}

void sort_workspace_node(WorkspaceNode& node) {
    std::sort(node.children.begin(), node.children.end(), [](const WorkspaceNode& left, const WorkspaceNode& right) {
        if (left.is_directory != right.is_directory) {
            return left.is_directory > right.is_directory;
        }
        return lowercase_copy(left.name) < lowercase_copy(right.name);
    });
    for (WorkspaceNode& child : node.children) {
        sort_workspace_node(child);
    }
}

void expand_workspace_defaults(WorkspaceNode& node, int depth) {
    if (!node.is_directory) {
        return;
    }
    node.expanded = depth < 2;
    for (WorkspaceNode& child : node.children) {
        expand_workspace_defaults(child, depth + 1);
    }
}

WorkspaceNode build_workspace_tree(const std::filesystem::path& root,
                                   const std::vector<std::filesystem::path>& files) {
    WorkspaceNode tree;
    tree.path = root;
    tree.name = root.filename().empty() ? std::string{"."} : root.filename().string();
    tree.is_directory = true;
    tree.expanded = true;

    for (const std::filesystem::path& file : files) {
        const std::vector<std::string> parts = split_relative_path_parts(relative_display(root, file));
        if (parts.empty()) {
            continue;
        }

        WorkspaceNode* current = &tree;
        std::filesystem::path current_path = root;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const bool is_directory = index + 1 < parts.size();
            current_path /= parts[index];
            current = find_or_insert_child(*current, parts[index], is_directory, current_path);
        }
    }

    sort_workspace_node(tree);
    expand_workspace_defaults(tree, 0);
    return tree;
}

std::vector<std::filesystem::path> filter_files_under_folder(const std::vector<std::filesystem::path>& files,
                                                             const std::filesystem::path& folder) {
    if (folder.empty()) {
        return files;
    }

    std::vector<std::filesystem::path> filtered;
    for (const std::filesystem::path& file : files) {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(file, folder, ec);
        if (ec || relative.empty()) {
            continue;
        }
        const std::string relative_text = relative.generic_string();
        if (relative_text.rfind("..", 0) == 0) {
            continue;
        }
        filtered.push_back(file);
    }
    return filtered;
}

std::filesystem::path workspace_root_for_program(const std::filesystem::path& program_path) {
    if (program_path.empty()) {
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }

    const std::filesystem::path parent = program_path.parent_path();
    if (!parent.empty()) {
        return parent;
    }

    std::error_code ec;
    return std::filesystem::current_path(ec);
}

} // namespace tui_debug_ui

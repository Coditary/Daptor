#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tui_debug_ui {

/// Minimal YAML subset parser for daptor config files.
struct YamlNode {
    enum class Kind { Null, Scalar, Sequence, Mapping } kind = Kind::Null;
    std::string scalar;
    std::vector<YamlNode> sequence;
    std::map<std::string, YamlNode> mapping;

    [[nodiscard]] bool is_mapping() const { return kind == Kind::Mapping; }
    [[nodiscard]] bool is_sequence() const { return kind == Kind::Sequence; }
    [[nodiscard]] bool is_scalar() const { return kind == Kind::Scalar; }

    [[nodiscard]] const YamlNode* get(const std::string& key) const;
    [[nodiscard]] std::optional<std::string> as_string() const;
    [[nodiscard]] std::optional<int> as_int() const;
};

[[nodiscard]] YamlNode parse_yaml(const std::string& content);

}  // namespace tui_debug_ui

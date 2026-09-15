#include "tui_debug_ui/yaml_config.hpp"

#include <cctype>

namespace tui_debug_ui {
namespace {

std::string trim(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(start, end - start);
}

std::string strip_quotes(std::string value) {
    value = trim(value);
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

int count_indent(const std::string& line) {
    int indent = 0;
    for (const char ch : line) {
        if (ch == ' ') {
            ++indent;
            continue;
        }
        if (ch == '\t') {
            indent += 2;
            continue;
        }
        break;
    }
    return indent;
}

bool parse_scalar_pair(const std::string& content, std::string& key_out, std::string& value_out) {
    const std::size_t colon = content.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    key_out = trim(content.substr(0, colon));
    value_out = strip_quotes(trim(content.substr(colon + 1)));
    return !key_out.empty();
}

YamlNode make_scalar(const std::string& value) {
    YamlNode node;
    node.kind = YamlNode::Kind::Scalar;
    node.scalar = value;
    return node;
}

YamlNode& ensure_mapping(YamlNode& node) {
    if (node.kind == YamlNode::Kind::Null || node.kind == YamlNode::Kind::Mapping) {
        node.kind = YamlNode::Kind::Mapping;
        return node;
    }
    node.kind = YamlNode::Kind::Mapping;
    node.sequence.clear();
    node.scalar.clear();
    return node;
}

YamlNode& ensure_sequence(YamlNode& node) {
    if (node.kind == YamlNode::Kind::Null || node.kind == YamlNode::Kind::Sequence) {
        node.kind = YamlNode::Kind::Sequence;
        return node;
    }
    node.kind = YamlNode::Kind::Sequence;
    node.mapping.clear();
    node.scalar.clear();
    return node;
}

struct ParseFrame {
    YamlNode* node = nullptr;
    int indent = -1;
};

}  // namespace

const YamlNode* YamlNode::get(const std::string& key) const {
    if (kind != Kind::Mapping) {
        return nullptr;
    }
    const auto it = mapping.find(key);
    if (it == mapping.end()) {
        return nullptr;
    }
    return &it->second;
}

std::optional<std::string> YamlNode::as_string() const {
    if (kind == Kind::Scalar) {
        return scalar;
    }
    return std::nullopt;
}

std::optional<int> YamlNode::as_int() const {
    if (kind != Kind::Scalar) {
        return std::nullopt;
    }
    try {
        return std::stoi(scalar);
    } catch (...) {
        return std::nullopt;
    }
}

YamlNode parse_yaml(const std::string& content) {
    YamlNode root;
    root.kind = YamlNode::Kind::Mapping;
    std::vector<ParseFrame> stack{{&root, -1}};

    std::size_t line_start = 0;
    for (std::size_t index = 0; index <= content.size(); ++index) {
        if (index != content.size() && content[index] != '\n') {
            continue;
        }

        const std::string raw_line = content.substr(line_start, index - line_start);
        line_start = index + 1;

        const std::string trimmed = trim(raw_line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const int indent = count_indent(raw_line);
        while (stack.size() > 1 && indent <= stack.back().indent) {
            stack.pop_back();
        }

        YamlNode* parent = stack.back().node;

        if (trimmed.rfind("- ", 0) == 0) {
            YamlNode& sequence = ensure_sequence(*parent);
            YamlNode item;
            const std::string item_content = trim(trimmed.substr(2));
            std::string key;
            std::string value;
            if (parse_scalar_pair(item_content, key, value)) {
                item.kind = YamlNode::Kind::Mapping;
                if (value.empty()) {
                    item.mapping[key] = YamlNode{};
                } else {
                    item.mapping[key] = make_scalar(value);
                }
                sequence.sequence.push_back(std::move(item));
                stack.push_back({&sequence.sequence.back(), indent});
            } else {
                item = make_scalar(item_content);
                sequence.sequence.push_back(std::move(item));
            }
            continue;
        }

        std::string key;
        std::string value;
        if (!parse_scalar_pair(trimmed, key, value)) {
            continue;
        }

        YamlNode& mapping = ensure_mapping(*parent);
        if (value.empty()) {
            mapping.mapping[key] = YamlNode{};
            stack.push_back({&mapping.mapping[key], indent});
        } else {
            mapping.mapping[key] = make_scalar(value);
        }
    }

    return root;
}

}  // namespace tui_debug_ui

#pragma once

#include <tui_debug_ui/source_panel.hpp>

extern "C" {
#include <tui_debug.h>
}

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define TUI_DEBUG_UI_HAS_NLOHMANN_JSON 1
#endif

namespace tui_debug_ui {

struct HighlightBridgeError {
    std::string message;
};

/// Build unstyled viewport lines on the UI thread so gutters/breakpoints paint immediately.
inline std::vector<HighlightedLine> build_plain_viewport_lines(const std::string& source, int first_line,
                                                                 int line_count) {
    if (first_line < 1 || line_count <= 0) {
        return {};
    }

    std::vector<HighlightedLine> result;
    result.reserve(static_cast<std::size_t>(line_count));

    int current_line = 1;
    std::size_t pos = 0;
    while (pos <= source.size() && current_line < first_line) {
        while (pos < source.size() && source[pos] != '\n') {
            ++pos;
        }
        ++current_line;
        if (pos < source.size()) {
            ++pos;
        }
    }

    while (current_line < first_line + line_count && pos <= source.size()) {
        const std::size_t line_start = pos;
        while (pos < source.size() && source[pos] != '\n') {
            ++pos;
        }

        std::string text = source.substr(line_start, pos - line_start);
        if (!text.empty() && text.back() == '\r') {
            text.pop_back();
        }

        HighlightedLine line{};
        line.line_number = current_line;
        line.spans.push_back({std::move(text), HighlightKind::Default});
        result.push_back(std::move(line));

        ++current_line;
        if (pos < source.size()) {
            ++pos;
        }
    }

    return result;
}

/// JSON schema (v1):
/// `[{"line":1,"spans":[{"text":"def","kind":"keyword"}]}]`
/// `line_number` is accepted as an alias for `line`.
inline std::vector<HighlightedLine> parse_highlight_json(std::string_view json) {
#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    const nlohmann::json root = nlohmann::json::parse(json);
    if (!root.is_array()) {
        throw std::runtime_error("highlight JSON root must be an array");
    }

    std::vector<HighlightedLine> lines;
    lines.reserve(root.size());

    for (const nlohmann::json& entry : root) {
        HighlightedLine line{};
        if (entry.contains("line")) {
            line.line_number = entry.at("line").get<int>();
        } else if (entry.contains("line_number")) {
            line.line_number = entry.at("line_number").get<int>();
        }

        if (entry.contains("spans") && entry.at("spans").is_array()) {
            for (const nlohmann::json& span_json : entry.at("spans")) {
                HighlightSpan span{};
                span.text = span_json.value("text", std::string{});
                span.kind = highlight_kind_from_string(span_json.value("kind", "default"));
                line.spans.push_back(std::move(span));
            }
        }

        lines.push_back(std::move(line));
    }

    return lines;
#else
    (void)json;
    return {};
#endif
}

inline std::optional<std::string> fetch_highlight_json(const std::string& language, const std::string& source,
                                                       int first_line, int line_count,
                                                       std::size_t buffer_capacity = 65536) {
    std::vector<char> buffer(buffer_capacity, '\0');
    const int status = tui_debug_highlight_viewport(language.c_str(), source.c_str(), first_line, line_count,
                                                    buffer.data(), buffer.size());
    if (status != 0) {
        return std::nullopt;
    }
    return std::string{buffer.data()};
}

inline std::vector<HighlightedLine> fetch_highlight_viewport(const std::string& language,
                                                               const std::string& source, int first_line,
                                                               int line_count,
                                                               HighlightBridgeError* error = nullptr) {
    const auto json = fetch_highlight_json(language, source, first_line, line_count);
    if (!json.has_value()) {
        if (error != nullptr) {
            const char* last = tui_debug_last_error();
            error->message = (last != nullptr && last[0] != '\0') ? last : "tui_debug_highlight_viewport failed";
        }
        return {};
    }

    try {
        return parse_highlight_json(*json);
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            error->message = ex.what();
        }
        return {};
    }
}

} // namespace tui_debug_ui

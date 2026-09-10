#include "tui_debug_ui/step_in_selection.hpp"

#include <cctype>
#include <unordered_set>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define TUI_DEBUG_UI_HAS_NLOHMANN_JSON 1
#endif

namespace tui_debug_ui {
namespace {

bool is_ident_start(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool is_ident_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool is_skipped_callee(const std::string& ident) {
    static const std::unordered_set<std::string> kSkip = {
        "if",   "while", "for",    "return", "print", "elif", "else", "assert", "raise",
        "pass", "break", "continue", "with", "lambda", "not", "and", "or",
    };
    return kSkip.contains(ident);
}

std::string base_label_name(const std::string& label) {
    std::string trimmed = label;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) {
        trimmed.pop_back();
    }
    const std::size_t call_pos = trimmed.find(" (call ");
    if (call_pos != std::string::npos) {
        trimmed = trimmed.substr(0, call_pos);
    }
    const std::size_t paren_pos = trimmed.find('(');
    if (paren_pos != std::string::npos) {
        trimmed = trimmed.substr(0, paren_pos);
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) {
        trimmed.pop_back();
    }
    return trimmed;
}

const StepInTargetSpan* find_span_for_label(const std::vector<StepInTargetSpan>& parsed,
                                            const std::string& label) {
    const std::string base = base_label_name(label);
    for (const StepInTargetSpan& span : parsed) {
        if (base_label_name(span.label) == base) {
            return &span;
        }
    }
    return nullptr;
}

}  // namespace

std::vector<StepInTargetSpan> find_step_in_targets_on_line(const std::string& line_text) {
    std::vector<StepInTargetSpan> targets;

    for (std::size_t index = 0; index < line_text.size();) {
        if (!is_ident_start(line_text[index])) {
            ++index;
            continue;
        }

        const std::size_t name_start = index;
        while (index < line_text.size() && is_ident_char(line_text[index])) {
            ++index;
        }

        const std::string ident = line_text.substr(name_start, index - name_start);
        while (index < line_text.size() && std::isspace(static_cast<unsigned char>(line_text[index])) != 0) {
            ++index;
        }

        if (index >= line_text.size() || line_text[index] != '(' || is_skipped_callee(ident)) {
            continue;
        }

        StepInTargetSpan target{};
        target.start_column = static_cast<int>(name_start);
        target.end_column = static_cast<int>(index + 1);
        target.label = ident + "()";
        targets.push_back(std::move(target));
    }

    return targets;
}

std::vector<StepInTargetSpan> build_step_in_target_spans(const std::string& line_text,
                                                         const std::string& targets_json,
                                                         std::string& error_out) {
    error_out.clear();
    const std::vector<StepInTargetSpan> parsed = find_step_in_targets_on_line(line_text);

#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    try {
        const nlohmann::json root = nlohmann::json::parse(targets_json);
        if (!root.is_array()) {
            error_out = "step-in targets payload is not an array";
            return parsed;
        }

        std::vector<StepInTargetSpan> merged;
        merged.reserve(root.size());
        int fallback_index = 0;
        for (const nlohmann::json& entry : root) {
            if (!entry.is_object()) {
                continue;
            }

            StepInTargetSpan span{};
            span.target_id = entry.value("id", static_cast<std::int64_t>(-1));
            span.label = entry.value("label", std::string{});

            const bool has_column = entry.contains("column") && entry.at("column").is_number_integer();
            const bool has_end_column =
                entry.contains("endColumn") && entry.at("endColumn").is_number_integer();
            if (has_column) {
                const int column = static_cast<int>(entry.at("column").get<std::int64_t>());
                span.start_column = std::max(0, column - 1);
                if (has_end_column) {
                    span.end_column = static_cast<int>(entry.at("endColumn").get<std::int64_t>());
                } else {
                    span.end_column = span.start_column + 1;
                }
            } else if (const StepInTargetSpan* matched = find_span_for_label(parsed, span.label)) {
                span.start_column = matched->start_column;
                span.end_column = matched->end_column;
            } else if (fallback_index < static_cast<int>(parsed.size())) {
                span.start_column = parsed[static_cast<std::size_t>(fallback_index)].start_column;
                span.end_column = parsed[static_cast<std::size_t>(fallback_index)].end_column;
                ++fallback_index;
            } else {
                continue;
            }

            if (span.end_column <= span.start_column) {
                span.end_column = span.start_column + 1;
            }
            if (span.label.empty()) {
                span.label = "target";
            }
            merged.push_back(std::move(span));
        }

        if (!merged.empty()) {
            return merged;
        }
    } catch (const std::exception& ex) {
        error_out = ex.what();
    }
#else
    (void)targets_json;
#endif

    return parsed;
}

}  // namespace tui_debug_ui

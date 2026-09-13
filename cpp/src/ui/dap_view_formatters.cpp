#include "tui_debug_ui/dap_view_formatters.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <sstream>
#include <string>

namespace tui_debug_ui {

namespace {

using Json = nlohmann::json;

std::string ascii_column(const std::string& hex_row) {
    std::string out;
    out.reserve(hex_row.size() / 2);
    for (std::size_t i = 0; i + 1 < hex_row.size(); i += 2) {
        const std::string pair = hex_row.substr(i, 2);
        const int value = std::stoi(pair, nullptr, 16);
        out.push_back(value >= 32 && value <= 126 ? static_cast<char>(value) : '.');
    }
    return out;
}

std::string format_hex_groups(const std::string& hex_row) {
    std::ostringstream out;
    for (std::size_t i = 0; i < hex_row.size(); i += 2) {
        if (i > 0) {
            out << (i % 4 == 0 ? "  " : " ");
        }
        out << hex_row.substr(i, 2);
    }
    return out.str();
}

std::uint64_t parse_address_offset(const std::string& address, std::int64_t row_index,
                                   std::int64_t bytes_per_row) {
    if (address.empty()) {
        return static_cast<std::uint64_t>(row_index * bytes_per_row);
    }
    std::string cleaned = address;
    if (cleaned.rfind("0x", 0) == 0 || cleaned.rfind("0X", 0) == 0) {
        cleaned = cleaned.substr(2);
    }
    try {
        const std::uint64_t base = std::stoull(cleaned, nullptr, 16);
        return base + static_cast<std::uint64_t>(row_index * bytes_per_row);
    } catch (...) {
        return static_cast<std::uint64_t>(row_index * bytes_per_row);
    }
}

}  // namespace

std::vector<std::string> format_memory_hex_dump(const std::string& address, const std::string& hex_data,
                                                std::int64_t bytes_per_row) {
    std::vector<std::string> lines;
    if (hex_data.empty()) {
        lines.push_back("(no readable memory)");
        return lines;
    }

    const std::int64_t row_size = bytes_per_row > 0 ? bytes_per_row : 16;
    for (std::size_t offset = 0; offset < hex_data.size(); offset += static_cast<std::size_t>(row_size * 2)) {
        const std::string row_hex = hex_data.substr(offset, static_cast<std::size_t>(row_size * 2));
        const std::uint64_t row_address =
            parse_address_offset(address, static_cast<std::int64_t>(offset / (row_size * 2)), row_size);
        std::ostringstream line;
        line << "0x" << std::hex << row_address << "  " << format_hex_groups(row_hex) << "  |" << ascii_column(row_hex)
             << '|';
        lines.push_back(line.str());
    }
    return lines;
}

std::vector<std::string> format_disassembly_lines(const std::string& json, DisassemblyLineStyle style) {
    std::vector<std::string> lines;
    const Json parsed = Json::parse(json, nullptr, false);
    if (!parsed.is_array() || parsed.empty()) {
        lines.push_back("(no instructions)");
        return lines;
    }

    for (const Json& item : parsed) {
        if (!item.is_object()) {
            continue;
        }
        const std::string address = item.value("address", std::string{});
        const std::string instruction = item.value("instruction", std::string{});
        const std::string bytes = item.value("instructionBytes", std::string{});
        const std::string symbol = item.value("symbol", std::string{});

        std::ostringstream line;
        if (!address.empty()) {
            line << address << "  ";
        }
        if (style == DisassemblyLineStyle::Bytes) {
            if (!bytes.empty()) {
                line << bytes;
            } else if (!instruction.empty()) {
                line << instruction;
            }
        } else {
            if (!instruction.empty()) {
                line << instruction;
            } else if (!bytes.empty()) {
                line << bytes;
            }
        }
        if (!symbol.empty()) {
            line << "  <" << symbol << '>';
        }
        if (item.contains("line") && item.at("line").is_number_integer()) {
            line << "  ; line " << item.at("line").get<std::int64_t>();
        }
        if (!line.str().empty()) {
            lines.push_back(line.str());
        }
    }

    if (lines.empty()) {
        lines.push_back("(no instructions)");
    }
    return lines;
}

std::vector<std::string> format_runtime_source_lines(const std::string& source) {
    std::vector<std::string> lines;
    if (source.empty()) {
        lines.push_back("(empty source)");
        return lines;
    }

    std::size_t start = 0;
    std::uint32_t line_no = 1;
    while (start <= source.size()) {
        std::size_t end = source.find('\n', start);
        std::string text;
        if (end == std::string::npos) {
            text = source.substr(start);
            start = source.size() + 1;
        } else {
            text = source.substr(start, end - start);
            start = end + 1;
        }
        if (!text.empty() && text.back() == '\r') {
            text.pop_back();
        }
        lines.push_back(std::to_string(line_no) + "  " + text);
        ++line_no;
        if (end == std::string::npos) {
            break;
        }
    }
    return lines;
}

bool is_memory_dump_row(const std::string& line) {
    return line.rfind("0x", 0) == 0 && line.find("  |") != std::string::npos;
}

std::optional<std::string> extract_memory_row_hex(const std::string& line) {
    if (!is_memory_dump_row(line)) {
        return std::nullopt;
    }

    const std::size_t ascii_column = line.find("  |");
    const std::size_t hex_start = line.find("  ", 2);
    if (ascii_column == std::string::npos || hex_start == std::string::npos || hex_start >= ascii_column) {
        return std::nullopt;
    }

    std::string hex;
    for (char ch : line.substr(hex_start + 2, ascii_column - hex_start - 2)) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (hex.empty() || hex.size() % 2 != 0) {
        return std::nullopt;
    }
    return hex;
}

std::optional<std::uint64_t> parse_memory_row_address(const std::string& line) {
    if (!is_memory_dump_row(line)) {
        return std::nullopt;
    }

    const std::size_t space = line.find(' ');
    if (space == std::string::npos || space <= 2) {
        return std::nullopt;
    }

    try {
        return std::stoull(line.substr(0, space), nullptr, 0);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string> parse_memory_address_reference(const std::string& input) {
    std::string trimmed;
    for (char ch : input) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            trimmed.push_back(ch);
        }
    }
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::string hex = trimmed;
    if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) {
        hex = hex.substr(2);
    }

    if (hex.empty()) {
        return std::nullopt;
    }

    for (char ch : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
    }

    return "0x" + hex;
}

std::optional<std::string> extract_address_from_eval_result(const std::string& result) {
    std::string trimmed;
    for (char ch : result) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            trimmed.push_back(ch);
        }
    }
    if (trimmed.empty()) {
        return std::nullopt;
    }

    const std::size_t hex_pos = trimmed.find("0x");
    if (hex_pos == std::string::npos) {
        return std::nullopt;
    }

    std::string hex;
    for (std::size_t i = hex_pos + 2; i < trimmed.size(); ++i) {
        const char ch = trimmed[i];
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            continue;
        }
        if (!hex.empty()) {
            break;
        }
    }

    if (hex.empty()) {
        return std::nullopt;
    }
    return "0x" + hex;
}

std::vector<std::size_t> find_memory_search_matches(const std::string& hex_data, const std::string& query) {
    std::vector<std::size_t> matches;
    if (hex_data.empty() || query.empty()) {
        return matches;
    }

    std::string query_hex;
    bool query_is_hex = true;
    for (char ch : query) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            query_hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            query_is_hex = false;
            break;
        }
    }

    if (query_is_hex && !query_hex.empty()) {
        if (query_hex.size() % 2 != 0) {
            query_hex.insert(0, "0");
        }
        std::size_t pos = 0;
        while ((pos = hex_data.find(query_hex, pos)) != std::string::npos) {
            matches.push_back(pos / 2);
            ++pos;
        }
        return matches;
    }

    std::string needle;
    for (unsigned char ch : query) {
        if (std::isspace(ch)) {
            continue;
        }
        needle.push_back(static_cast<char>(ch));
    }
    if (needle.empty() || hex_data.size() < needle.size() * 2) {
        return matches;
    }

    for (std::size_t byte_offset = 0; byte_offset + needle.size() <= hex_data.size() / 2; ++byte_offset) {
        bool found = true;
        for (std::size_t i = 0; i < needle.size(); ++i) {
            const std::size_t hex_index = (byte_offset + i) * 2;
            const std::string pair = hex_data.substr(hex_index, 2);
            const int value = std::stoi(pair, nullptr, 16);
            if (static_cast<unsigned char>(value) != static_cast<unsigned char>(needle[i])) {
                found = false;
                break;
            }
        }
        if (found) {
            matches.push_back(byte_offset);
        }
    }
    return matches;
}

std::string format_hex_for_edit(const std::string& compact_hex) {
    std::ostringstream out;
    for (std::size_t i = 0; i < compact_hex.size(); i += 2) {
        if (i > 0) {
            out << ((i % 16 == 0) ? "  " : " ");
        }
        out << compact_hex.substr(i, 2);
    }
    return out.str();
}

std::optional<std::string> normalize_hex_input(const std::string& input) {
    std::string hex;
    for (char ch : input) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (hex.empty() || hex.size() % 2 != 0) {
        return std::nullopt;
    }
    return hex;
}

}  // namespace tui_debug_ui

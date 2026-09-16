#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tui_debug_ui {

[[nodiscard]] std::vector<std::string> format_memory_hex_dump(const std::string& address, const std::string& hex_data,
                                                              std::int64_t bytes_per_row = 16);

enum class DisassemblyLineStyle {
    Asm,
    Bytes,
};

[[nodiscard]] std::vector<std::string> format_disassembly_lines(const std::string& json,
                                                                DisassemblyLineStyle style);

[[nodiscard]] std::vector<std::string> format_runtime_source_lines(const std::string& source);

[[nodiscard]] bool is_memory_dump_row(const std::string& line);
[[nodiscard]] std::optional<std::string> extract_memory_row_hex(const std::string& line);
[[nodiscard]] std::optional<std::uint64_t> parse_memory_row_address(const std::string& line);
[[nodiscard]] std::optional<std::string> parse_memory_address_reference(const std::string& input);
[[nodiscard]] std::optional<std::uint64_t> parse_memory_address_u64(const std::string& input);
[[nodiscard]] std::string format_memory_address(std::uint64_t address);
[[nodiscard]] std::optional<std::string> extract_address_from_eval_result(const std::string& result);
[[nodiscard]] std::vector<std::size_t> find_memory_search_matches(const std::string& hex_data,
                                                                  const std::string& query);
[[nodiscard]] std::size_t memory_search_match_byte_length(const std::string& query);
struct MemoryDumpRowLayout {
    int hex_region_start = 0;
    int ascii_region_start = 0;
};
[[nodiscard]] std::optional<MemoryDumpRowLayout> memory_dump_row_layout(const std::string& line);
[[nodiscard]] int memory_dump_hex_column(int byte_in_row);
[[nodiscard]] int memory_dump_hex_width(int byte_count);
[[nodiscard]] std::string format_hex_for_edit(const std::string& compact_hex);
[[nodiscard]] std::optional<std::string> normalize_hex_input(const std::string& input);

}  // namespace tui_debug_ui

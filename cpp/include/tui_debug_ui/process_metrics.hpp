#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace tui_debug_ui {

struct ProcessMetricsSample {
    double system_cpu_percent = 0.0;
    double debuggee_cpu_percent = 0.0;
    std::uint64_t system_mem_used_bytes = 0;
    std::uint64_t system_mem_total_bytes = 0;
    std::uint64_t debuggee_mem_bytes = 0;
    int tracked_pids = 0;
    bool valid = false;
};

/// Samples system and debuggee CPU/RAM from `/proc` for tracked OS PIDs (aggregated).
class ProcessMetricsSampler {
  public:
    static constexpr std::size_t kHistorySize = 48;

    [[nodiscard]] bool tick(const std::vector<std::uint32_t>& pids);
    [[nodiscard]] const ProcessMetricsSample& latest() const { return latest_; }
    [[nodiscard]] const std::deque<double>& system_cpu_history() const { return system_cpu_history_; }
    [[nodiscard]] const std::deque<double>& debuggee_cpu_history() const { return debuggee_cpu_history_; }
    [[nodiscard]] const std::deque<double>& system_mem_history() const { return system_mem_history_; }
    [[nodiscard]] const std::deque<double>& debuggee_mem_history() const { return debuggee_mem_history_; }
    void reset();

  private:
    struct CpuCounter {
        std::uint64_t total = 0;
        std::uint64_t idle = 0;
    };

    struct ProcessCounter {
        std::uint32_t pid = 0;
        std::uint64_t cpu_ticks = 0;
    };

    void push_history(std::deque<double>& history, double value);
    [[nodiscard]] std::optional<CpuCounter> read_system_cpu() const;
    [[nodiscard]] std::optional<std::uint64_t> read_system_mem() const;
    [[nodiscard]] std::optional<std::uint64_t> read_system_mem_total() const;
    [[nodiscard]] std::optional<ProcessCounter> read_process_counter(std::uint32_t pid) const;
    [[nodiscard]] std::optional<std::uint64_t> read_process_rss(std::uint32_t pid) const;

    bool has_previous_ = false;
    CpuCounter previous_system_cpu_{};
    std::vector<ProcessCounter> previous_process_cpu_{};
    std::chrono::steady_clock::time_point previous_sample_{};
    ProcessMetricsSample latest_{};
    std::deque<double> system_cpu_history_;
    std::deque<double> debuggee_cpu_history_;
    std::deque<double> system_mem_history_;
    std::deque<double> debuggee_mem_history_;
};

[[nodiscard]] std::string format_byte_size(std::uint64_t bytes);
[[nodiscard]] std::string format_percent(double value);
[[nodiscard]] bool is_process_alive(std::uint32_t pid);

}  // namespace tui_debug_ui

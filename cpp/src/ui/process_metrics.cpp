#include "tui_debug_ui/process_metrics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace tui_debug_ui {
namespace {

std::string read_file_to_string(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::uint64_t parse_uint64_field(const std::string& text, const char* key) {
    const std::string needle = std::string(key) + ":";
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos) {
        return 0;
    }
    std::size_t start = pos + needle.size();
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::uint64_t value = 0;
    while (start < text.size() && std::isdigit(static_cast<unsigned char>(text[start])) != 0) {
        value = value * 10 + static_cast<std::uint64_t>(text[start] - '0');
        ++start;
    }
    return value;
}

}  // namespace

void ProcessMetricsSampler::reset() {
    has_previous_ = false;
    previous_system_cpu_ = {};
    previous_process_cpu_.clear();
    latest_ = {};
    system_cpu_history_.clear();
    debuggee_cpu_history_.clear();
    system_mem_history_.clear();
    debuggee_mem_history_.clear();
}

void ProcessMetricsSampler::push_history(std::deque<double>& history, double value) {
    history.push_back(value);
    while (history.size() > kHistorySize) {
        history.pop_front();
    }
}

std::optional<ProcessMetricsSampler::CpuCounter> ProcessMetricsSampler::read_system_cpu() const {
    const std::string stat = read_file_to_string("/proc/stat");
    if (stat.empty()) {
        return std::nullopt;
    }
    const std::size_t line_end = stat.find('\n');
    const std::string line = stat.substr(0, line_end);
    if (line.rfind("cpu ", 0) != 0) {
        return std::nullopt;
    }
    std::istringstream input(line.substr(4));
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    input >> user >> nice >> system >> idle;
    if (!input) {
        return std::nullopt;
    }
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;
    input >> iowait >> irq >> softirq >> steal;
    CpuCounter counter;
    counter.total = user + nice + system + idle + iowait + irq + softirq + steal;
    counter.idle = idle + iowait;
    return counter;
}

std::optional<std::uint64_t> ProcessMetricsSampler::read_system_mem_total() const {
    const std::string meminfo = read_file_to_string("/proc/meminfo");
    if (meminfo.empty()) {
        return std::nullopt;
    }
    const std::uint64_t total_kb = parse_uint64_field(meminfo, "MemTotal");
    if (total_kb == 0) {
        return std::nullopt;
    }
    return total_kb * 1024;
}

std::optional<std::uint64_t> ProcessMetricsSampler::read_system_mem() const {
    const std::string meminfo = read_file_to_string("/proc/meminfo");
    if (meminfo.empty()) {
        return std::nullopt;
    }
    const std::uint64_t total_kb = parse_uint64_field(meminfo, "MemTotal");
    const std::uint64_t available_kb = parse_uint64_field(meminfo, "MemAvailable");
    if (total_kb == 0) {
        return std::nullopt;
    }
    if (available_kb > 0 && available_kb <= total_kb) {
        return (total_kb - available_kb) * 1024;
    }
    const std::uint64_t free_kb = parse_uint64_field(meminfo, "MemFree");
    return (total_kb - free_kb) * 1024;
}

std::optional<ProcessMetricsSampler::ProcessCounter> ProcessMetricsSampler::read_process_counter(
    std::uint32_t pid) const {
    const std::string stat = read_file_to_string("/proc/" + std::to_string(pid) + "/stat");
    if (stat.empty()) {
        return std::nullopt;
    }
    const std::size_t close_paren = stat.rfind(')');
    if (close_paren == std::string::npos || close_paren + 2 >= stat.size()) {
        return std::nullopt;
    }
    // Fields after `comm)`: state is a single letter, then numeric fields; utime/stime are #14/#15.
    std::istringstream input(stat.substr(close_paren + 2));
    char state = '\0';
    input >> state;
    if (!input) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    std::uint64_t utime = 0;
    std::uint64_t stime = 0;
    for (int field = 0; field < 12; ++field) {
        input >> value;
        if (!input) {
            return std::nullopt;
        }
        if (field == 10) {
            utime = value;
        } else if (field == 11) {
            stime = value;
        }
    }
    ProcessCounter counter;
    counter.pid = pid;
    counter.cpu_ticks = utime + stime;
    return counter;
}

std::optional<std::uint64_t> ProcessMetricsSampler::read_process_rss(std::uint32_t pid) const {
    const std::string status = read_file_to_string("/proc/" + std::to_string(pid) + "/status");
    if (status.empty()) {
        return std::nullopt;
    }
    const std::uint64_t rss_kb = parse_uint64_field(status, "VmRSS");
    return rss_kb * 1024;
}

bool is_process_alive(std::uint32_t pid) {
    return access(("/proc/" + std::to_string(pid)).c_str(), F_OK) == 0;
}

bool ProcessMetricsSampler::tick(const std::vector<std::uint32_t>& pids) {
    const auto now = std::chrono::steady_clock::now();
    const auto system_cpu = read_system_cpu();
    const auto system_mem_used = read_system_mem();
    const auto system_mem_total = read_system_mem_total();

    std::vector<ProcessCounter> process_counters;
    process_counters.reserve(pids.size());
    std::uint64_t debuggee_rss = 0;
    int alive_pids = 0;
    for (const std::uint32_t pid : pids) {
        if (!is_process_alive(pid)) {
            continue;
        }
        ++alive_pids;
        if (const auto counter = read_process_counter(pid)) {
            process_counters.push_back(*counter);
        }
        if (const auto rss = read_process_rss(pid)) {
            debuggee_rss += *rss;
        }
    }

    ProcessMetricsSample sample;
    sample.tracked_pids = alive_pids;
    if (system_mem_used.has_value()) {
        sample.system_mem_used_bytes = *system_mem_used;
    }
    if (system_mem_total.has_value()) {
        sample.system_mem_total_bytes = *system_mem_total;
    }
    sample.debuggee_mem_bytes = debuggee_rss;

    if (has_previous_ && system_cpu.has_value()) {
        const double elapsed =
            std::chrono::duration<double>(now - previous_sample_).count();
        if (elapsed > 0.0) {
            const std::uint64_t total_delta = system_cpu->total - previous_system_cpu_.total;
            if (total_delta > 0) {
                const double busy_fraction =
                    static_cast<double>(total_delta - (system_cpu->idle - previous_system_cpu_.idle)) /
                    static_cast<double>(total_delta);
                sample.system_cpu_percent = std::clamp(busy_fraction * 100.0, 0.0, 100.0);
            }

            std::uint64_t process_delta = 0;
            for (const ProcessCounter& counter : process_counters) {
                for (const ProcessCounter& previous : previous_process_cpu_) {
                    if (previous.pid == counter.pid) {
                        process_delta += counter.cpu_ticks - previous.cpu_ticks;
                        break;
                    }
                }
            }
            const long clock_ticks = sysconf(_SC_CLK_TCK);
            if (clock_ticks > 0 && process_delta > 0) {
                const double cpu_seconds = static_cast<double>(process_delta) / static_cast<double>(clock_ticks);
                sample.debuggee_cpu_percent = std::clamp((cpu_seconds / elapsed) * 100.0, 0.0, 10000.0);
            }
        }
    }

    sample.valid = system_mem_total.has_value() || sample.tracked_pids > 0;
    latest_ = sample;
    has_previous_ = true;
    previous_sample_ = now;
    if (system_cpu.has_value()) {
        previous_system_cpu_ = *system_cpu;
    }
    previous_process_cpu_ = std::move(process_counters);

    if (sample.system_mem_total_bytes > 0) {
        push_history(system_mem_history_,
                     100.0 * static_cast<double>(sample.system_mem_used_bytes) /
                         static_cast<double>(sample.system_mem_total_bytes));
    }
    push_history(system_cpu_history_, sample.system_cpu_percent);
    push_history(debuggee_cpu_history_, sample.debuggee_cpu_percent);
    if (sample.system_mem_total_bytes > 0) {
        push_history(debuggee_mem_history_,
                     100.0 * static_cast<double>(sample.debuggee_mem_bytes) /
                         static_cast<double>(sample.system_mem_total_bytes));
    }
    return sample.valid;
}

std::string format_byte_size(std::uint64_t bytes) {
    if (bytes >= 1'000'000'000) {
        return std::to_string(bytes / 1'000'000'000) + "." +
               std::to_string((bytes % 1'000'000'000) / 100'000'000) + " GB";
    }
    if (bytes >= 1'000'000) {
        return std::to_string(bytes / 1'000'000) + "." + std::to_string((bytes % 1'000'000) / 100'000) +
               " MB";
    }
    if (bytes >= 1'000) {
        return std::to_string(bytes / 1'000) + " KB";
    }
    return std::to_string(bytes) + " B";
}

std::string format_percent(double value) {
    if (value < 0.05) {
        return "0.0%";
    }
    const int whole = static_cast<int>(value);
    const int tenth = static_cast<int>((value - static_cast<double>(whole)) * 10.0);
    return std::to_string(whole) + "." + std::to_string(tenth) + "%";
}

}  // namespace tui_debug_ui

#include "tui_debug_ui/app.hpp"
#include "tui_debug_ui/session_backend.hpp"
#include "tui_debug_ui/tty_setup.hpp"

extern "C" {
#include "tui_debug.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

bool looks_like_elf_executable(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    char magic[4] = {};
    input.read(magic, 4);
    return input.gcount() == 4 && magic[0] == '\x7f' && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

bool executable_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    return access(path.c_str(), X_OK) == 0 || looks_like_elf_executable(path);
}

std::string strip_source_extension(const std::string& path) {
    for (const char* extension : {".c", ".cpp", ".cc", ".cxx", ".rs"}) {
        const std::size_t ext_len = std::strlen(extension);
        if (path.size() > ext_len && path.compare(path.size() - ext_len, ext_len, extension) == 0) {
            return path.substr(0, path.size() - ext_len);
        }
    }
    return path;
}

bool command_on_path(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return false;
    }
    std::string prefix;
    for (const char* cursor = path_env;; ++cursor) {
        if (*cursor == ':' || *cursor == '\0') {
            if (!prefix.empty()) {
                const std::string candidate = prefix + "/" + name;
                if (access(candidate.c_str(), X_OK) == 0) {
                    return true;
                }
            } else if (access(name, X_OK) == 0) {
                return true;
            }
            prefix.clear();
            if (*cursor == '\0') {
                break;
            }
            continue;
        }
        prefix.push_back(*cursor);
    }
    return false;
}

std::string resolve_native_launch_path(const std::string& program_path) {
    if (looks_like_elf_executable(program_path)) {
        return program_path;
    }

    const std::string candidate = strip_source_extension(program_path);
    if (candidate != program_path && executable_exists(candidate)) {
        return candidate;
    }
    return program_path;
}

void print_usage(const char* argv0) {
    std::fprintf(stderr, "Usage: %s [--mock] [--lldb] [--rr] <program> [program-args...]\n", argv0);
    std::fprintf(stderr, "  --mock   Frontend-only mode (no Rust/DAP backend)\n");
    std::fprintf(stderr, "  --lldb   Force lldb-dap for native binaries\n");
    std::fprintf(stderr, "  --rr     Use rr record+replay (reverse debugging, Linux)\n");
    std::fprintf(stderr, "  Python:  %s fixtures/step_in_demo.py\n", argv0);
    std::fprintf(stderr, "  C/C++:   %s fixtures/reverse_demo\n", argv0);
    std::fprintf(stderr, "  C++ ex:  %s fixtures/exception_demo 2  (build: fixtures/build-exception-demo.sh)\n", argv0);
    std::fprintf(stderr, "           (native ELF binaries auto-select lldb-dap)\n");
    std::fprintf(stderr, "  Reverse: %s --rr fixtures/reverse_demo\n", argv0);
    std::fprintf(stderr, "tui-debug-ui 0.1.0\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    tui_debug_ui::SessionMode mode = tui_debug_ui::SessionMode::Rust;
    tui_debug_ui::DebugAdapter adapter = tui_debug_ui::DebugAdapter::Debugpy;
    const char* program_path = nullptr;
    std::vector<std::string> program_args;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mock") == 0) {
            mode = tui_debug_ui::SessionMode::Mock;
            continue;
        }
        if (std::strcmp(argv[i], "--lldb") == 0) {
            adapter = tui_debug_ui::DebugAdapter::Lldb;
            continue;
        }
        if (std::strcmp(argv[i], "--rr") == 0) {
            adapter = tui_debug_ui::DebugAdapter::Rr;
            continue;
        }
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (program_path == nullptr) {
            program_path = argv[i];
            continue;
        }
        program_args.push_back(argv[i]);
    }

    if (program_path == nullptr) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!isatty(STDIN_FILENO)) {
        std::fprintf(stderr,
                     "tui-debug-ui requires an interactive terminal.\n"
                     "Run it directly in a TTY, not via a pipe or background job.\n");
        return EXIT_FAILURE;
    }

    std::string launch_path = resolve_native_launch_path(program_path);
    if (mode == tui_debug_ui::SessionMode::Rust && adapter == tui_debug_ui::DebugAdapter::Debugpy &&
        looks_like_elf_executable(launch_path)) {
        adapter = tui_debug_ui::DebugAdapter::Lldb;
    }

    if (mode == tui_debug_ui::SessionMode::Rust &&
        (adapter == tui_debug_ui::DebugAdapter::Lldb || adapter == tui_debug_ui::DebugAdapter::Rr) &&
        !looks_like_elf_executable(launch_path)) {
        std::fprintf(stderr,
                     "Native debug adapters require a built executable, not a source file.\n"
                     "Build first, e.g.: gcc -g -O0 -o fixtures/reverse_demo fixtures/reverse_demo.c\n");
        return EXIT_FAILURE;
    }

    if (mode == tui_debug_ui::SessionMode::Rust && adapter == tui_debug_ui::DebugAdapter::Rr) {
        if (!command_on_path("rr")) {
            std::fprintf(stderr,
                         "rr not found in PATH — reverse debugging requires rr.\n"
                         "Install on Fedora: sudo dnf install rr\n"
                         "Then run: %s --rr %s\n",
                         argv[0], program_path);
            return EXIT_FAILURE;
        }
        if (!command_on_path("gdb")) {
            std::fprintf(stderr,
                         "gdb not found in PATH — rr replay debugging requires gdb.\n"
                         "Install on Fedora: sudo dnf install gdb\n");
            return EXIT_FAILURE;
        }
    }

    tui_debug_ui::ignore_job_control_tty_signals();
    tui_debug_ui::ignore_sigpipe();
    tui_debug_ui::claim_terminal_for_ui();
    tui_debug_ui::install_sigint_quit_handler();
    tui_debug_ui::sync_terminal_size_from_tty();

    if (mode == tui_debug_ui::SessionMode::Rust) {
        tui_debug_init();
    }

    tui_debug_ui::DebugApp app(launch_path, mode, adapter, std::move(program_args));
    return app.run();
}

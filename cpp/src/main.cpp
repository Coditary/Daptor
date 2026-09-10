#include "tui_debug_ui/app.hpp"
#include "tui_debug_ui/session_backend.hpp"
#include "tui_debug_ui/tty_setup.hpp"

extern "C" {
#include "tui_debug.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr, "Usage: %s [--mock] <program.py>\n", argv0);
    std::fprintf(stderr, "  --mock   Frontend-only mode (no Rust/DAP backend)\n");
    std::fprintf(stderr, "tui-debug-ui 0.1.0\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    tui_debug_ui::SessionMode mode = tui_debug_ui::SessionMode::Rust;
    const char* program_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mock") == 0) {
            mode = tui_debug_ui::SessionMode::Mock;
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
        std::fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
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

    tui_debug_ui::ignore_job_control_tty_signals();
    tui_debug_ui::ignore_sigpipe();
    tui_debug_ui::claim_terminal_for_ui();
    tui_debug_ui::install_sigint_quit_handler();
    tui_debug_ui::sync_terminal_size_from_tty();

    if (mode == tui_debug_ui::SessionMode::Rust) {
        tui_debug_init();
    }

    tui_debug_ui::DebugApp app(program_path, mode);
    return app.run();
}

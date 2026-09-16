#include "tui_debug_ui/app.hpp"
#include "tui_debug_ui/app_config.hpp"
#include "tui_debug_ui/launch_plan.hpp"
#include "tui_debug_ui/tty_setup.hpp"

extern "C" {
#include "tui_debug.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr, "Usage: %s [options] <target> [program-args...]\n", argv0);
    std::fprintf(stderr, "  --mock              Frontend-only mode (no Rust/DAP backend)\n");
    std::fprintf(stderr, "  --profile <name>    Launch profile from definitions.yaml\n");
    std::fprintf(stderr, "  --adapter <name>    Force adapter from definitions.yaml\n");
    std::fprintf(stderr, "  --binary <path>     Override resolved binary path\n");
    std::fprintf(stderr, "  --workspace <path>  Override workspace root\n");
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "Launch adapters and profiles are configured in definitions.yaml.\n");
    std::fprintf(stderr, "See config/definitions.yaml in the repository for a starter template.\n");
    std::fprintf(stderr, "daptor 0.1.0\n");
}

}  // namespace

int main(int argc, char* argv[]) {
    tui_debug_ui::SessionMode mode = tui_debug_ui::SessionMode::Rust;
    std::optional<std::string> profile;
    std::optional<std::string> adapter;
    std::optional<std::filesystem::path> binary_override;
    std::optional<std::filesystem::path> workspace_override;
    const char* target_path = nullptr;
    std::vector<std::string> program_args;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mock") == 0) {
            mode = tui_debug_ui::SessionMode::Mock;
            continue;
        }
        if (std::strcmp(argv[i], "--profile") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--profile requires a value\n");
                return EXIT_FAILURE;
            }
            profile = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--adapter") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--adapter requires a value\n");
                return EXIT_FAILURE;
            }
            adapter = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--binary") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--binary requires a value\n");
                return EXIT_FAILURE;
            }
            binary_override = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--workspace") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--workspace requires a value\n");
                return EXIT_FAILURE;
            }
            workspace_override = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (target_path == nullptr) {
            target_path = argv[i];
            continue;
        }
        program_args.push_back(argv[i]);
    }

    if (target_path == nullptr) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!isatty(STDIN_FILENO)) {
        std::fprintf(stderr,
                     "daptor requires an interactive terminal.\n"
                     "Run it directly in a TTY, not via a pipe or background job.\n");
        return EXIT_FAILURE;
    }

    const tui_debug_ui::AppConfig app_config = tui_debug_ui::load_app_config();

    tui_debug_ui::LaunchRequest launch_request{
        .target = target_path,
        .args = program_args,
        .profile = profile,
        .adapter = adapter,
        .binary = binary_override,
        .workspace = workspace_override,
    };

    std::string resolve_error;
    std::optional<tui_debug_ui::LaunchPlan> launch_plan;
    if (mode == tui_debug_ui::SessionMode::Rust) {
        launch_plan = tui_debug_ui::resolve_launch_plan(app_config.config_path, launch_request, resolve_error);
        if (!launch_plan.has_value()) {
            std::fprintf(stderr, "%s\n", resolve_error.c_str());
            return EXIT_FAILURE;
        }
    } else {
        tui_debug_ui::LaunchPlan mock_plan{};
        mock_plan.program_path = target_path;
        mock_plan.target_path = target_path;
        launch_plan = mock_plan;
    }

    tui_debug_ui::ignore_job_control_tty_signals();
    tui_debug_ui::ignore_sigpipe();
    tui_debug_ui::claim_terminal_for_ui();
    tui_debug_ui::install_sigint_quit_handler();
    tui_debug_ui::sync_terminal_size_from_tty();

    if (mode == tui_debug_ui::SessionMode::Rust) {
        if (app_config.paths.tree_sitter_dir.has_value()) {
            tui_debug_set_tree_sitter_dir(app_config.paths.tree_sitter_dir->c_str());
        }
        tui_debug_init();
    }

    tui_debug_ui::DebugApp app(
        launch_plan->program_path, mode, launch_plan->ui, std::move(program_args), app_config,
        launch_plan->resolved_json.empty() ? std::nullopt : std::optional<std::string>(launch_plan->resolved_json),
        launch_plan->workspace, launch_plan->display_source);
    return app.run();
}

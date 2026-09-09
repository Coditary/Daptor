#include "tui_debug_ui/tty_setup.hpp"

#include <ncursesw/curses.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if !defined(_WIN32)
#include <atomic>
#endif

namespace tui_debug_ui {
namespace {

bool query_winsize(struct winsize& ws) {
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        return true;
    }

    const int tty_fd = open("/dev/tty", O_RDONLY);
    if (tty_fd < 0) {
        return false;
    }

    const bool ok = ioctl(tty_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0;
    close(tty_fd);
    return ok;
}

void set_env_dimension(const char* name, unsigned short value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%u", value);
    setenv(name, buffer, 1);
}

}  // namespace

#if !defined(_WIN32)
std::atomic<bool> g_sigint_quit_requested{false};

void on_sigint(int /*signo*/) {
    g_sigint_quit_requested.store(true, std::memory_order_relaxed);
}
#endif

void ignore_job_control_tty_signals() {
    std::signal(SIGTTIN, SIG_IGN);
    std::signal(SIGTTOU, SIG_IGN);
}

void ignore_sigpipe() {
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

void claim_terminal_for_ui() {
#if !defined(_WIN32)
    if (setpgid(0, 0) != 0 && errno != EPERM) {
        // Already a process-group leader on some shells — continue.
    }
    const pid_t pg = getpgrp();
    if (tcgetpgrp(STDIN_FILENO) != pg) {
        tcsetpgrp(STDIN_FILENO, pg);
    }
#endif
}

void reclaim_terminal_for_ui() {
#if !defined(_WIN32)
    const pid_t pg = getpgrp();
    if (tcgetpgrp(STDIN_FILENO) != pg) {
        tcsetpgrp(STDIN_FILENO, pg);
    }
#endif
}

void install_sigint_quit_handler() {
#if !defined(_WIN32)
    struct sigaction action {};
    action.sa_handler = on_sigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
#endif
}

bool consume_sigint_quit_request() {
#if !defined(_WIN32)
    return g_sigint_quit_requested.exchange(false, std::memory_order_relaxed);
#else
    return false;
#endif
}

void sync_terminal_size_from_tty() {
    struct winsize ws {};
    if (!query_winsize(ws)) {
        unsetenv("LINES");
        unsetenv("COLUMNS");
        return;
    }

    set_env_dimension("LINES", ws.ws_row);
    set_env_dimension("COLUMNS", ws.ws_col);
}

void apply_ncurses_winsize() {
    struct winsize ws {};
    if (!query_winsize(ws)) {
        return;
    }

    resizeterm(static_cast<int>(ws.ws_row), static_cast<int>(ws.ws_col));
}

void set_terminal_theme_background(tuinator::Rgb background) {
    char sequence[48];
    std::snprintf(sequence, sizeof(sequence), "\033]11;#%02x%02x%02x\007", background.r, background.g, background.b);

    const int tty_fd = open("/dev/tty", O_WRONLY);
    if (tty_fd < 0) {
        return;
    }

    (void)write(tty_fd, sequence, std::strlen(sequence));
    close(tty_fd);
}

}  // namespace tui_debug_ui

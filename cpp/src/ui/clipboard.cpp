#include "tui_debug_ui/clipboard.hpp"

#include <unistd.h>

#include <array>
#include <cstdio>
#include <string>

namespace tui_debug_ui {
namespace {

std::string base64_encode(const std::string& input) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 2 < input.size()) {
        const unsigned value = (static_cast<unsigned char>(input[i]) << 16) |
                               (static_cast<unsigned char>(input[i + 1]) << 8) |
                               static_cast<unsigned char>(input[i + 2]);
        out.push_back(kTable[(value >> 18) & 0x3F]);
        out.push_back(kTable[(value >> 12) & 0x3F]);
        out.push_back(kTable[(value >> 6) & 0x3F]);
        out.push_back(kTable[value & 0x3F]);
        i += 3;
    }

    if (i < input.size()) {
        unsigned value = static_cast<unsigned char>(input[i]) << 16;
        out.push_back(kTable[(value >> 18) & 0x3F]);
        if (i + 1 < input.size()) {
            value |= static_cast<unsigned char>(input[i + 1]) << 8;
            out.push_back(kTable[(value >> 12) & 0x3F]);
            out.push_back(kTable[(value >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back(kTable[(value >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

bool copy_via_osc52(const std::string& text) {
    const std::string seq = "\033]52;c;" + base64_encode(text) + "\033\\";
    return write(STDOUT_FILENO, seq.data(), seq.size()) == static_cast<ssize_t>(seq.size());
}

bool copy_via_command(const std::string& text, const char* command) {
    FILE* pipe = popen(command, "w");
    if (pipe == nullptr) {
        return false;
    }
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), pipe);
    const int status = pclose(pipe);
    return written == text.size() && status == 0;
}

}  // namespace

bool copy_to_clipboard(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    if (copy_via_osc52(text)) {
        return true;
    }
    if (copy_via_command(text, "wl-copy -n 2>/dev/null")) {
        return true;
    }
    if (copy_via_command(text, "xclip -selection clipboard 2>/dev/null")) {
        return true;
    }
    return false;
}

}  // namespace tui_debug_ui

#include "tui_debug_ui/session_backend.hpp"

namespace tui_debug_ui {

std::unique_ptr<SessionBackend> create_mock_session_backend();
std::unique_ptr<SessionBackend> create_rust_session_backend();

std::unique_ptr<SessionBackend> create_session_backend(SessionMode mode) {
    if (mode == SessionMode::Mock) {
        return create_mock_session_backend();
    }
    return create_rust_session_backend();
}

}  // namespace tui_debug_ui

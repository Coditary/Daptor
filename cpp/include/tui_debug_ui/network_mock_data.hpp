#pragma once

#include <string>
#include <vector>

namespace tui_debug_ui {

enum class NetworkExchangeState {
    Completed,
    Pending,
    Dropped,
};

/// Where a traffic row came from — shown in the list so it is not confused with debuggee traffic.
enum class NetworkExchangeOrigin {
    Captured,
    Compose,
    Demo,
};

struct NetworkExchange {
    std::string id;
    NetworkExchangeState state = NetworkExchangeState::Completed;
    NetworkExchangeOrigin origin = NetworkExchangeOrigin::Captured;
    std::string method;
    int status_code = 0;
    int duration_ms = 0;
    std::string path;
    std::string summary;
    std::string request_headers;
    std::string request_body;
    std::string response_headers;
    std::string response_body;
};

struct NetworkMockSession {
    std::string proxy_address = "127.0.0.1:8888";
    bool intercept_enabled = true;
    std::vector<NetworkExchange> exchanges;
};

struct NetworkComposeTemplate {
    std::string name;
    std::string method;
    std::string url;
    std::string headers;
    std::string body;
    int timeout_ms = 30000;
};

[[nodiscard]] int parse_compose_timeout_ms(const std::string& text, int default_ms = 30000);

[[nodiscard]] NetworkMockSession make_demo_network_session();
[[nodiscard]] NetworkExchangeOrigin network_exchange_origin_from_string(const std::string& value);
[[nodiscard]] const char* network_exchange_origin_label(NetworkExchangeOrigin origin);

}  // namespace tui_debug_ui

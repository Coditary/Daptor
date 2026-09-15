#include "tui_debug_ui/network_mock_data.hpp"

namespace tui_debug_ui {

NetworkExchangeOrigin network_exchange_origin_from_string(const std::string& value) {
    if (value == "compose_mock" || value == "compose") {
        return NetworkExchangeOrigin::Compose;
    }
    if (value == "demo" || value == "preview") {
        return NetworkExchangeOrigin::Demo;
    }
    return NetworkExchangeOrigin::Captured;
}

const char* network_exchange_origin_label(NetworkExchangeOrigin origin) {
    switch (origin) {
    case NetworkExchangeOrigin::Compose:
        return "compose";
    case NetworkExchangeOrigin::Demo:
        return "demo";
    case NetworkExchangeOrigin::Captured:
        return "live";
    }
    return "live";
}

NetworkMockSession make_demo_network_session() {
    NetworkMockSession session;
    session.proxy_address = "127.0.0.1:8888";
    session.intercept_enabled = true;

    session.exchanges.push_back(NetworkExchange{
        .id = "1",
        .state = NetworkExchangeState::Completed,
        .origin = NetworkExchangeOrigin::Demo,
        .method = "GET",
        .status_code = 200,
        .duration_ms = 18,
        .path = "/api/health",
        .summary = "health check on startup",
        .request_headers =
            "Host: api.shop.example\n"
            "Accept: application/json\n"
            "User-Agent: demo-client/1.2.0",
        .request_body = "",
        .response_headers =
            "HTTP/1.1 200 OK\n"
            "Content-Type: application/json\n"
            "Cache-Control: no-store",
        .response_body = R"({"status":"ok","version":"2.4.1"})",
    });

    session.exchanges.push_back(NetworkExchange{
        .id = "2",
        .state = NetworkExchangeState::Completed,
        .origin = NetworkExchangeOrigin::Demo,
        .method = "GET",
        .status_code = 200,
        .duration_ms = 94,
        .path = "/api/users?page=1&limit=20",
        .summary = "paginated user list",
        .request_headers =
            "Host: api.shop.example\n"
            "Accept: application/json\n"
            "Authorization: Bearer eyJhbGciOi…mock",
        .request_body = "",
        .response_headers =
            "HTTP/1.1 200 OK\n"
            "Content-Type: application/json",
        .response_body =
            R"({"items":[{"id":1,"name":"Ada Lovelace"},{"id":2,"name":"Grace Hopper"}],"page":1,"total":42})",
    });

    session.exchanges.push_back(NetworkExchange{
        .id = "3",
        .state = NetworkExchangeState::Pending,
        .origin = NetworkExchangeOrigin::Demo,
        .method = "POST",
        .status_code = 0,
        .duration_ms = 0,
        .path = "/api/v1/orders",
        .summary = "checkout — waiting for forward/drop",
        .request_headers =
            "Host: api.shop.example\n"
            "Content-Type: application/json\n"
            "Authorization: Bearer eyJhbGciOi…mock\n"
            "X-Request-Id: req-9f2c11",
        .request_body =
            R"({
  "cart_id": "cart_7xk29",
  "payment_method": "card",
  "items": [
    {"sku": "BOOK-RUST", "qty": 1},
    {"sku": "MUG-DEBUG", "qty": 2}
  ]
})",
        .response_headers = "(not sent yet — intercepted)",
        .response_body = "",
    });

    session.exchanges.push_back(NetworkExchange{
        .id = "4",
        .state = NetworkExchangeState::Completed,
        .origin = NetworkExchangeOrigin::Demo,
        .method = "POST",
        .status_code = 201,
        .duration_ms = 142,
        .path = "/api/v1/login",
        .summary = "session established",
        .request_headers =
            "Host: api.shop.example\n"
            "Content-Type: application/json",
        .request_body = R"({"email":"dev@example.com","password":"••••••••"})",
        .response_headers =
            "HTTP/1.1 201 Created\n"
            "Content-Type: application/json\n"
            "Set-Cookie: session=mock_session_token; HttpOnly",
        .response_body = R"({"token":"mock.jwt.token","expires_in":3600})",
    });

    session.exchanges.push_back(NetworkExchange{
        .id = "5",
        .state = NetworkExchangeState::Completed,
        .origin = NetworkExchangeOrigin::Demo,
        .method = "GET",
        .status_code = 404,
        .duration_ms = 31,
        .path = "/api/products/99999",
        .summary = "missing product lookup",
        .request_headers =
            "Host: api.shop.example\n"
            "Accept: application/json",
        .request_body = "",
        .response_headers =
            "HTTP/1.1 404 Not Found\n"
            "Content-Type: application/json",
        .response_body = R"({"error":"product_not_found","id":99999})",
    });

    session.exchanges.push_back(NetworkExchange{
        .id = "6",
        .state = NetworkExchangeState::Completed,
        .origin = NetworkExchangeOrigin::Demo,
        .method = "WS",
        .status_code = 101,
        .duration_ms = 56,
        .path = "wss://api.shop.example/ws/events",
        .summary = "websocket upgrade + 3 frames captured",
        .request_headers =
            "GET /ws/events HTTP/1.1\n"
            "Host: api.shop.example\n"
            "Upgrade: websocket\n"
            "Connection: Upgrade",
        .request_body = "",
        .response_headers =
            "HTTP/1.1 101 Switching Protocols\n"
            "Upgrade: websocket\n"
            "Connection: Upgrade",
        .response_body =
            "↑ {\"type\":\"subscribe\",\"channel\":\"orders\"}\n"
            "↓ {\"type\":\"ack\",\"channel\":\"orders\"}\n"
            "↓ {\"type\":\"event\",\"order_id\":\"ord_42\",\"status\":\"paid\"}",
    });

    return session;
}

}  // namespace tui_debug_ui

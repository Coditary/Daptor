"""HTTP fixture for exercising the Network panel while debugging.

The debugged program below performs real HTTP requests against httpbin.org.
When debugging with a real session, HTTP(S) traffic from the debuggee is
captured via a local MITM proxy and shown in the Network tab. Use
`TUI_DEBUG_NETWORK_MOCK=1` or `--mock` to force the demo traffic instead.

Run (with real debugpy session):
  ./cpp/build-ui.sh run examples/python/network_demo.py

Run without debugpy (UI-only mock debugger):
  ./cpp/build-ui.sh run --mock examples/python/network_demo.py

Standalone HTTP smoke test (no debugger):
  python3 examples/python/network_demo.py

With a real debug session, captured httpbin requests appear in Network.
`--mock` or a failed proxy startup falls back to the built-in demo traffic.

Suggested workflow:
  1. Set a breakpoint on the `return` in `fetch_get` (line ~24).
  2. Start debugging (F5 / continue until the breakpoint).
  3. Open the bottom **Network** tab — you should see mock traffic (6 requests,
     one intercepted POST).
  4. Press `v` to switch Traffic ↔ Compose; in Compose pick a template and press
     `s` to append a mock sent request to Traffic.
  5. Continue execution — the program performs real GET/POST calls to httpbin.org
     (visible in Locals / stdout, not yet mirrored into the Network list).
"""

from __future__ import annotations

import json
import urllib.error
import urllib.request


def fetch_get(url: str, *, timeout: float = 15.0) -> dict:
    request = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = response.read().decode("utf-8")
    return json.loads(body)


def fetch_post(url: str, payload: dict, *, timeout: float = 15.0) -> dict:
    data = json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={"Content-Type": "application/json", "X-Demo": "tui-debug"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = response.read().decode("utf-8")
    return json.loads(body)


def demo() -> None:
    health = fetch_get("https://httpbin.org/get?probe=health")
    print("GET health:", health.get("args"))

    users = fetch_get("https://httpbin.org/get?resource=users&limit=3")
    print("GET users args:", users.get("args"))

    login = fetch_post(
        "https://httpbin.org/post",
        {"username": "demo", "password": "secret"},
    )
    print("POST login json keys:", sorted(login.get("json", {}).keys()))

    order = fetch_post(
        "https://httpbin.org/post",
        {"sku": "widget-7", "qty": 2, "total": 59.97},
    )
    print("POST order status url:", order.get("url"))


if __name__ == "__main__":
    try:
        demo()
    except urllib.error.URLError as error:
        print("Network error (need internet access):", error)
        raise SystemExit(1) from error

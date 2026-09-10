"""Stdlib stepping fixture — verify library source navigation.

Run:
  ./cpp/build-ui.sh run fixtures/stdlib_step.py

Manual test:
  1. Continue from stop-on-entry to the breakpoint on `json.dumps`.
  2. Step Into — the source panel should open `json/encoder.py` (or similar).
  3. Use Stacks + Enter to jump between your code and the stdlib frame.

Automated check:
  cargo test -p tui-debug-core step_into_python_stdlib_source
"""

import json


def main() -> None:
    payload = {"hello": "stdlib", "n": 2}
    text = json.dumps(payload)  # breakpoint + step-into target
    print(text)


if __name__ == "__main__":
    main()

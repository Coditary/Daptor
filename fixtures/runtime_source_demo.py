#!/usr/bin/env python3
"""Runtime Source panel demo (debugpy).

Build / run:
  ./cpp/build/tui-debug-ui fixtures/runtime_source_demo.py

Manual test:
  1. Continue to the breakpoint on `result = compute(...)`.
  2. Add a "Runtime Source" panel — it should show this file.
  3. Step in Python; re-fetch with the panel refresh (↻) after stops.
"""


def compute(base: int, factor: int) -> int:
    total = base
    for step in range(factor):
        total = (total * 3 + step) % 997
    return total


def main() -> None:
    base = 41
    factor = 7
    result = compute(base, factor)  # breakpoint here
    print(f"result={result}")


if __name__ == "__main__":
    main()

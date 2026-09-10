"""Multi-file fixture for testing source navigation across modules.

Run:
  ./cpp/build-ui.sh run fixtures/multi_file.py

Suggested manual test:
  1. Set a breakpoint in main() on the `build_greeting(name)` line.
  2. Continue — stop in main().
  3. Step Into — source should switch to greeter.py (follow: on).
  4. Tab to Stacks, pick an older frame, press Enter — jump back to main().
  5. Set a breakpoint in greeter.py on `return message`, continue — stops there.
  6. Check the Breakpoints panel lists both files; Enter jumps to each.
  7. Press `f` to toggle follow execution while browsing another file.
"""

from greeter import build_greeting, shout_greeting


def main() -> None:
    names = ["world", "debugger", "tui-debug"]
    lines = []

    for name in names:
        greeting = build_greeting(name)
        lines.append(shout_greeting(name))
        print(greeting)
        print(lines[-1])

    print(f"done: {len(lines)} greetings")


if __name__ == "__main__":
    main()

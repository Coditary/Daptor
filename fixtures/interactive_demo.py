"""Interactive stdin demo for tui-debug's integrated console.

Run:
  ./cpp/build-ui.sh run fixtures/interactive_demo.py

Then focus the Console panel and answer the prompts.
Set a breakpoint on the `name = input(...)` line to pause before typing.
"""


def ask_questions() -> tuple[str, int]:
    print("=== tui-debug interactive demo ===")
    print("Focus the Console panel, then type your answers here.")
    print()

    name = input("Your name: ")
    raw_count = input("How many greetings (1-5)? ")
    count = max(1, min(5, int(raw_count)))
    return name, count


def main() -> None:
    name, count = ask_questions()

    for index in range(count):
        print(f"Hello, {name}! ({index + 1}/{count})")

    answer = input("Type 'quit' to exit early, or press Enter to finish: ")
    if answer.strip().lower() == "quit":
        print("Goodbye!")
    else:
        print("Done.")


if __name__ == "__main__":
    main()

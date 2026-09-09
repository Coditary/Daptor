"""Small fixture program for exercising the DAP server layer."""

def greet(name: str) -> str:
    message = f"Hello, {name}!"
    return message


def main() -> None:
    names = ["world", "debugger", "tui-debug"]
    results = []

    for name in names:
        value = greet(name)
        results.append(value)
        print(value)

    total = len(results)
    print(f"done: {total} greetings")


if __name__ == "__main__":
    main()

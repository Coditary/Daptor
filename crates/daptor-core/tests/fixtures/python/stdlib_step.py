"""Stdlib stepping fixture — verify library source navigation.

Automated check:
  cargo test -p daptor-core step_into_python_stdlib_source
"""

import json


def main() -> None:
    payload = {"hello": "stdlib", "n": 2}
    text = json.dumps(payload)  # breakpoint + step-into target
    print(text)


if __name__ == "__main__":
    main()

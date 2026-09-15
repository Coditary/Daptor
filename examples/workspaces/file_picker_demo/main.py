"""File picker demo — nested folders and duplicate basenames.

Run:
  ./cpp/build/tui-debug-ui --mock examples/workspaces/file_picker_demo/main.py

Manual test:
  1. Add a "File Tree" panel via the pane header menu (+).
  2. Expand `lib/`, `tests/`, `helpers/` with Enter or click on the arrow.
  3. Enter on a file opens it in the source panel.
  4. Press `p` for the fuzzy file picker (search-only, no tree).
  5. Type `utils` in the picker — three different `utils.py` with folder prefixes.
  6. Set breakpoints in `lib/models/user.py` and `helpers/utils.py`.
"""

from lib.models.user import User
from lib.utils import summarize


def main() -> None:
    users = [
        User("Ada"),
        User("Grace"),
        User("Linus"),
    ]

    for user in users:
        print(summarize(user))

    print("file picker demo done")


if __name__ == "__main__":
    main()

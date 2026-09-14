"""User model — good target for a breakpoint in a nested folder."""


class User:
    def __init__(self, name: str) -> None:
        self.name = name

    def display_name(self) -> str:
        return self.name

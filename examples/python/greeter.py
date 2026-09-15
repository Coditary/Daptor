"""Helper module for multi-file debugger UI tests."""


def build_greeting(name: str) -> str:
    prefix = "Hello"
    suffix = "!"
    message = f"{prefix}, {name}{suffix}"
    return message


def shout_greeting(name: str) -> str:
    return build_greeting(name).upper()

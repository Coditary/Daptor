"""Shared helpers used by main.py."""

from lib.models.user import User


def summarize(user: User) -> str:
    return f"[lib] {user.name}: active"

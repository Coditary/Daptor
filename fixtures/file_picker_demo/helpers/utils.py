"""Misc helpers — third `utils.py` for picker disambiguation."""


def slugify(value: str) -> str:
    return value.strip().lower().replace(" ", "-")

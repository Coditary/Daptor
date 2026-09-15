"""Test helpers — same basename as lib/utils.py on purpose."""


def assert_equal(left: object, right: object) -> None:
    if left != right:
        raise AssertionError(f"{left!r} != {right!r}")

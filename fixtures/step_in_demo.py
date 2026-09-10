"""Demo fixture for inline step-in target selection (use with --mock)."""

def foo(x):
    return x + 1


def bar(x):
    return x * 2


def baz(a, b):
    return a + b


def qux(x):
    return str(x)


def wibble():
    return 0


def demo():
    name = "debugger"
    items = [1, 2, 3]
    # Mock session stops here — press Step Into (i or toolbar) to preview target selection.
    result = foo(bar(1)) + baz(qux(2), wibble()) + str(len(items))
    return result


if __name__ == "__main__":
    demo()

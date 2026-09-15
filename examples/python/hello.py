"""Small fixture program for exercising the DAP server layer."""


def greet(name: str) -> str:
    message = f"Hello, {name}!"
    return message


def scroll_test_locals() -> int:
    """Set a breakpoint on `return total` to exercise Locals scrolling."""
    # Many individual locals so the Locals pane needs a scrollbar.
    _local_000 = 0
    _local_001 = 1
    _local_002 = 2
    _local_003 = 3
    _local_004 = 4
    _local_005 = 5
    _local_006 = 6
    _local_007 = 7
    _local_008 = 8
    _local_009 = 9
    _local_010 = 10
    _local_011 = 11
    _local_012 = 12
    _local_013 = 13
    _local_014 = 14
    _local_015 = 15
    _local_016 = 16
    _local_017 = 17
    _local_018 = 18
    _local_019 = 19
    _local_020 = 20
    _local_021 = 21
    _local_022 = 22
    _local_023 = 23
    _local_024 = 24
    _local_025 = 25
    _local_026 = 26
    _local_027 = 27
    _local_028 = 28
    _local_029 = 29
    _local_030 = 30
    _local_031 = 31
    _local_032 = 32
    _local_033 = 33
    _local_034 = 34
    _local_035 = 35
    _local_036 = 36
    _local_037 = 37
    _local_038 = 38
    _local_039 = 39
    _local_040 = 40
    _local_041 = 41
    _local_042 = 42
    _local_043 = 43
    _local_044 = 44
    _local_045 = 45
    _local_046 = 46
    _local_047 = 47
    _local_048 = 48
    _local_049 = 49
    _local_050 = 50
    _local_051 = 51
    _local_052 = 52
    _local_053 = 53
    _local_054 = 54
    _local_055 = 55
    _local_056 = 56
    _local_057 = 57
    _local_058 = 58
    _local_059 = 59
    _local_060 = 60
    _local_061 = 61
    _local_062 = 62
    _local_063 = 63
    _local_064 = 64
    _local_065 = 65
    _local_066 = 66
    _local_067 = 67
    _local_068 = 68
    _local_069 = 69
    _local_070 = 70
    _local_071 = 71
    _local_072 = 72
    _local_073 = 73
    _local_074 = 74
    _local_075 = 75
    _local_076 = 76
    _local_077 = 77
    _local_078 = 78
    _local_079 = 79
    total = sum(
        (
            _local_000,
            _local_001,
            _local_002,
            _local_003,
            _local_004,
            _local_005,
            _local_006,
            _local_007,
            _local_008,
            _local_009,
            _local_010,
            _local_011,
            _local_012,
            _local_013,
            _local_014,
            _local_015,
            _local_016,
            _local_017,
            _local_018,
            _local_019,
            _local_020,
            _local_021,
            _local_022,
            _local_023,
            _local_024,
            _local_025,
            _local_026,
            _local_027,
            _local_028,
            _local_029,
            _local_030,
            _local_031,
            _local_032,
            _local_033,
            _local_034,
            _local_035,
            _local_036,
            _local_037,
            _local_038,
            _local_039,
            _local_040,
            _local_041,
            _local_042,
            _local_043,
            _local_044,
            _local_045,
            _local_046,
            _local_047,
            _local_048,
            _local_049,
            _local_050,
            _local_051,
            _local_052,
            _local_053,
            _local_054,
            _local_055,
            _local_056,
            _local_057,
            _local_058,
            _local_059,
            _local_060,
            _local_061,
            _local_062,
            _local_063,
            _local_064,
            _local_065,
            _local_066,
            _local_067,
            _local_068,
            _local_069,
            _local_070,
            _local_071,
            _local_072,
            _local_073,
            _local_074,
            _local_075,
            _local_076,
            _local_077,
            _local_078,
            _local_079,
        )
    )
    return total


def main() -> None:
    scroll_test_locals()
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

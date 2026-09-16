#include <iostream>
#include <string>

static int sum_up_to(int limit) {
    int total = 0;
    for (int value = 1; value <= limit; ++value) {
        total += value;
    }
    return total;
}

static int parse_limit(int argc, char* argv[]) {
    if (argc < 2) {
        return 5;
    }
    try {
        return std::stoi(argv[1]);
    } catch (...) {
        return 5;
    }
}

int main(int argc, char* argv[]) {
    const int limit = parse_limit(argc, argv);
    const int result = sum_up_to(limit);
    std::cout << "sum(1.." << limit << ") = " << result << '\n';
    return 0;
}

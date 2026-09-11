#include <iostream>
#include <stdexcept>
#include <string>

static void throw_runtime_error_demo() {
    throw std::runtime_error("demo runtime_error");
}

static void throw_logic_error_demo() {
    throw std::logic_error("demo logic_error");
}

static int parse_choice(int argc, char* argv[]) {
    if (argc >= 2) {
        try {
            return std::stoi(argv[1]);
        } catch (...) {
        }
    }
    return 1;
}

int main(int argc, char* argv[]) {
    const int choice = parse_choice(argc, argv);
    std::cerr << "exception_demo: choice=" << choice << '\n';
    std::cerr << "  1 = std::runtime_error\n";
    std::cerr << "  2 = std::logic_error\n";

    try {
        if (choice == 2) {
            throw_logic_error_demo();
        }
        throw_runtime_error_demo();
    } catch (const std::logic_error& e) {
        std::cerr << "caught logic_error: " << e.what() << '\n';
        return 0;
    } catch (const std::runtime_error& e) {
        std::cerr << "caught runtime_error: " << e.what() << '\n';
        return 0;
    }

    return 0;
}

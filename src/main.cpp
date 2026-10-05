#include <exception>
#include <iostream>

#include "cli/cli.hpp"

int main(int argc, char** argv) {
    try {
        return pilot::Cli::run(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "FATAL: unhandled exception: " << ex.what() << '\n';
    } catch (...) {
        std::cerr << "FATAL: unknown exception\n";
    }
    return 1;
}

#include "lsp_server.h"
#include <iostream>

int main(int argc, char** argv) {
    (void)argc;  // Reserved for future command-line argument parsing
    (void)argv;  // Reserved for future command-line argument parsing

    try {
        naab::lsp::LSPServer server;
        server.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}

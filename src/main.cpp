// insty-lsp — language server entry point.

#include <cstring>
#include <memory>
#include <string>

#include <lsp/lsp_server.hpp>

int main(int argc, char** argv) {
    int port = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::strncmp(argv[i], "--socket=", 9) == 0) {
            port = std::atoi(argv[i] + 9);
        }
    }

    std::unique_ptr<LSP::Transport> transport;
    if (port > 0) {
        transport = std::make_unique<LSP::SocketTransport>(port);
    } else {
        transport = std::make_unique<LSP::StdioTransport>();
    }

    LSP::Server server(std::move(transport));
    server.start();
    return 0;
}

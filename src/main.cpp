// insty-lsp — language server entry point.

#include <cstring>
#include <memory>
#include <string>

#include <lsp/lsp_server.hpp>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    // The LSP wire protocol frames messages with literal CRLF separators
    // ("\r\n\r\n"). On Windows, stdio defaults to text mode, which rewrites
    // every '\n' we emit into "\r\n" — turning our "\r\n" into "\r\r\n" and
    // breaking the client's strict header parser (start() hangs forever).
    // Force binary mode on stdin/stdout so bytes pass through untranslated.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

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

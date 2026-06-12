#include <lsp/lsp_server.hpp>

#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace LSP {

bool StdioTransport::readLine(std::string& line) {
    if (!std::getline(std::cin, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

bool StdioTransport::read(char* buffer, size_t size) {
    std::cin.read(buffer, static_cast<std::streamsize>(size));
    return std::cin.good() || std::cin.eof();
}

void StdioTransport::write(const std::string& data) {
    if (!std::cout.good()) {
        throw std::runtime_error("stdout is not in good state");
    }
    std::cout << data << std::flush;
    if (!std::cout.good()) {
        throw std::runtime_error("Failed to write to stdout");
    }
}

SocketTransport::SocketTransport(int port)
    : serverSocket(-1), clientSocket(-1), port(port) {
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(serverSocket, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        close(serverSocket);
        throw std::runtime_error("Failed to bind socket to port " + std::to_string(port));
    }

    if (listen(serverSocket, 1) < 0) {
        close(serverSocket);
        throw std::runtime_error("Failed to listen on socket");
    }

    std::cerr << "LSP server listening on port " << port << std::endl;
}

SocketTransport::~SocketTransport() {
    if (clientSocket >= 0) {
        close(clientSocket);
    }
    if (serverSocket >= 0) {
        close(serverSocket);
    }
}

bool SocketTransport::acceptConnection() {
    if (clientSocket >= 0) {
        return true;
    }

    std::cerr << "Waiting for client connection..." << std::endl;
    clientSocket = accept(serverSocket, nullptr, nullptr);
    if (clientSocket < 0) {
        std::cerr << "Failed to accept connection" << std::endl;
        return false;
    }
    std::cerr << "Client connected" << std::endl;
    return true;
}

bool SocketTransport::readLine(std::string& line) {
    if (!acceptConnection()) {
        return false;
    }

    line.clear();
    char c = '\0';
    while (::read(clientSocket, &c, 1) == 1) {
        if (c == '\n') {
            return true;
        }
        if (c != '\r') {
            line += c;
        }
    }
    return false;
}

bool SocketTransport::read(char* buffer, size_t size) {
    if (!acceptConnection()) {
        return false;
    }

    size_t totalRead = 0;
    while (totalRead < size) {
        const ssize_t n = ::read(clientSocket, buffer + totalRead, size - totalRead);
        if (n <= 0) {
            return false;
        }
        totalRead += static_cast<size_t>(n);
    }
    return true;
}

void SocketTransport::write(const std::string& data) {
    if (!acceptConnection()) {
        throw std::runtime_error("No client connection");
    }

    const ssize_t written = ::write(clientSocket, data.c_str(), data.size());
    if (written < 0 || static_cast<size_t>(written) != data.size()) {
        throw std::runtime_error("Failed to write to socket");
    }
}

bool SocketTransport::isOpen() const {
    return clientSocket >= 0;
}

} // namespace LSP

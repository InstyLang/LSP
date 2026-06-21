#include <lsp/lsp_internal.hpp>

#include <parser/parser.hpp>
#include <utilities/errors.hpp>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace LSP::detail {

// Resolve the directory containing the running executable so the server can
// locate sibling resources. Uses the platform-native mechanism and falls back
// to the current working directory when that is unavailable.
std::filesystem::path resolveExecutableDir() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buffer).parent_path();
    }
    return std::filesystem::current_path();
#elif defined(__linux__)
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return path.parent_path();
    }
    return std::filesystem::current_path();
#else
    return std::filesystem::current_path();
#endif
}

bool isIdentifierChar(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (text.empty() || text.back() == '\n') {
        lines.emplace_back();
    }
    return lines;
}

std::string getLineText(const std::string& text, int line) {
    auto lines = splitLines(text);
    if (line < 0 || line >= static_cast<int>(lines.size())) {
        return "";
    }
    return lines[static_cast<size_t>(line)];
}

size_t offsetForPosition(const std::string& text, int line, int character) {
    if (line < 0 || character < 0) {
        return 0;
    }

    int currentLine = 0;
    size_t offset = 0;
    while (offset < text.size() && currentLine < line) {
        if (text[offset++] == '\n') {
            ++currentLine;
        }
    }

    return std::min(offset + static_cast<size_t>(character), text.size());
}

std::optional<std::pair<size_t, size_t>> identifierRangeAt(const std::string& text, int line, int character) {
    if (text.empty()) {
        return std::nullopt;
    }

    size_t offset = offsetForPosition(text, line, character);
    if (offset > 0 && (offset == text.size() || !isIdentifierChar(text[offset])) && isIdentifierChar(text[offset - 1])) {
        --offset;
    }
    if (offset >= text.size() || !isIdentifierChar(text[offset])) {
        return std::nullopt;
    }

    size_t start = offset;
    while (start > 0 && isIdentifierChar(text[start - 1])) {
        --start;
    }

    size_t end = offset;
    while (end < text.size() && isIdentifierChar(text[end])) {
        ++end;
    }

    return std::make_pair(start, end);
}

std::optional<std::pair<std::string, std::string>> qualifiedReferenceAt(const std::string& text, int line, int character) {
    auto current = identifierRangeAt(text, line, character);
    if (!current) {
        return std::nullopt;
    }

    const auto& [start, end] = *current;
    if (start == 0 || text[start - 1] != '.') {
        return std::nullopt;
    }

    size_t qualifierEnd = start - 1;
    size_t qualifierStart = qualifierEnd;
    while (qualifierStart > 0 && isIdentifierChar(text[qualifierStart - 1])) {
        --qualifierStart;
    }
    if (!isIdentifierChar(text[qualifierStart])) {
        return std::nullopt;
    }

    return std::make_pair(
        text.substr(qualifierStart, qualifierEnd - qualifierStart),
        text.substr(start, end - start));
}

CompletionContext completionContextAt(const std::string& text, int line, int character) {
    CompletionContext ctx;
    const std::string lineText = getLineText(text, line);
    if (character < 0) {
        return ctx;
    }

    const size_t prefixLength = std::min(static_cast<size_t>(character), lineText.size());
    const std::string prefix = lineText.substr(0, prefixLength);
    const std::string stripped = trim(prefix);

    if (stripped.rfind("import ", 0) == 0 && stripped.find(" as ") == std::string::npos) {
        ctx.kind = CompletionContext::Kind::Import;
        ctx.prefix = trim(stripped.substr(7));
        return ctx;
    }

    size_t memberStart = prefix.size();
    while (memberStart > 0 && isIdentifierChar(prefix[memberStart - 1])) {
        --memberStart;
    }
    if (memberStart > 0 && prefix[memberStart - 1] == '.') {
        size_t qualifierEnd = memberStart - 1;
        size_t qualifierStart = qualifierEnd;
        while (qualifierStart > 0 && isIdentifierChar(prefix[qualifierStart - 1])) {
            --qualifierStart;
        }
        if (qualifierStart < qualifierEnd && isIdentifierChar(prefix[qualifierStart])) {
            ctx.kind = CompletionContext::Kind::Member;
            ctx.qualifier = prefix.substr(qualifierStart, qualifierEnd - qualifierStart);
            ctx.prefix = prefix.substr(memberStart);
            return ctx;
        }
    }

    size_t identStart = prefix.size();
    while (identStart > 0 && isIdentifierChar(prefix[identStart - 1])) {
        --identStart;
    }
    ctx.prefix = prefix.substr(identStart);
    return ctx;
}

std::string inferModuleNameFromText(const std::string& text, const fs::path& filePath) {
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string cleaned = trim(line);
        if (cleaned.empty() || cleaned.rfind("//", 0) == 0) {
            continue;
        }
        if (cleaned.rfind("module ", 0) == 0) {
            std::string rest = trim(cleaned.substr(7));
            size_t end = 0;
            while (end < rest.size() && isIdentifierChar(rest[end])) {
                ++end;
            }
            return rest.substr(0, end);
        }
        break;
    }
    return filePath.stem().string();
}

bool hasDirectImport(const AST::ImportStatement& importStmt, const std::string& name) {
    return std::find(importStmt.importedSymbols.begin(), importStmt.importedSymbols.end(), name) !=
           importStmt.importedSymbols.end();
}

bool sameLocation(const Location& lhs, const Location& rhs) {
    return lhs.uri == rhs.uri &&
           lhs.line == rhs.line &&
           lhs.column == rhs.column &&
           lhs.length == rhs.length;
}

std::optional<std::string> identifierAt(const std::string& text, int line, int character) {
    auto range = identifierRangeAt(text, line, character);
    if (!range) {
        return std::nullopt;
    }
    return text.substr(range->first, range->second - range->first);
}

bool locationContainsPosition(const Location& location, int line, int character) {
    return location.line == line &&
           character >= location.column &&
           character <= location.column + std::max(1, location.length);
}

Location locationFromToken(const std::string& uri, const Token& token) {
    return Location{
        uri,
        std::max(0, token.line - 1),
        std::max(0, token.column - 1),
        std::max(1, static_cast<int>(token.value.size()))
    };
}

std::optional<std::reference_wrapper<const SemanticSymbol>> semanticSymbolAt(const DocumentState& doc, int line, int character) {
    for (const auto& reference : doc.semanticReferences) {
        if (locationContainsPosition(reference.location, line, character) &&
            reference.symbolId >= 0 &&
            reference.symbolId < static_cast<int>(doc.semanticSymbols.size())) {
            return std::cref(doc.semanticSymbols[static_cast<size_t>(reference.symbolId)]);
        }
    }

    for (const auto& symbol : doc.semanticSymbols) {
        if (locationContainsPosition(symbol.declaration, line, character)) {
            return std::cref(symbol);
        }
    }

    return std::nullopt;
}

ParsedDocument parseDocumentState(const std::string& text, const std::string& filename) {
    ParsedDocument parsed;
    ErrorReporting::initErrorReporter(text, filename);

    try {
        std::string mutableText = text;
        Parser parser;
        auto ast = parser.produceAST(mutableText);

        if (ast && (!ErrorReporting::globalErrorReporter || !ErrorReporting::globalErrorReporter->hasError())) {
            parsed.ast = ast;
            parsed.scope = parser.getScopeManager();
            parsed.moduleName = ast->moduleName;
            parsed.imports = ast->imports;
            parsed.hasValidAST = true;
        }
    } catch (const std::exception& e) {
        Diagnostic diag;
        diag.message = std::string("Validation error: ") + e.what();
        diag.severity = 1;
        diag.line = 0;
        diag.column = 0;
        parsed.diagnostics.push_back(diag);
    } catch (...) {
        Diagnostic diag;
        diag.message = "Unknown validation error occurred";
        diag.severity = 1;
        diag.line = 0;
        diag.column = 0;
        parsed.diagnostics.push_back(diag);
    }

    if (ErrorReporting::globalErrorReporter) {
        for (const auto& diag : ErrorReporting::globalErrorReporter->getDiagnostics()) {
            Diagnostic converted;
            converted.message = diag.message;
            if (!diag.hint.empty()) {
                converted.message += " (" + diag.hint + ")";
            }
            converted.severity = diag.level == ErrorReporting::ErrorLevel::Error ? 1 :
                                 diag.level == ErrorReporting::ErrorLevel::Warning ? 2 : 3;
            converted.line = std::max(0, diag.location.line - 1);
            converted.column = std::max(0, diag.location.column - 1);
            converted.length = std::max(1, diag.location.length);
            parsed.diagnostics.push_back(converted);
        }
    }

    ErrorReporting::cleanupErrorReporter();
    return parsed;
}

void addUniqueCompletion(std::vector<CompletionItem>& items, std::set<std::string>& seen,
                         std::string label, int kind, std::string detail, std::string insertText) {
    const std::string key = label + "|" + detail;
    if (!seen.insert(key).second) {
        return;
    }
    items.push_back(CompletionItem{
        std::move(label),
        kind,
        std::move(detail),
        std::move(insertText)
    });
}

Location locationFromSymbol(const DocumentState& doc, const Symbol& symbol) {
    return Location{
        doc.uri,
        std::max(0, symbol.declarationLine - 1),
        std::max(0, symbol.declarationColumn - 1),
        std::max(1, symbol.length)
    };
}

bool looksLikeFunction(const Symbol& symbol) {
    return !symbol.preview.empty() && symbol.preview.rfind("fun ", 0) == 0;
}

int completionKindForSymbol(const Symbol& symbol) {
    if (symbol.type == "struct" || symbol.type == "class" || symbol.type == "enum") {
        return 7;
    }
    if (looksLikeFunction(symbol)) {
        return 3;
    }
    return 6;
}

} // namespace LSP::detail

namespace LSP {

Server::Server(std::unique_ptr<Transport> transport)
    : running(false), transport(std::move(transport)) {
#ifdef SIGPIPE
    // SIGPIPE does not exist on Windows; only ignore it where defined so a
    // broken client connection does not terminate the server.
    signal(SIGPIPE, SIG_IGN);
#endif
    executableDir = detail::resolveExecutableDir();
}

void Server::start() {
    running = true;
    std::string line;

    while (running && transport->isOpen() && transport->readLine(line)) {
        try {
            if (line.rfind("Content-Length:", 0) == 0) {
                const size_t contentLength = std::stoul(line.substr(15));

                while (transport->readLine(line) && !line.empty()) {
                }

                std::string content(contentLength, '\0');
                if (!transport->read(content.data(), contentLength)) {
                    break;
                }

                handleMessage(content);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error handling message: " << e.what() << std::endl;
        }
    }
}

void Server::handleMessage(const std::string& message) {
    try {
        const JSONValue msg = JSONValue::parse(message);
        if (!msg.isObject()) {
            return;
        }

        const std::string method = msg["method"].getString();
        const JSONValue id = msg["id"];
        const JSONValue params = msg["params"];

        if (method == "initialize") {
            handleInitialize(id, params);
        } else if (method == "textDocument/didOpen") {
            handleDidOpen(params);
        } else if (method == "textDocument/didChange") {
            handleDidChange(params);
        } else if (method == "textDocument/didClose") {
            handleDidClose(params);
        } else if (method == "workspace/didChangeWatchedFiles") {
            handleDidChangeWatchedFiles(params);
        } else if (method == "textDocument/completion") {
            handleCompletion(id, params);
        } else if (method == "textDocument/hover") {
            handleHover(id, params);
        } else if (method == "textDocument/definition") {
            handleDefinition(id, params);
        } else if (method == "textDocument/references") {
            handleReferences(id, params);
        } else if (method == "textDocument/prepareRename") {
            handlePrepareRename(id, params);
        } else if (method == "textDocument/rename") {
            handleRename(id, params);
        } else if (method == "shutdown") {
            handleShutdown(id);
        } else if (method == "exit") {
            running = false;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing message: " << e.what() << std::endl;
    }
}

void Server::handleInitialize(const JSONValue& id, const JSONValue& params) {
    workspaceRoots.clear();

    if (params.contains("workspaceFolders") && params["workspaceFolders"].isArray()) {
        for (const auto& folder : params["workspaceFolders"].asArray()) {
            const std::string uri = folder["uri"].getString();
            if (!uri.empty()) {
                workspaceRoots.emplace_back(uriToPath(uri));
            }
        }
    }

    if (workspaceRoots.empty()) {
        const std::string rootUri = params["rootUri"].getString();
        if (!rootUri.empty()) {
            workspaceRoots.emplace_back(uriToPath(rootUri));
        }
    }

    if (workspaceRoots.empty()) {
        const std::string rootPath = params["rootPath"].getString();
        if (!rootPath.empty()) {
            workspaceRoots.emplace_back(rootPath);
        }
    }

    if (workspaceRoots.empty()) {
        workspaceRoots.push_back(fs::current_path());
    }

    moduleCacheByPath.clear();
    modulePathsByName.clear();
    indexedModuleDirectories.clear();
    indexWorkspaceModules();

    JSONValue::Object capabilities;
    capabilities["textDocumentSync"] = 1;

    JSONValue::Object completionProvider;
    completionProvider["triggerCharacters"] = JSONValue::Array{".", "@", "_"};
    capabilities["completionProvider"] = completionProvider;
    capabilities["hoverProvider"] = true;
    capabilities["definitionProvider"] = true;
    capabilities["referencesProvider"] = true;
    JSONValue::Object renameProvider;
    renameProvider["prepareProvider"] = true;
    capabilities["renameProvider"] = renameProvider;

    JSONValue::Object serverInfo;
    serverInfo["name"] = "insty-lsp";
    serverInfo["version"] = "0.2.0";

    JSONValue::Object result;
    result["capabilities"] = capabilities;
    result["serverInfo"] = serverInfo;

    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    sendResponse(JSONValue(response).serialize());
}

std::string Server::uriToPath(const std::string& uri) {
    constexpr const char* prefix = "file://";
    if (uri.rfind(prefix, 0) == 0) {
        return uri.substr(std::strlen(prefix));
    }
    return uri;
}

std::string Server::pathToUri(const std::string& path) {
    return "file://" + path;
}

std::string Server::canonicalPathString(const fs::path& path) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    return (ec ? path.lexically_normal() : canonical).string();
}

void Server::handleShutdown(const JSONValue& id) {
    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = JSONValue();
    sendResponse(JSONValue(response).serialize());
}

void Server::sendResponse(const std::string& response) {
    try {
        const std::string header = "Content-Length: " + std::to_string(response.size()) + "\r\n\r\n";
        transport->write(header + response);
    } catch (const std::exception& e) {
        std::cerr << "Error sending response: " << e.what() << std::endl;
        running = false;
    } catch (...) {
        std::cerr << "Unknown error sending response" << std::endl;
        running = false;
    }
}

void Server::sendDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diagnostics) {
    JSONValue::Array diags;
    for (const auto& d : diagnostics) {
        JSONValue::Object diag;
        JSONValue::Object range;
        JSONValue::Object start;
        start["line"] = d.line;
        start["character"] = d.column;
        JSONValue::Object end;
        end["line"] = d.line;
        end["character"] = d.column + std::max(1, d.length);
        range["start"] = start;
        range["end"] = end;
        diag["range"] = range;
        diag["severity"] = d.severity;
        diag["message"] = d.message;
        diags.push_back(diag);
    }

    JSONValue::Object params;
    params["uri"] = uri;
    params["diagnostics"] = diags;

    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["method"] = "textDocument/publishDiagnostics";
    response["params"] = params;
    sendResponse(JSONValue(response).serialize());
}

void Server::sendCompletionResponse(const JSONValue& id, const std::vector<CompletionItem>& items) {
    JSONValue::Array result;
    for (const auto& item : items) {
        JSONValue::Object completionItem;
        completionItem["label"] = item.label;
        completionItem["kind"] = item.kind;
        completionItem["detail"] = item.detail;
        if (!item.insertText.empty()) {
            completionItem["insertText"] = item.insertText;
        }
        result.push_back(completionItem);
    }

    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    sendResponse(JSONValue(response).serialize());
}

void Server::sendLocationsResponse(const JSONValue& id, const std::vector<Location>& locations) {
    JSONValue::Array result;
    for (const auto& location : locations) {
        JSONValue::Object item;
        item["uri"] = location.uri;

        JSONValue::Object range;
        JSONValue::Object start;
        start["line"] = location.line;
        start["character"] = location.column;
        JSONValue::Object end;
        end["line"] = location.line;
        end["character"] = location.column + std::max(1, location.length);
        range["start"] = start;
        range["end"] = end;
        item["range"] = range;
        result.push_back(item);
    }

    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    sendResponse(JSONValue(response).serialize());
}

void Server::sendPrepareRenameResponse(const JSONValue& id, const Location& location, const std::string& placeholder) {
    JSONValue::Object result;

    JSONValue::Object range;
    JSONValue::Object start;
    start["line"] = location.line;
    start["character"] = location.column;
    JSONValue::Object end;
    end["line"] = location.line;
    end["character"] = location.column + std::max(1, location.length);
    range["start"] = start;
    range["end"] = end;

    result["range"] = range;
    result["placeholder"] = placeholder;

    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    sendResponse(JSONValue(response).serialize());
}

void Server::sendLocationResponse(const JSONValue& id, const Location& location) {
    JSONValue::Object result;
    result["uri"] = location.uri;

    JSONValue::Object range;
    JSONValue::Object start;
    start["line"] = location.line;
    start["character"] = location.column;
    JSONValue::Object end;
    end["line"] = location.line;
    end["character"] = location.column + std::max(1, location.length);
    range["start"] = start;
    range["end"] = end;
    result["range"] = range;

    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    sendResponse(JSONValue(response).serialize());
}

void Server::sendWorkspaceEditResponse(const JSONValue& id, const std::vector<TextEdit>& edits) {
    JSONValue::Object changes;
    for (const auto& edit : edits) {
        JSONValue::Object textEdit;

        JSONValue::Object range;
        JSONValue::Object start;
        start["line"] = edit.location.line;
        start["character"] = edit.location.column;
        JSONValue::Object end;
        end["line"] = edit.location.line;
        end["character"] = edit.location.column + std::max(1, edit.location.length);
        range["start"] = start;
        range["end"] = end;

        textEdit["range"] = range;
        textEdit["newText"] = edit.newText;

        auto existing = changes.find(edit.location.uri);
        if (existing == changes.end()) {
            changes[edit.location.uri] = JSONValue::Array{};
            existing = changes.find(edit.location.uri);
        }

        auto uriEdits = existing->second.asArray();
        uriEdits.push_back(textEdit);
        existing->second = uriEdits;
    }

    JSONValue::Object workspaceEdit;
    workspaceEdit["changes"] = changes;

    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = workspaceEdit;
    sendResponse(JSONValue(response).serialize());
}

void Server::sendHoverResponse(const JSONValue& id, const Symbol& symbol) {
    JSONValue::Object contents;
    contents["kind"] = "markdown";

    std::string value;
    if (!symbol.preview.empty()) {
        value = "```insty\n" + symbol.preview + "\n```";
    } else {
        value = "**" + symbol.name + "**: `" + symbol.type + "`";
    }
    if (!symbol.docs.empty()) {
        value += "\n\n" + symbol.docs;
    }
    contents["value"] = value;

    JSONValue::Object result;
    result["contents"] = contents;

    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;
    sendResponse(JSONValue(response).serialize());
}

void Server::sendNullResponse(const JSONValue& id) {
    JSONValue::Object response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = JSONValue();
    sendResponse(JSONValue(response).serialize());
}

} // namespace LSP

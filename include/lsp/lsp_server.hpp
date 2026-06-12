#pragma once

#include <extra/ast.hpp>
#include <filesystem>
#include <iostream>
#include <lexer/lexer.hpp>
#include <map>
#include <memory>
#include <optional>
#include <parser/scope_manager.hpp>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"

namespace LSP {

struct Diagnostic {
    std::string message;
    int severity; // 1=Error, 2=Warning, 3=Info, 4=Hint
    int line;
    int column;
    int length = 1;
};

struct CompletionItem {
    std::string label;
    int kind; // LSP CompletionItemKind
    std::string detail;
    std::string insertText;
};

struct Location {
    std::string uri;
    int line = 0;
    int column = 0;
    int length = 1;
};

struct TextEdit {
    Location location;
    std::string newText;
};

struct SemanticSymbol {
    int id = -1;
    std::string name;
    std::string type;
    std::string preview;
    Location declaration;
    int declarationOffset = -1;
    int scopeStartOffset = -1;
    int scopeEndOffset = -1;
    bool isParameter = false;
    bool isGlobal = false;
    bool isHoisted = false;
};

struct SemanticReference {
    int symbolId = -1;
    Location location;
};

struct DocumentState {
    std::string uri;
    std::string filePath;
    std::string text;
    std::string moduleName;
    std::vector<std::string> imports;
    std::shared_ptr<AST::ProgramRoot> ast;
    ScopeManager globalScope;
    std::vector<SemanticSymbol> semanticSymbols;
    std::vector<SemanticReference> semanticReferences;
    bool hasValidAST = false;
    bool fromDisk = false;
};

struct ModuleCacheEntry {
    std::string moduleName;
    std::string canonicalPath;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool readLine(std::string& line) = 0;
    virtual bool read(char* buffer, size_t size) = 0;
    virtual void write(const std::string& data) = 0;
    virtual bool isOpen() const = 0;
};

class StdioTransport : public Transport {
public:
    bool readLine(std::string& line) override;
    bool read(char* buffer, size_t size) override;
    void write(const std::string& data) override;
    bool isOpen() const override { return true; }
};

class SocketTransport : public Transport {
public:
    explicit SocketTransport(int port);
    ~SocketTransport();
    bool readLine(std::string& line) override;
    bool read(char* buffer, size_t size) override;
    void write(const std::string& data) override;
    bool isOpen() const override;

private:
    int serverSocket;
    int clientSocket;
    int port;
    bool acceptConnection();
};

class Server {
public:
    explicit Server(std::unique_ptr<Transport> transport);
    void start();

private:
    bool running;
    std::unique_ptr<Transport> transport;
    std::vector<std::filesystem::path> workspaceRoots;
    std::filesystem::path executableDir;
    std::map<std::string, DocumentState> documents;
    std::map<std::string, std::string> moduleUriByName;
    std::map<std::string, std::string> moduleUriByPath;
    std::map<std::string, ModuleCacheEntry> moduleCacheByPath;
    std::map<std::string, std::set<std::string>> modulePathsByName;
    std::set<std::string> indexedModuleDirectories;

    void handleMessage(const std::string& message);
    void handleInitialize(const JSONValue& id, const JSONValue& params);
    void handleDidOpen(const JSONValue& params);
    void handleDidChange(const JSONValue& params);
    void handleDidClose(const JSONValue& params);
    void handleDidChangeWatchedFiles(const JSONValue& params);
    void handleCompletion(const JSONValue& id, const JSONValue& params);
    void handleHover(const JSONValue& id, const JSONValue& params);
    void handleDefinition(const JSONValue& id, const JSONValue& params);
    void handleReferences(const JSONValue& id, const JSONValue& params);
    void handlePrepareRename(const JSONValue& id, const JSONValue& params);
    void handleRename(const JSONValue& id, const JSONValue& params);
    void handleShutdown(const JSONValue& id);

    void validateDocument(const std::string& uri);
    void reindexDocument(const std::string& uri, const std::string& text, bool fromDisk = false);
    void buildSemanticIndex(DocumentState& doc);
    void clearDocumentIndex(const DocumentState& doc);
    void indexDocument(const DocumentState& doc);
    std::optional<std::reference_wrapper<DocumentState>> ensureModuleLoaded(const std::string& moduleName, const DocumentState* contextDoc);
    std::optional<std::reference_wrapper<DocumentState>> getDocument(const std::string& uri);
    std::optional<std::reference_wrapper<const DocumentState>> getDocument(const std::string& uri) const;
    void indexWorkspaceModules();
    void ensureModuleDirectoryIndexed(const std::filesystem::path& dir);
    void upsertModuleCacheEntry(const std::filesystem::path& path, const std::string& text);
    void refreshModuleFile(const std::filesystem::path& path);
    void removeModuleFile(const std::filesystem::path& path);
    std::vector<std::filesystem::path> getModuleSearchDirectories(const DocumentState* contextDoc) const;
    std::vector<std::filesystem::path> getCandidateModuleFiles(const std::string& moduleName, const DocumentState* contextDoc) const;
    std::optional<std::filesystem::path> resolveModulePath(const std::string& moduleName, const DocumentState* contextDoc);
    void loadWorkspaceDocuments();
    std::vector<std::string> collectAvailableModules(const DocumentState* contextDoc);
    std::vector<Diagnostic> collectImportDiagnostics(DocumentState& doc);
    std::vector<CompletionItem> collectCompletionItems(const DocumentState& doc, int line, int character);
    std::vector<Location> collectReferences(const DocumentState& doc, int line, int character, bool includeDeclaration);
    std::optional<Location> prepareRenameLocation(const DocumentState& doc, int line, int character);
    std::optional<Location> findDefinitionLocation(const DocumentState& doc, int line, int character);
    std::optional<Symbol> findHoverSymbol(const DocumentState& doc, int line, int character);
    std::optional<Symbol> findImportedSymbol(const DocumentState& doc, const std::string& qualifier, const std::string& name);
    std::optional<Location> findImportedLocation(const DocumentState& doc, const std::string& qualifier, const std::string& name);
    std::optional<Location> symbolLocationFromDoc(const DocumentState& doc, const Symbol& symbol) const;
    static std::string uriToPath(const std::string& uri);
    static std::string pathToUri(const std::string& path);
    static std::string canonicalPathString(const std::filesystem::path& path);

    void sendResponse(const std::string& response);
    void sendDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diagnostics);
    void sendCompletionResponse(const JSONValue& id, const std::vector<CompletionItem>& items);
    void sendLocationsResponse(const JSONValue& id, const std::vector<Location>& locations);
    void sendPrepareRenameResponse(const JSONValue& id, const Location& location, const std::string& placeholder);
    void sendLocationResponse(const JSONValue& id, const Location& location);
    void sendWorkspaceEditResponse(const JSONValue& id, const std::vector<TextEdit>& edits);
    void sendHoverResponse(const JSONValue& id, const Symbol& symbol);
    void sendNullResponse(const JSONValue& id);
};

} // namespace LSP

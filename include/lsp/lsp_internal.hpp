#pragma once

#include <lsp/lsp_server.hpp>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace LSP::detail {

struct ParsedDocument {
    std::shared_ptr<AST::ProgramRoot> ast;
    ScopeManager scope;
    std::string moduleName;
    std::vector<std::string> imports;
    std::vector<Diagnostic> diagnostics;
    bool hasValidAST = false;
};

struct CompletionContext {
    enum class Kind {
        General,
        Import,
        Member
    };

    Kind kind = Kind::General;
    std::string qualifier;
    std::string prefix;
};

bool isIdentifierChar(char ch);
std::string trim(std::string value);
std::vector<std::string> splitLines(const std::string& text);
std::string getLineText(const std::string& text, int line);
size_t offsetForPosition(const std::string& text, int line, int character);
std::optional<std::pair<size_t, size_t>> identifierRangeAt(const std::string& text, int line, int character);
std::optional<std::pair<std::string, std::string>> qualifiedReferenceAt(const std::string& text, int line, int character);
CompletionContext completionContextAt(const std::string& text, int line, int character);
std::string inferModuleNameFromText(const std::string& text, const std::filesystem::path& filePath);
bool hasDirectImport(const AST::ImportStatement& importStmt, const std::string& name);
bool sameLocation(const Location& lhs, const Location& rhs);
std::optional<std::string> identifierAt(const std::string& text, int line, int character);
bool locationContainsPosition(const Location& location, int line, int character);
Location locationFromToken(const std::string& uri, const Token& token);
std::optional<std::reference_wrapper<const SemanticSymbol>> semanticSymbolAt(const DocumentState& doc, int line, int character);
ParsedDocument parseDocumentState(const std::string& text, const std::string& filename);
void addUniqueCompletion(std::vector<CompletionItem>& items, std::set<std::string>& seen,
                         std::string label, int kind, std::string detail, std::string insertText = "");
Location locationFromSymbol(const DocumentState& doc, const Symbol& symbol);
bool looksLikeFunction(const Symbol& symbol);
int completionKindForSymbol(const Symbol& symbol);
std::filesystem::path resolveExecutableDir();

} // namespace LSP::detail

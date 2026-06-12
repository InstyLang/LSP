#include <lsp/lsp_internal.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace LSP {

namespace {

int completionKindForSemanticSymbol(const SemanticSymbol& symbol) {
    Symbol converted(
        symbol.name,
        symbol.type,
        false,
        symbol.isParameter,
        symbol.declaration.line + 1,
        symbol.declaration.column + 1,
        symbol.declaration.length,
        "",
        symbol.preview
    );
    return detail::completionKindForSymbol(converted);
}

std::vector<std::reference_wrapper<const SemanticSymbol>> visibleSemanticSymbolsAt(
    const DocumentState& doc, int line, int character, const std::string& prefix) {
    const int cursorOffset = static_cast<int>(detail::offsetForPosition(doc.text, line, character));
    std::vector<std::reference_wrapper<const SemanticSymbol>> visible;

    for (const auto& symbol : doc.semanticSymbols) {
        if (!prefix.empty() && symbol.name.rfind(prefix, 0) != 0) {
            continue;
        }
        if (symbol.scopeStartOffset >= 0 && cursorOffset < symbol.scopeStartOffset) {
            continue;
        }
        if (symbol.scopeEndOffset >= 0 && cursorOffset > symbol.scopeEndOffset) {
            continue;
        }
        if (!symbol.isHoisted && symbol.declarationOffset >= 0 && cursorOffset < symbol.declarationOffset) {
            continue;
        }
        visible.push_back(std::cref(symbol));
    }

    std::sort(visible.begin(), visible.end(), [](const auto& lhsRef, const auto& rhsRef) {
        const auto& lhs = lhsRef.get();
        const auto& rhs = rhsRef.get();
        const int lhsSpan = lhs.scopeEndOffset - lhs.scopeStartOffset;
        const int rhsSpan = rhs.scopeEndOffset - rhs.scopeStartOffset;
        if (lhsSpan != rhsSpan) {
            return lhsSpan < rhsSpan;
        }
        if (lhs.declarationOffset != rhs.declarationOffset) {
            return lhs.declarationOffset > rhs.declarationOffset;
        }
        return lhs.name < rhs.name;
    });

    return visible;
}

} // namespace

void Server::handleCompletion(const JSONValue& id, const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    const int line = static_cast<int>(params["position"]["line"].getNumber());
    const int character = static_cast<int>(params["position"]["character"].getNumber());

    auto doc = getDocument(uri);
    if (!doc) {
        sendCompletionResponse(id, {});
        return;
    }

    sendCompletionResponse(id, collectCompletionItems(doc->get(), line, character));
}

void Server::handleHover(const JSONValue& id, const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    const int line = static_cast<int>(params["position"]["line"].getNumber());
    const int character = static_cast<int>(params["position"]["character"].getNumber());

    auto doc = getDocument(uri);
    if (!doc) {
        sendNullResponse(id);
        return;
    }

    auto symbol = findHoverSymbol(doc->get(), line, character);
    if (!symbol) {
        sendNullResponse(id);
        return;
    }

    sendHoverResponse(id, *symbol);
}

void Server::handleDefinition(const JSONValue& id, const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    const int line = static_cast<int>(params["position"]["line"].getNumber());
    const int character = static_cast<int>(params["position"]["character"].getNumber());

    auto doc = getDocument(uri);
    if (!doc) {
        sendNullResponse(id);
        return;
    }

    auto location = findDefinitionLocation(doc->get(), line, character);
    if (!location) {
        sendNullResponse(id);
        return;
    }

    sendLocationResponse(id, *location);
}

void Server::handleReferences(const JSONValue& id, const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    const int line = static_cast<int>(params["position"]["line"].getNumber());
    const int character = static_cast<int>(params["position"]["character"].getNumber());
    const bool includeDeclaration = params.contains("context") ?
        params["context"]["includeDeclaration"].getBool(false) : false;

    auto doc = getDocument(uri);
    if (!doc) {
        sendLocationsResponse(id, {});
        return;
    }

    sendLocationsResponse(id, collectReferences(doc->get(), line, character, includeDeclaration));
}

void Server::handlePrepareRename(const JSONValue& id, const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    const int line = static_cast<int>(params["position"]["line"].getNumber());
    const int character = static_cast<int>(params["position"]["character"].getNumber());

    auto doc = getDocument(uri);
    if (!doc) {
        sendNullResponse(id);
        return;
    }

    auto location = prepareRenameLocation(doc->get(), line, character);
    auto placeholder = detail::identifierAt(doc->get().text, line, character);
    if (!location || !placeholder) {
        sendNullResponse(id);
        return;
    }

    sendPrepareRenameResponse(id, *location, *placeholder);
}

void Server::handleRename(const JSONValue& id, const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    const int line = static_cast<int>(params["position"]["line"].getNumber());
    const int character = static_cast<int>(params["position"]["character"].getNumber());
    const std::string newName = params["newName"].getString();

    auto doc = getDocument(uri);
    if (!doc || newName.empty()) {
        sendNullResponse(id);
        return;
    }

    if (!prepareRenameLocation(doc->get(), line, character)) {
        sendNullResponse(id);
        return;
    }

    if (!std::isalpha(static_cast<unsigned char>(newName.front())) && newName.front() != '_') {
        sendNullResponse(id);
        return;
    }
    if (!std::all_of(newName.begin(), newName.end(), detail::isIdentifierChar)) {
        sendNullResponse(id);
        return;
    }

    auto references = collectReferences(doc->get(), line, character, true);
    if (references.empty()) {
        sendNullResponse(id);
        return;
    }

    std::vector<TextEdit> edits;
    edits.reserve(references.size());
    for (const auto& reference : references) {
        edits.push_back(TextEdit{reference, newName});
    }

    sendWorkspaceEditResponse(id, edits);
}

std::vector<Diagnostic> Server::collectImportDiagnostics(DocumentState& doc) {
    std::vector<Diagnostic> diagnostics;
    if (!doc.hasValidAST || !doc.ast) {
        return diagnostics;
    }

    for (const auto& stmt : doc.ast->body) {
        auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
        if (!importStmt) {
            continue;
        }

        auto loaded = ensureModuleLoaded(importStmt->moduleName, &doc);
        const auto lines = detail::splitLines(doc.text);
        if (!loaded) {
            Diagnostic diag;
            diag.message = "Unresolved module import '" + importStmt->moduleName + "'";
            diag.severity = 1;
            diag.line = 0;
            diag.column = 0;
            diag.length = std::max(1, static_cast<int>(importStmt->moduleName.size()));

            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string importPrefix = "import " + importStmt->moduleName;
                const size_t column = lines[i].find(importPrefix);
                if (column != std::string::npos) {
                    diag.line = static_cast<int>(i);
                    diag.column = static_cast<int>(column + 7);
                    break;
                }
            }
            diagnostics.push_back(diag);
            continue;
        }

        for (const auto& symbolName : importStmt->importedSymbols) {
            if (loaded->get().globalScope.lookup(symbolName)) {
                continue;
            }

            Diagnostic diag;
            diag.message = "Unresolved imported symbol '" + symbolName + "' from module '" + importStmt->moduleName + "'";
            diag.severity = 1;
            diag.line = 0;
            diag.column = 0;
            diag.length = std::max(1, static_cast<int>(symbolName.size()));

            for (size_t i = 0; i < lines.size(); ++i) {
                const size_t column = lines[i].find(symbolName);
                if (column != std::string::npos && lines[i].find("import " + importStmt->moduleName) != std::string::npos) {
                    diag.line = static_cast<int>(i);
                    diag.column = static_cast<int>(column);
                    break;
                }
            }
            diagnostics.push_back(diag);
        }
    }

    return diagnostics;
}

std::vector<CompletionItem> Server::collectCompletionItems(const DocumentState& doc, int line, int character) {
    std::vector<CompletionItem> items;
    std::set<std::string> seen;
    std::set<std::string> seenLabels;
    const detail::CompletionContext ctx = detail::completionContextAt(doc.text, line, character);

    if (ctx.kind == detail::CompletionContext::Kind::Import) {
        const size_t dotPos = ctx.prefix.find('.');
        if (dotPos != std::string::npos) {
            std::string moduleName = detail::trim(ctx.prefix.substr(0, dotPos));
            std::string symbolPrefix = detail::trim(ctx.prefix.substr(dotPos + 1));
            if (!symbolPrefix.empty() && symbolPrefix.front() == '{') {
                symbolPrefix.erase(symbolPrefix.begin());
                symbolPrefix = detail::trim(symbolPrefix);
            }

            auto imported = ensureModuleLoaded(moduleName, &doc);
            if (imported) {
                for (const auto& [name, sym] : imported->get().globalScope.getGlobalSymbols()) {
                    if (!symbolPrefix.empty() && name.rfind(symbolPrefix, 0) != 0) {
                        continue;
                    }
                    detail::addUniqueCompletion(items, seen, name, detail::completionKindForSymbol(sym), sym.type, name);
                }
            }
            return items;
        }

        for (const auto& moduleName : collectAvailableModules(&doc)) {
            if (!ctx.prefix.empty() && moduleName.rfind(ctx.prefix, 0) != 0) {
                continue;
            }
            detail::addUniqueCompletion(items, seen, moduleName, 9, "module", moduleName);
        }
        return items;
    }

    if (ctx.kind == detail::CompletionContext::Kind::Member) {
        auto imported = ensureModuleLoaded(ctx.qualifier, &doc);
        if (!imported) {
            if (doc.ast) {
                for (const auto& stmt : doc.ast->body) {
                    auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
                    if (!importStmt) {
                        continue;
                    }
                    const std::string alias = importStmt->alias.empty() ? importStmt->moduleName : importStmt->alias;
                    if (alias == ctx.qualifier) {
                        imported = ensureModuleLoaded(importStmt->moduleName, &doc);
                        break;
                    }
                }
            }
        }

        if (imported) {
            for (const auto& [name, sym] : imported->get().globalScope.getGlobalSymbols()) {
                if (!ctx.prefix.empty() && name.rfind(ctx.prefix, 0) != 0) {
                    continue;
                }
                detail::addUniqueCompletion(items, seen, name, detail::completionKindForSymbol(sym), sym.type, name);
            }
        }
        return items;
    }

    const std::vector<std::string> keywords = {
        "module", "import", "fun", "struct", "enum", "class", "if",
        "else", "while", "for", "return", "break", "skip", "when",
        "switch", "loop", "unsafe", "new", "delete", "cast", "true",
        "false", "const", "let", "this"
    };
    for (const auto& keyword : keywords) {
        detail::addUniqueCompletion(items, seen, keyword, 14, "keyword", keyword);
    }

    const std::vector<std::string> types = {
        "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64",
        "u128", "f16", "f32", "f64", "f128", "bool", "text", "void"
    };
    for (const auto& type : types) {
        detail::addUniqueCompletion(items, seen, type, 25, "type", type);
    }

    const std::vector<std::string> builtins = {
        "@syscall", "@sizeof", "@typeof", "@alignof", "@offsetof",
        "@readFile", "@bitcast", "@inttoptr", "@ptrtoint"
    };
    for (const auto& builtin : builtins) {
        detail::addUniqueCompletion(items, seen, builtin, 3, "builtin", builtin);
    }

    for (const auto& symbolRef : visibleSemanticSymbolsAt(doc, line, character, ctx.prefix)) {
        const auto& symbol = symbolRef.get();
        detail::addUniqueCompletion(items, seen, symbol.name, completionKindForSemanticSymbol(symbol), symbol.type, symbol.name);
        seenLabels.insert(symbol.name);
    }

    for (const auto& [name, sym] : doc.globalScope.getGlobalSymbols()) {
        if (seenLabels.find(name) != seenLabels.end()) {
            continue;
        }
        if (!ctx.prefix.empty() && name.rfind(ctx.prefix, 0) != 0) {
            continue;
        }
        detail::addUniqueCompletion(items, seen, name, detail::completionKindForSymbol(sym), sym.type, name);
    }

    if (doc.ast) {
        for (const auto& stmt : doc.ast->body) {
            auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
            if (!importStmt) {
                continue;
            }

            const std::string alias = importStmt->alias.empty() ? importStmt->moduleName : importStmt->alias;
            if (seenLabels.find(alias) == seenLabels.end()) {
                detail::addUniqueCompletion(items, seen, alias, 9, "module", alias);
            }

            if (!importStmt->importedSymbols.empty()) {
                auto imported = ensureModuleLoaded(importStmt->moduleName, &doc);
                if (!imported) {
                    continue;
                }
                for (const auto& symbolName : importStmt->importedSymbols) {
                    if (seenLabels.find(symbolName) != seenLabels.end()) {
                        continue;
                    }
                    if (!ctx.prefix.empty() && symbolName.rfind(ctx.prefix, 0) != 0) {
                        continue;
                    }
                    if (auto sym = imported->get().globalScope.lookup(symbolName)) {
                        detail::addUniqueCompletion(items, seen, symbolName, detail::completionKindForSymbol(*sym),
                                                    "imported from " + importStmt->moduleName, symbolName);
                    }
                }
            }

            if (!importStmt->isWildcard) {
                continue;
            }

            auto imported = ensureModuleLoaded(importStmt->moduleName, &doc);
            if (!imported) {
                continue;
            }
            for (const auto& [name, sym] : imported->get().globalScope.getGlobalSymbols()) {
                if (seenLabels.find(name) != seenLabels.end()) {
                    continue;
                }
                if (!ctx.prefix.empty() && name.rfind(ctx.prefix, 0) != 0) {
                    continue;
                }
                detail::addUniqueCompletion(items, seen, name, detail::completionKindForSymbol(sym), "imported from " + importStmt->moduleName, name);
            }
        }
    }

    return items;
}

std::vector<Location> Server::collectReferences(const DocumentState& doc, int line, int character, bool includeDeclaration) {
    std::vector<Location> references;

    auto definition = findDefinitionLocation(doc, line, character);
    auto name = detail::identifierAt(doc.text, line, character);
    if (!definition || !name) {
        return references;
    }

    loadWorkspaceDocuments();

    std::set<std::string> seen;
    for (const auto& [uri, candidateDoc] : documents) {
        Lexer lexer;
        auto tokens = lexer.tokenize(candidateDoc.text);
        for (const auto& token : tokens) {
            if (token.type != TokenType::Identifier || token.value != *name) {
                continue;
            }

            const int tokenLine = std::max(0, token.line - 1);
            const int tokenColumn = std::max(0, token.column - 1);
            auto resolved = findDefinitionLocation(candidateDoc, tokenLine, tokenColumn);
            if (!resolved || !detail::sameLocation(*resolved, *definition)) {
                continue;
            }

            Location location{
                candidateDoc.uri,
                tokenLine,
                tokenColumn,
                std::max(1, static_cast<int>(token.value.size()))
            };
            if (!includeDeclaration && detail::sameLocation(location, *definition)) {
                continue;
            }

            const std::string key = location.uri + ":" + std::to_string(location.line) + ":" + std::to_string(location.column);
            if (seen.insert(key).second) {
                references.push_back(location);
            }
        }
    }

    return references;
}

std::optional<Location> Server::prepareRenameLocation(const DocumentState& doc, int line, int character) {
    auto range = detail::identifierRangeAt(doc.text, line, character);
    if (!range) {
        return std::nullopt;
    }

    auto definition = findDefinitionLocation(doc, line, character);
    if (!definition) {
        return std::nullopt;
    }

    const size_t startOffset = range->first;
    const size_t endOffset = range->second;
    const int startLine = line;
    const int startColumn = static_cast<int>(range->first - detail::offsetForPosition(doc.text, line, 0));
    const int length = std::max(1, static_cast<int>(endOffset - startOffset));

    return Location{doc.uri, startLine, startColumn, length};
}

std::optional<Location> Server::findDefinitionLocation(const DocumentState& doc, int line, int character) {
    if (auto semantic = detail::semanticSymbolAt(doc, line, character)) {
        return semantic->get().declaration;
    }

    auto qualified = detail::qualifiedReferenceAt(doc.text, line, character);
    if (qualified) {
        return findImportedLocation(doc, qualified->first, qualified->second);
    }

    auto identRange = detail::identifierRangeAt(doc.text, line, character);
    if (!identRange) {
        return std::nullopt;
    }

    const std::string name = doc.text.substr(identRange->first, identRange->second - identRange->first);
    if (auto local = doc.globalScope.lookup(name)) {
        return symbolLocationFromDoc(doc, *local);
    }

    if (doc.ast) {
        for (const auto& stmt : doc.ast->body) {
            auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
            if (!importStmt) {
                continue;
            }
            if (detail::hasDirectImport(*importStmt, name)) {
                if (auto imported = findImportedLocation(doc, importStmt->moduleName, name)) {
                    return imported;
                }
            }
            if (!importStmt->isWildcard) {
                continue;
            }
            if (auto imported = findImportedLocation(doc, importStmt->moduleName, name)) {
                return imported;
            }
        }
    }

    return std::nullopt;
}

std::optional<Symbol> Server::findHoverSymbol(const DocumentState& doc, int line, int character) {
    if (auto semantic = detail::semanticSymbolAt(doc, line, character)) {
        const auto& symbol = semantic->get();
        return Symbol(
            symbol.name,
            symbol.type,
            false,
            symbol.isParameter,
            symbol.declaration.line + 1,
            symbol.declaration.column + 1,
            symbol.declaration.length,
            "",
            symbol.preview
        );
    }

    auto qualified = detail::qualifiedReferenceAt(doc.text, line, character);
    if (qualified) {
        return findImportedSymbol(doc, qualified->first, qualified->second);
    }

    auto identRange = detail::identifierRangeAt(doc.text, line, character);
    if (!identRange) {
        return std::nullopt;
    }

    const std::string name = doc.text.substr(identRange->first, identRange->second - identRange->first);
    if (auto local = doc.globalScope.lookup(name)) {
        return *local;
    }

    if (doc.ast) {
        for (const auto& stmt : doc.ast->body) {
            auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
            if (!importStmt) {
                continue;
            }
            if (detail::hasDirectImport(*importStmt, name)) {
                if (auto imported = findImportedSymbol(doc, importStmt->moduleName, name)) {
                    return imported;
                }
            }
            if (!importStmt->isWildcard) {
                continue;
            }
            if (auto imported = findImportedSymbol(doc, importStmt->moduleName, name)) {
                return imported;
            }
        }
    }

    return std::nullopt;
}

std::optional<Symbol> Server::findImportedSymbol(const DocumentState& doc, const std::string& qualifier, const std::string& name) {
    if (!doc.ast) {
        return std::nullopt;
    }

    for (const auto& stmt : doc.ast->body) {
        auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
        if (!importStmt) {
            continue;
        }

        const std::string alias = importStmt->alias.empty() ? importStmt->moduleName : importStmt->alias;
        if (qualifier != alias && qualifier != importStmt->moduleName) {
            continue;
        }

        auto imported = ensureModuleLoaded(importStmt->moduleName, &doc);
        if (!imported) {
            return std::nullopt;
        }

        return imported->get().globalScope.lookup(name);
    }

    return std::nullopt;
}

std::optional<Location> Server::findImportedLocation(const DocumentState& doc, const std::string& qualifier, const std::string& name) {
    if (!doc.ast) {
        return std::nullopt;
    }

    for (const auto& stmt : doc.ast->body) {
        auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
        if (!importStmt) {
            continue;
        }

        const std::string alias = importStmt->alias.empty() ? importStmt->moduleName : importStmt->alias;
        if (qualifier != alias && qualifier != importStmt->moduleName) {
            continue;
        }

        auto imported = ensureModuleLoaded(importStmt->moduleName, &doc);
        if (!imported) {
            return std::nullopt;
        }

        if (auto sym = imported->get().globalScope.lookup(name)) {
            return symbolLocationFromDoc(imported->get(), *sym);
        }
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<Location> Server::symbolLocationFromDoc(const DocumentState& doc, const Symbol& symbol) const {
    if (symbol.declarationLine <= 0) {
        return std::nullopt;
    }
    return detail::locationFromSymbol(doc, symbol);
}

} // namespace LSP

#include <lsp/lsp_internal.hpp>

#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <utilities/errors.hpp>
#include <utilities/utils.hpp>

#include <algorithm>
#include <set>

namespace fs = std::filesystem;

namespace LSP {

namespace {

class SemanticIndexer {
public:
    explicit SemanticIndexer(const DocumentState& doc)
        : doc_(doc) {
        Lexer lexer;
        tokens_ = lexer.tokenize(doc.text);
    }

    void build(std::vector<SemanticSymbol>& symbols, std::vector<SemanticReference>& references) {
        symbols_.clear();
        references_.clear();
        scopes_.clear();

        if (!doc_.ast) {
            symbols.clear();
            references.clear();
            return;
        }

        pushScope(true, 0, static_cast<int>(doc_.text.size()));
        predeclareBlock(doc_.ast->body);
        for (const auto& stmt : doc_.ast->body) {
            visitExpr(stmt);
        }

        symbols = symbols_;
        references = references_;
    }

private:
    struct ScopeFrame {
        bool isGlobal = false;
        int startOffset = 0;
        int endOffset = 0;
        std::map<std::string, std::vector<int>> symbolsByName;
    };

    const DocumentState& doc_;
    std::vector<Token> tokens_;
    std::vector<SemanticSymbol> symbols_;
    std::vector<SemanticReference> references_;
    std::vector<ScopeFrame> scopes_;

    void pushScope(bool isGlobal, int startOffset, int endOffset) {
        scopes_.push_back(ScopeFrame{isGlobal, startOffset, endOffset, {}});
    }

    void popScope() {
        if (!scopes_.empty()) {
            scopes_.pop_back();
        }
    }

    int declareSymbol(const std::string& name, const std::string& type, const Location& declaration,
                      int declarationOffset, const std::string& preview, bool isParameter, bool isGlobal,
                      bool isHoisted) {
        SemanticSymbol symbol;
        symbol.id = static_cast<int>(symbols_.size());
        symbol.name = name;
        symbol.type = type;
        symbol.preview = preview;
        symbol.declaration = declaration;
        symbol.declarationOffset = declarationOffset;
        if (!scopes_.empty()) {
            symbol.scopeStartOffset = scopes_.back().startOffset;
            symbol.scopeEndOffset = scopes_.back().endOffset;
        }
        symbol.isParameter = isParameter;
        symbol.isGlobal = isGlobal;
        symbol.isHoisted = isHoisted;
        symbols_.push_back(symbol);
        if (!scopes_.empty()) {
            scopes_.back().symbolsByName[name].push_back(symbol.id);
        }
        return symbol.id;
    }

    std::optional<int> lookupSymbol(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->symbolsByName.find(name);
            if (found != it->symbolsByName.end() && !found->second.empty()) {
                return found->second.back();
            }
        }
        return std::nullopt;
    }

    int nodeStartOffset(const AST::ExprAST& node) const {
        return std::max(0, node.range.startOffset);
    }

    int nodeEndOffset(const AST::ExprAST& node) const {
        if (node.range.endOffset >= 0) {
            return node.range.endOffset;
        }
        return static_cast<int>(doc_.text.size());
    }

    std::optional<size_t> findIdentifierTokenIndex(const std::string& name, int startOffset, int endOffset, size_t fromIndex = 0) const {
        for (size_t i = fromIndex; i < tokens_.size(); ++i) {
            const auto& token = tokens_[i];
            if (token.start < startOffset) {
                continue;
            }
            if (token.start > endOffset) {
                break;
            }
            if (token.type == TokenType::Identifier && token.value == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    std::optional<Location> findIdentifierLocation(const std::string& name, int startOffset, int endOffset, size_t fromIndex = 0, size_t* foundIndex = nullptr) const {
        auto tokenIndex = findIdentifierTokenIndex(name, startOffset, endOffset, fromIndex);
        if (!tokenIndex) {
            return std::nullopt;
        }
        if (foundIndex) {
            *foundIndex = *tokenIndex;
        }
        return detail::locationFromToken(doc_.uri, tokens_[*tokenIndex]);
    }

    int locationOffset(const Location& location) const {
        return static_cast<int>(detail::offsetForPosition(doc_.text, location.line, location.column));
    }

    void predeclareBlock(const std::vector<std::shared_ptr<AST::ExprAST>>& body) {
        for (const auto& stmt : body) {
            auto fn = AST::ast_cast<AST::FunctionDeclaration>(stmt);
            if (!fn) {
                continue;
            }

            const int headerEndOffset =
                !fn->body.empty() ? std::max(nodeStartOffset(*fn), fn->body.front()->range.startOffset) : nodeEndOffset(*fn);
            auto declaration = findIdentifierLocation(fn->name, nodeStartOffset(*fn), headerEndOffset);
            if (!declaration) {
                continue;
            }

            declareSymbol(
                fn->name,
                fn->returnType,
                *declaration,
                locationOffset(*declaration),
                "fun " + fn->name + "(...) -> " + fn->returnType,
                false,
                scopes_.size() == 1,
                true);
        }
    }

    void declareVariable(const AST::VariableDeclarationExpr& varDecl) {
        const int startOffset = nodeStartOffset(varDecl);
        const int endOffset = nodeEndOffset(varDecl);
        auto declaration = findIdentifierLocation(varDecl.identifier, startOffset, endOffset);
        if (!declaration) {
            return;
        }

        const bool isGlobal = scopes_.size() == 1;
        declareSymbol(
            varDecl.identifier,
            varDecl.typeHint,
            *declaration,
            locationOffset(*declaration),
            varDecl.identifier + ": " + varDecl.typeHint,
            false,
            isGlobal,
            false);
    }

    void declareFunctionParameters(const AST::FunctionDeclaration& fnDecl) {
        const int headerStartOffset = nodeStartOffset(fnDecl);
        const int headerEndOffset =
            !fnDecl.body.empty() ? std::max(headerStartOffset, fnDecl.body.front()->range.startOffset) : nodeEndOffset(fnDecl);

        size_t cursor = 0;
        if (auto fnNameIndex = findIdentifierTokenIndex(fnDecl.name, headerStartOffset, headerEndOffset)) {
            cursor = *fnNameIndex + 1;
        }

        for (const auto& param : fnDecl.parameters) {
            size_t foundIndex = 0;
            auto declaration = findIdentifierLocation(param.name, headerStartOffset, headerEndOffset, cursor, &foundIndex);
            if (!declaration) {
                continue;
            }
            cursor = foundIndex + 1;
            declareSymbol(
                param.name,
                param.type,
                *declaration,
                locationOffset(*declaration),
                param.name + ": " + param.type,
                true,
                false,
                false);
        }
    }

    void addReference(const AST::IdentifierExpr& identifier) {
        auto symbolId = lookupSymbol(identifier.name);
        if (!symbolId || identifier.range.startLine <= 0) {
            return;
        }

        references_.push_back(SemanticReference{
            *symbolId,
            Location{
                doc_.uri,
                std::max(0, identifier.range.startLine - 1),
                std::max(0, identifier.range.startColumn - 1),
                std::max(1, static_cast<int>(identifier.name.size()))
            }
        });
    }

    void visitBlock(const std::vector<std::shared_ptr<AST::ExprAST>>& body, bool createScope) {
        if (createScope) {
            int startOffset = 0;
            int endOffset = static_cast<int>(doc_.text.size());
            if (!body.empty()) {
                startOffset = std::max(0, body.front()->range.startOffset);
                endOffset = body.back()->range.endOffset >= 0 ? body.back()->range.endOffset : endOffset;
            }
            pushScope(false, startOffset, endOffset);
        }

        predeclareBlock(body);
        for (const auto& stmt : body) {
            visitExpr(stmt);
        }

        if (createScope) {
            popScope();
        }
    }

    void visitExpr(const std::shared_ptr<AST::ExprAST>& expr) {
        if (!expr) {
            return;
        }

        switch (expr->nodeType()) {
            case AST::NodeType::IdentifierExpr:
                addReference(static_cast<const AST::IdentifierExpr&>(*expr));
                return;
            case AST::NodeType::VariableDeclaration: {
                const auto& node = static_cast<const AST::VariableDeclarationExpr&>(*expr);
                if (node.initialValue) {
                    visitExpr(node.initialValue);
                }
                for (const auto& arg : node.constructorArgs) {
                    visitExpr(arg);
                }
                declareVariable(node);
                return;
            }
            case AST::NodeType::AssignmentExpr: {
                const auto& node = static_cast<const AST::AssignmentExpr&>(*expr);
                visitExpr(node.target);
                visitExpr(node.value);
                return;
            }
            case AST::NodeType::BinaryOperation: {
                const auto& node = static_cast<const AST::BinaryOperationExpr&>(*expr);
                visitExpr(node.lhs);
                visitExpr(node.rhs);
                return;
            }
            case AST::NodeType::ShiftOperation: {
                const auto& node = static_cast<const AST::ShiftOperationExpr&>(*expr);
                visitExpr(node.lhs);
                visitExpr(node.rhs);
                return;
            }
            case AST::NodeType::FunctionCall: {
                const auto& node = static_cast<const AST::FunctionCallExpr&>(*expr);
                visitExpr(node.callee);
                for (const auto& arg : node.arguments) {
                    visitExpr(arg);
                }
                return;
            }
            case AST::NodeType::WhenStatement: {
                const auto& node = static_cast<const AST::WhenStatement&>(*expr);
                visitExpr(node.condition);
                visitBlock(node.consequent, true);
                return;
            }
            case AST::NodeType::SwitchStatement: {
                const auto& node = static_cast<const AST::SwitchStatement&>(*expr);
                visitExpr(node.subject);
                for (const auto& arm : node.arms) {
                    for (const auto& pattern : arm.patterns) {
                        visitExpr(pattern);
                    }
                    visitBlock(arm.body, true);
                }
                return;
            }
            case AST::NodeType::WhileLoop: {
                const auto& node = static_cast<const AST::WhileLoop&>(*expr);
                visitExpr(node.condition);
                visitBlock(node.body, true);
                return;
            }
            case AST::NodeType::IfStatement: {
                const auto& node = static_cast<const AST::IfStatement&>(*expr);
                visitExpr(node.condition);
                visitBlock(node.consequent, true);
                visitBlock(node.alternate, true);
                return;
            }
            case AST::NodeType::FunctionDeclaration: {
                const auto& node = static_cast<const AST::FunctionDeclaration&>(*expr);
                pushScope(false, nodeStartOffset(node), nodeEndOffset(node));
                declareFunctionParameters(node);
                visitBlock(node.body, false);
                popScope();
                return;
            }
            case AST::NodeType::ArrayLiteral: {
                const auto& node = static_cast<const AST::ArrayLiteral&>(*expr);
                for (const auto& element : node.elements) {
                    visitExpr(element);
                }
                return;
            }
            case AST::NodeType::ObjectProperty:
                visitExpr(static_cast<const AST::ObjectProperty&>(*expr).value);
                return;
            case AST::NodeType::ObjectLiteral: {
                const auto& node = static_cast<const AST::ObjectLiteral&>(*expr);
                for (const auto& property : node.properties) {
                    visitExpr(property);
                }
                return;
            }
            case AST::NodeType::MemberAccess: {
                const auto& node = static_cast<const AST::MemberAccessExpr&>(*expr);
                visitExpr(node.object);
                if (node.computed) {
                    visitExpr(node.property);
                }
                return;
            }
            case AST::NodeType::ReturnStatement:
                visitExpr(static_cast<const AST::ReturnStatement&>(*expr).returnValue);
                return;
            case AST::NodeType::EqualityCheck: {
                const auto& node = static_cast<const AST::EqualityCheckExpr&>(*expr);
                visitExpr(node.left);
                visitExpr(node.right);
                return;
            }
            case AST::NodeType::LogicalOperation: {
                const auto& node = static_cast<const AST::LogicalOperationExpr&>(*expr);
                visitExpr(node.left);
                visitExpr(node.right);
                return;
            }
            case AST::NodeType::InfiniteLoop:
                visitBlock(static_cast<const AST::InfiniteLoop&>(*expr).body, true);
                return;
            case AST::NodeType::StructInstantiation: {
                const auto& node = static_cast<const AST::StructInstantiation&>(*expr);
                for (const auto& field : node.fieldValues) {
                    visitExpr(field.value);
                }
                return;
            }
            case AST::NodeType::AddressOfExpr:
                visitExpr(static_cast<const AST::AddressOfExpr&>(*expr).operand);
                return;
            case AST::NodeType::DereferenceExpr:
                visitExpr(static_cast<const AST::DereferenceExpr&>(*expr).operand);
                return;
            case AST::NodeType::BuiltinCall: {
                const auto& node = static_cast<const AST::BuiltinCallExpr&>(*expr);
                for (const auto& arg : node.arguments) {
                    visitExpr(arg);
                }
                return;
            }
            case AST::NodeType::CompileTimeIf: {
                const auto& node = static_cast<const AST::CompileTimeIfExpr&>(*expr);
                for (const auto& branch : node.branches) {
                    visitExpr(branch.condition);
                    visitBlock(branch.body, true);
                }
                return;
            }
            case AST::NodeType::CastExpr:
                visitExpr(static_cast<const AST::CastExpr&>(*expr).expression);
                return;
            case AST::NodeType::NewExpression: {
                const auto& node = static_cast<const AST::NewExpression&>(*expr);
                visitExpr(node.initializer);
                visitExpr(node.arraySize);
                return;
            }
            case AST::NodeType::DeleteExpression:
                visitExpr(static_cast<const AST::DeleteExpression&>(*expr).operand);
                return;
            case AST::NodeType::ClassDeclaration: {
                const auto& node = static_cast<const AST::ClassDeclaration&>(*expr);
                for (const auto& method : node.methods) {
                    int startOffset = nodeStartOffset(node);
                    int endOffset = nodeEndOffset(node);
                    if (!method.body.empty()) {
                        startOffset = std::max(0, method.body.front()->range.startOffset);
                        endOffset = method.body.back()->range.endOffset >= 0 ? method.body.back()->range.endOffset : endOffset;
                    }
                    pushScope(false, startOffset, endOffset);
                    visitBlock(method.body, false);
                    popScope();
                }
                return;
            }
            case AST::NodeType::ImplBlock: {
                const auto& node = static_cast<const AST::ImplBlock&>(*expr);
                for (const auto& method : node.methods) {
                    int startOffset = nodeStartOffset(node);
                    int endOffset = nodeEndOffset(node);
                    if (!method.body.empty()) {
                        startOffset = std::max(0, method.body.front()->range.startOffset);
                        endOffset = method.body.back()->range.endOffset >= 0 ? method.body.back()->range.endOffset : endOffset;
                    }
                    pushScope(false, startOffset, endOffset);
                    visitBlock(method.body, false);
                    popScope();
                }
                return;
            }
            default:
                return;
        }
    }
};

} // namespace

void Server::validateDocument(const std::string& uri) {
    auto current = getDocument(uri);
    if (!current) {
        return;
    }

    DocumentState& doc = current->get();
    detail::ParsedDocument parsed = detail::parseDocumentState(doc.text, doc.filePath.empty() ? uri : doc.filePath);

    clearDocumentIndex(doc);
    doc.ast = parsed.ast;
    doc.globalScope = parsed.scope;
    doc.moduleName = parsed.moduleName.empty() ? detail::inferModuleNameFromText(doc.text, fs::path(doc.filePath)) : parsed.moduleName;
    doc.imports = parsed.imports;
    doc.hasValidAST = parsed.hasValidAST;
    buildSemanticIndex(doc);
    indexDocument(doc);

    std::vector<Diagnostic> diagnostics = parsed.diagnostics;
    auto importDiagnostics = collectImportDiagnostics(doc);
    diagnostics.insert(diagnostics.end(), importDiagnostics.begin(), importDiagnostics.end());

    std::set<std::string> seen;
    std::vector<Diagnostic> deduped;
    for (const auto& diag : diagnostics) {
        const std::string key = std::to_string(diag.line) + ":" + std::to_string(diag.column) + ":" + diag.message;
        if (seen.insert(key).second) {
            deduped.push_back(diag);
        }
    }

    sendDiagnostics(uri, deduped);
}

void Server::reindexDocument(const std::string& uri, const std::string& text, bool fromDisk) {
    if (auto existing = getDocument(uri)) {
        clearDocumentIndex(existing->get());
    }

    DocumentState doc;
    doc.uri = uri;
    doc.filePath = uriToPath(uri);
    doc.text = text;
    doc.fromDisk = fromDisk;

    detail::ParsedDocument parsed = detail::parseDocumentState(doc.text, doc.filePath.empty() ? uri : doc.filePath);
    doc.ast = parsed.ast;
    doc.globalScope = parsed.scope;
    doc.moduleName = parsed.moduleName.empty() ? detail::inferModuleNameFromText(doc.text, fs::path(doc.filePath)) : parsed.moduleName;
    doc.imports = parsed.imports;
    doc.hasValidAST = parsed.hasValidAST;
    buildSemanticIndex(doc);

    documents[uri] = std::move(doc);
    indexDocument(documents[uri]);
}

void Server::buildSemanticIndex(DocumentState& doc) {
    doc.semanticSymbols.clear();
    doc.semanticReferences.clear();
    if (!doc.hasValidAST || !doc.ast) {
        return;
    }

    SemanticIndexer indexer(doc);
    indexer.build(doc.semanticSymbols, doc.semanticReferences);
}

std::optional<std::reference_wrapper<DocumentState>> Server::ensureModuleLoaded(const std::string& moduleName, const DocumentState* contextDoc) {
    auto mapped = moduleUriByName.find(moduleName);
    if (mapped != moduleUriByName.end()) {
        return getDocument(mapped->second);
    }

    auto resolvedPath = resolveModulePath(moduleName, contextDoc);
    if (!resolvedPath) {
        return std::nullopt;
    }

    const std::string canonical = canonicalPathString(*resolvedPath);
    auto knownPath = moduleUriByPath.find(canonical);
    if (knownPath != moduleUriByPath.end()) {
        return getDocument(knownPath->second);
    }

    const std::string text = Utilities::readFile(canonical);
    if (text.empty()) {
        return std::nullopt;
    }

    const std::string uri = pathToUri(canonical);
    reindexDocument(uri, text, true);
    return getDocument(uri);
}

} // namespace LSP

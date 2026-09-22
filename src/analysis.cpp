#include <lsp/lsp_internal.hpp>

#include <lexer/lexer.hpp>
#include <parser/parser.hpp>
#include <sema/sema.hpp>
#include <extra/type_system.hpp>
#include <utilities/errors.hpp>
#include <utilities/utils.hpp>

#include <algorithm>
#include <functional>
#include <map>
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

            std::string signature = "fun " + fn->name;
            if (!fn->genericParams.empty()) {
                signature += "<";
                for (size_t gi = 0; gi < fn->genericParams.size(); ++gi) {
                    if (gi > 0) signature += ", ";
                    signature += fn->genericParams[gi];
                }
                signature += ">";
            }
            signature += "(";
            for (size_t pi = 0; pi < fn->parameters.size(); ++pi) {
                if (pi > 0) signature += ", ";
                const auto& param = fn->parameters[pi];
                if (param.isVolatile) signature += "volatile ";
                signature += param.type;
                if (!param.name.empty()) signature += " " + param.name;
            }
            signature += ") -> " + fn->returnType;

            declareSymbol(
                fn->name,
                fn->returnType,
                *declaration,
                locationOffset(*declaration),
                signature,
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
            case AST::NodeType::WhileLoop: {
                const auto& node = static_cast<const AST::WhileLoop&>(*expr);
                visitExpr(node.condition);
                visitBlock(node.body, true);
                return;
            }
            case AST::NodeType::ForLoop: {
                const auto& node = static_cast<const AST::ForLoop&>(*expr);
                // The loop variable is scoped to the whole `for` construct.
                pushScope(false, nodeStartOffset(node), nodeEndOffset(node));
                const int headerStart = nodeStartOffset(node);
                const int headerEnd = !node.body.empty()
                    ? std::max(headerStart, node.body.front()->range.startOffset)
                    : nodeEndOffset(node);
                if (auto decl = findIdentifierLocation(node.varName, headerStart, headerEnd)) {
                    declareSymbol(node.varName, "", *decl, locationOffset(*decl),
                                  node.varName + " (loop variable)", true, false, false);
                }
                if (node.isRange) {
                    visitExpr(node.rangeStart);
                    visitExpr(node.rangeEnd);
                } else {
                    visitExpr(node.iterable);
                }
                visitBlock(node.body, false);
                popScope();
                return;
            }
            case AST::NodeType::SwitchStatement: {
                const auto& node = static_cast<const AST::SwitchStatement&>(*expr);
                visitExpr(node.subject);
                // Bindings appear in each arm's pattern before its body. Scan the
                // token stream forward per arm to locate them (arms carry no range).
                int cursor = nodeStartOffset(node);
                for (const auto& arm : node.arms) {
                    int bodyStart = !arm.body.empty()
                        ? std::max(cursor, arm.body.front()->range.startOffset)
                        : nodeEndOffset(node);
                    int bodyEnd = !arm.body.empty() && arm.body.back()->range.endOffset >= 0
                        ? arm.body.back()->range.endOffset
                        : nodeEndOffset(node);
                    pushScope(false, cursor, bodyEnd);
                    for (const auto& binding : arm.bindings) {
                        size_t foundIndex = 0;
                        auto decl = findIdentifierLocation(binding, cursor, bodyStart,
                                                           0, &foundIndex);
                        if (decl) {
                            declareSymbol(binding, "", *decl, locationOffset(*decl),
                                          binding + " (switch binding)", true, false, false);
                        }
                    }
                    visitBlock(arm.body, false);
                    popScope();
                    cursor = std::max(cursor, bodyEnd);
                }
                return;
            }
            case AST::NodeType::SliceExpr: {
                const auto& node = static_cast<const AST::SliceExpr&>(*expr);
                visitExpr(node.object);
                visitExpr(node.start);
                visitExpr(node.end);
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
            case AST::NodeType::DestructureStatement: {
                const auto& node = static_cast<const AST::DestructureStatement&>(*expr);
                visitExpr(node.value);
                const int startOff = nodeStartOffset(node);
                const int endOff = nodeEndOffset(node);
                for (const auto& b : node.bindings) {
                    auto declLoc = findIdentifierLocation(b.name, startOff, endOff);
                    if (declLoc) {
                        declareSymbol(b.name, b.typeHint.empty() ? "variable" : b.typeHint,
                                      *declLoc, locationOffset(*declLoc), b.name + ": " + b.typeHint,
                                      false, false, false);
                    }
                }
                return;
            }
            case AST::NodeType::TupleLiteral: {
                const auto& node = static_cast<const AST::TupleLiteral&>(*expr);
                for (const auto& elem : node.elements) visitExpr(elem);
                return;
            }
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
    auto semanticDiagnostics = collectSemanticDiagnostics(doc);
    diagnostics.insert(diagnostics.end(), semanticDiagnostics.begin(), semanticDiagnostics.end());
    auto importDiagnostics = collectImportDiagnostics(doc);
    diagnostics.insert(diagnostics.end(), importDiagnostics.begin(), importDiagnostics.end());
    auto unusedDiagnostics = collectUnusedDiagnostics(doc);
    diagnostics.insert(diagnostics.end(), unusedDiagnostics.begin(), unusedDiagnostics.end());

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

namespace {

// The exported symbols of a module, with types interned in a shared TypeContext.
struct ModuleExports {
    std::vector<Sema::FunctionInfo> functions;
    std::vector<Sema::StructInfo> structs;
    std::vector<Sema::ClassInfo> classes;
    std::vector<Sema::EnumInfo> enums;
    std::vector<AST::ClassDeclaration*> classTemplates;
    std::vector<AST::FunctionDeclaration*> functionTemplates;
    std::vector<Sema::SumTypeInfo> sumTypes;
    std::vector<std::pair<std::string, std::string>> typeAliases;
};

// Accumulate `src`'s exported symbols into `dst`.
void appendExports(ModuleExports& dst, const Sema::SemaResult& src) {
    for (const auto& fn : src.functions) if (fn.isExported) dst.functions.push_back(fn);
    for (const auto& s : src.structs) if (s.isExported) dst.structs.push_back(s);
    for (const auto& c : src.classes) if (c.isExported) dst.classes.push_back(c);
    for (const auto& e : src.enums) if (e.isExported) dst.enums.push_back(e);
    for (auto* t : src.genericClassTemplates) if (t && t->isExported) dst.classTemplates.push_back(t);
    for (auto* t : src.genericFunctionTemplates) if (t && t->isExported) dst.functionTemplates.push_back(t);
    for (const auto& st : src.sumTypes) if (st.isExported) dst.sumTypes.push_back(st);
    for (const auto& ta : src.exportedTypeAliases) dst.typeAliases.push_back(ta);
}

} // namespace

std::vector<Diagnostic> Server::collectSemanticDiagnostics(DocumentState& doc) {
    std::vector<Diagnostic> diagnostics;
    if (!doc.hasValidAST || !doc.ast) {
        return diagnostics;
    }

    // A single TypeContext is shared across the transitive imported-module
    // analyses and the current document's analysis so their TypeRefs are
    // compatible (matching how the compiler driver analyzes a multi-module build).
    Types::TypeContext types;

    // Build the transitive import closure in dependency order (post-order DFS,
    // deduped by module URI, cycle-safe). Dependencies precede the modules that
    // import them, matching the driver's compile order.
    std::vector<DocumentState*> order;
    std::set<std::string> visited;
    std::function<void(DocumentState&)> visit = [&](DocumentState& mod) {
        if (!mod.ast) return;
        if (!visited.insert(mod.uri).second) return;  // already queued / cycle
        for (const auto& stmt : mod.ast->body) {
            auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
            if (!importStmt) continue;
            auto loaded = ensureModuleLoaded(importStmt->moduleName, &mod);
            if (loaded && loaded->get().ast) visit(loaded->get());
        }
        order.push_back(&mod);  // deps first
    };
    for (const auto& stmt : doc.ast->body) {
        auto importStmt = AST::ast_cast<AST::ImportStatement>(stmt);
        if (!importStmt) continue;
        auto loaded = ensureModuleLoaded(importStmt->moduleName, &doc);
        if (loaded && loaded->get().ast) visit(loaded->get());
    }

    // Accumulate exported symbols in dependency order, analyzing each module with
    // everything accumulated so far. The driver accumulates all exported symbols
    // across the whole build into one flat list handed to each subsequent module,
    // so a generic template body instantiated at an importer resolves symbols from
    // modules the importer did not directly import (e.g. StringMap<i64>'s body
    // calls std::str.hash even though the importer only imported std::map). Each
    // module is folded exactly once, so no symbol is duplicated.
    ModuleExports acc;
    for (DocumentState* mod : order) {
        Sema::SemaResult result;
        ErrorReporting::initErrorReporter(mod->text, mod->filePath.empty() ? mod->uri : mod->filePath);
        try {
            Sema::Analyzer analyzer(types, ErrorReporting::globalErrorReporter.get());
            result = analyzer.analyze(mod->ast, acc.functions, acc.structs, acc.classes,
                                      acc.enums, acc.classTemplates, acc.functionTemplates,
                                      acc.sumTypes, {}, acc.typeAliases);
        } catch (...) {
        }
        ErrorReporting::cleanupErrorReporter();
        appendExports(acc, result);
    }

    // Analyze the current document with the full accumulation, and harvest only
    // its own diagnostics.
    ErrorReporting::initErrorReporter(doc.text, doc.filePath.empty() ? doc.uri : doc.filePath);
    try {
        Sema::Analyzer analyzer(types, ErrorReporting::globalErrorReporter.get());
        analyzer.analyze(doc.ast, acc.functions, acc.structs, acc.classes,
                         acc.enums, acc.classTemplates, acc.functionTemplates,
                         acc.sumTypes, {}, acc.typeAliases);
    } catch (...) {
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
            diagnostics.push_back(converted);
        }
    }
    ErrorReporting::cleanupErrorReporter();

    return diagnostics;
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


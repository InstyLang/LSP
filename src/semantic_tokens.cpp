#include <lsp/lsp_internal.hpp>

#include <lexer/lexer.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <utility>

namespace LSP {

namespace detail {

// Token-type legend indices. Must match the order of semanticTokenTypes().
enum : int {
    ST_NAMESPACE = 0,
    ST_TYPE,
    ST_CLASS,
    ST_ENUM,
    ST_STRUCT,
    ST_TYPEPARAM,
    ST_PARAMETER,
    ST_VARIABLE,
    ST_PROPERTY,
    ST_ENUMMEMBER,
    ST_FUNCTION,
    ST_METHOD,
    ST_KEYWORD,
    ST_NUMBER,
    ST_STRING,
    ST_MACRO,
    ST_OPERATOR
};

// Modifier bit flags. Must match the order of semanticTokenModifiers().
enum : int {
    MOD_DECLARATION   = 1 << 0,
    MOD_DEFINITION    = 1 << 1,
    MOD_READONLY      = 1 << 2,
    MOD_DEFAULTLIB    = 1 << 3,
    MOD_MODIFICATION  = 1 << 4,
    MOD_DOCUMENTATION = 1 << 5,
    MOD_UNUSED        = 1 << 6
};

const std::vector<std::string>& semanticTokenTypes() {
    static const std::vector<std::string> kTypes = {
        "namespace", "type", "class", "enum", "struct", "typeParameter",
        "parameter", "variable", "property", "enumMember", "function", "method",
        "keyword", "number", "string", "macro", "operator"
    };
    return kTypes;
}

const std::vector<std::string>& semanticTokenModifiers() {
    static const std::vector<std::string> kMods = {
        "declaration", "definition", "readonly", "defaultLibrary",
        "modification", "documentation", "unused"
    };
    return kMods;
}

namespace {

bool isKeywordToken(TokenType type) {
    return type >= TokenType::KwModule && type <= TokenType::KwFalse;
}

struct Class {
    int type = ST_VARIABLE;
    int mods = 0;
};

} // namespace

std::vector<int> computeSemanticTokens(const DocumentState& doc) {
    std::vector<int> data;
    if (doc.text.empty()) {
        return data;
    }

    Lexer lexer;
    const std::vector<Token> tokens = lexer.tokenize(doc.text);

    // --- Name sets from the AST (used to classify uses of user-defined names) ---
    std::map<std::string, int> typeKind;   // name -> ST_CLASS / ST_ENUM / ST_STRUCT
    std::set<std::string> enumVariantNames;
    std::set<std::string> functionNames;
    std::set<std::string> methodNames;
    std::set<std::string> fieldNames;
    std::set<std::string> genericParamNames;

    auto addType = [&](const std::string& name, int kind) {
        if (!name.empty()) typeKind[name] = kind;
    };

    if (doc.ast) {
        for (const auto& stmt : doc.ast->body) {
            if (!stmt) continue;
            switch (stmt->nodeType()) {
                case AST::NodeType::FunctionDeclaration: {
                    const auto& f = static_cast<const AST::FunctionDeclaration&>(*stmt);
                    functionNames.insert(f.name);
                    for (const auto& g : f.genericParams) genericParamNames.insert(g);
                    break;
                }
                case AST::NodeType::StructDeclaration: {
                    const auto& s = static_cast<const AST::StructDeclaration&>(*stmt);
                    addType(s.name, ST_STRUCT);
                    for (const auto& g : s.genericParams) genericParamNames.insert(g);
                    for (const auto& field : s.fields) fieldNames.insert(field.name);
                    break;
                }
                case AST::NodeType::ClassDeclaration: {
                    const auto& c = static_cast<const AST::ClassDeclaration&>(*stmt);
                    addType(c.name, ST_CLASS);
                    for (const auto& g : c.genericParams) genericParamNames.insert(g);
                    for (const auto& field : c.fields) fieldNames.insert(field.name);
                    for (const auto& method : c.methods) methodNames.insert(method.name);
                    break;
                }
                case AST::NodeType::EnumDeclaration: {
                    const auto& e = static_cast<const AST::EnumDeclaration&>(*stmt);
                    addType(e.name, ST_ENUM);
                    for (const auto& v : e.variants) enumVariantNames.insert(v.name);
                    break;
                }
                case AST::NodeType::TypeAliasDeclaration: {
                    const auto& ta = static_cast<const AST::TypeAliasDeclaration&>(*stmt);
                    addType(ta.name, ST_TYPE);
                    break;
                }
                default:
                    break;
            }
        }
    }

    // Pull in this module's (and any resolved) global declarations so imported and
    // forward-declared names still classify.
    for (const auto& [name, sym] : doc.globalScope.getGlobalSymbols()) {
        if (sym.type == "struct") addType(name, ST_STRUCT);
        else if (sym.type == "class") addType(name, ST_CLASS);
        else if (sym.type == "enum") addType(name, ST_ENUM);
        else if (looksLikeFunction(sym)) functionNames.insert(name);
    }

    // --- Position map from the semantic index (accurate for locals/params/funcs) ---
    auto kindOfSymbol = [](const SemanticSymbol& s) -> int {
        if (!s.preview.empty() && s.preview.rfind("fun ", 0) == 0) return ST_FUNCTION;
        if (s.isParameter) return ST_PARAMETER;
        return ST_VARIABLE;
    };

    // Track usage count per symbol to identify unused variables/parameters
    std::vector<int> refCount(doc.semanticSymbols.size(), 0);
    for (const auto& r : doc.semanticReferences) {
        if (r.symbolId >= 0 && r.symbolId < static_cast<int>(doc.semanticSymbols.size())) {
            refCount[r.symbolId]++;
        }
    }

    std::map<std::pair<int, int>, Class> posMap;  // (line0, col0) -> classification
    for (size_t idx = 0; idx < doc.semanticSymbols.size(); ++idx) {
        const auto& s = doc.semanticSymbols[idx];
        int mods = MOD_DECLARATION;
        // If it's a variable or parameter with zero references (and not special like 'main' or leading underscore), mark as UNUSED
        if (!s.name.empty() && s.name[0] != '_' && s.name != "main" && (s.isParameter || kindOfSymbol(s) == ST_VARIABLE)) {
            if (refCount[idx] == 0) {
                mods |= MOD_UNUSED;
            }
        }
        posMap[{s.declaration.line, s.declaration.column}] = Class{kindOfSymbol(s), mods};
    }
    for (const auto& r : doc.semanticReferences) {
        if (r.symbolId >= 0 && r.symbolId < static_cast<int>(doc.semanticSymbols.size())) {
            posMap.emplace(std::make_pair(r.location.line, r.location.column),
                           Class{kindOfSymbol(doc.semanticSymbols[r.symbolId]), 0});
        }
    }

    auto nextNonNewline = [&](size_t i) -> int {
        for (size_t j = i + 1; j < tokens.size(); ++j) {
            if (tokens[j].type != TokenType::Newline) return static_cast<int>(j);
        }
        return -1;
    };

    // --- Declaration name positions from keyword context (struct/class/enum/fun N) ---
    std::map<std::pair<int, int>, Class> declByPos;
    for (size_t i = 0; i < tokens.size(); ++i) {
        int declType = -1;
        switch (tokens[i].type) {
            case TokenType::KwStruct: declType = ST_STRUCT; break;
            case TokenType::KwClass:  declType = ST_CLASS; break;
            case TokenType::KwEnum:   declType = ST_ENUM; break;
            case TokenType::KwType:   declType = ST_TYPE; break;
            case TokenType::KwFun:    declType = ST_FUNCTION; break;
            default: break;
        }
        if (declType < 0) continue;
        // Only when the name follows the keyword directly (skips attributed decls
        // like `struct [repr(C)] Name`, whose name still classifies via typeKind).
        int j = nextNonNewline(i);
        if (j >= 0 && tokens[static_cast<size_t>(j)].type == TokenType::Identifier) {
            const Token& n = tokens[static_cast<size_t>(j)];
            declByPos[{std::max(0, n.line - 1), std::max(0, n.column - 1)}] =
                Class{declType, MOD_DECLARATION};
        }
    }

    // --- Classify one identifier token ---
    auto classifyIdentifier = [&](size_t i) -> Class {
        const Token& t = tokens[i];
        const int line0 = std::max(0, t.line - 1);
        const int col0 = std::max(0, t.column - 1);
        const std::string& v = t.value;

        if (auto it = declByPos.find({line0, col0}); it != declByPos.end()) return it->second;
        if (auto it = posMap.find({line0, col0}); it != posMap.end()) return it->second;
        if (isPrimitiveTypeName(v)) return Class{ST_TYPE, MOD_DEFAULTLIB};
        if (genericParamNames.count(v)) return Class{ST_TYPEPARAM, 0};
        if (auto it = typeKind.find(v); it != typeKind.end()) return Class{it->second, 0};
        if (enumVariantNames.count(v)) return Class{ST_ENUMMEMBER, 0};
        if (functionNames.count(v)) return Class{ST_FUNCTION, 0};
        if (methodNames.count(v)) return Class{ST_METHOD, 0};
        if (fieldNames.count(v)) return Class{ST_PROPERTY, 0};

        const bool prevMember = i > 0 && (tokens[i - 1].type == TokenType::Dot ||
                                          tokens[i - 1].type == TokenType::ColonColon);
        const int nx = nextNonNewline(i);
        const bool nextCall = nx >= 0 && tokens[static_cast<size_t>(nx)].type == TokenType::LParen;
        if (prevMember && nextCall) return Class{ST_METHOD, 0};
        if (prevMember) return Class{ST_PROPERTY, 0};
        if (nextCall) return Class{ST_FUNCTION, 0};
        if (!v.empty() && std::isupper(static_cast<unsigned char>(v[0]))) return Class{ST_TYPE, 0};
        return Class{ST_VARIABLE, 0};
    };

    // --- Emit tokens (absolute positions first, then delta-encode) ---
    struct Emitted { int line; int col; int len; int type; int mods; };
    std::vector<Emitted> out;
    out.reserve(tokens.size());

    auto emit = [&](const Token& t, int type, int mods, int startOff, int endOff) {
        out.push_back(Emitted{
            std::max(0, t.line - 1),
            std::max(0, t.column - 1),
            std::max(1, endOff - startOff),
            type, mods});
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (isKeywordToken(t.type)) {
            emit(t, ST_KEYWORD, 0, t.start, t.end);
            continue;
        }
        switch (t.type) {
            case TokenType::IntegerLiteral:
            case TokenType::FloatLiteral:
                emit(t, ST_NUMBER, 0, t.start, t.end);
                continue;
            case TokenType::StringLiteral:
            case TokenType::CharLiteral:
                emit(t, ST_STRING, 0, t.start, t.end);
                continue;
            case TokenType::At: {
                // `@builtin` spans the `@` and the following identifier.
                if (i + 1 < tokens.size() && tokens[i + 1].type == TokenType::Identifier) {
                    const Token& name = tokens[i + 1];
                    emit(t, ST_MACRO, 0, t.start, name.end);
                    ++i;  // consumed the name token
                }
                continue;
            }
            case TokenType::Identifier: {
                // `in` is a contextual keyword only in for-headers; leave it to the
                // grammar so a variable named `in` is not mis-highlighted.
                if (t.value == "in") continue;
                const Class c = classifyIdentifier(i);
                emit(t, c.type, c.mods, t.start, t.end);
                continue;
            }
            default:
                continue;  // operators / punctuation / newlines -> handled by grammar
        }
    }

    std::sort(out.begin(), out.end(), [](const Emitted& a, const Emitted& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.col < b.col;
    });

    int prevLine = 0, prevCol = 0;
    for (const auto& e : out) {
        const int deltaLine = e.line - prevLine;
        const int deltaCol = deltaLine == 0 ? e.col - prevCol : e.col;
        data.push_back(deltaLine);
        data.push_back(deltaCol);
        data.push_back(e.len);
        data.push_back(e.type);
        data.push_back(e.mods);
        prevLine = e.line;
        prevCol = e.col;
    }

    return data;
}

} // namespace detail

void Server::handleSemanticTokensFull(const JSONValue& id, const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    auto doc = getDocument(uri);
    if (!doc) {
        sendSemanticTokensResponse(id, {});
        return;
    }
    sendSemanticTokensResponse(id, detail::computeSemanticTokens(doc->get()));
}

} // namespace LSP

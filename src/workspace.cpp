#include <lsp/lsp_internal.hpp>

#include <set>
#include <system_error>

#include <utilities/utils.hpp>

namespace fs = std::filesystem;

namespace LSP {

namespace {

bool pathWithinDirectory(const fs::path& path, const fs::path& dir) {
    std::error_code ec;
    const fs::path relative = path.lexically_relative(dir);
    if (relative.empty()) {
        return path == dir;
    }
    return !relative.empty() && *relative.begin() != "..";
}

} // namespace

void Server::handleDidOpen(const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    const std::string text = params["textDocument"]["text"].getString();
    reindexDocument(uri, text, false);
    if (auto doc = getDocument(uri)) {
        upsertModuleCacheEntry(doc->get().filePath, doc->get().text);
    }
    validateDocument(uri);
}

void Server::handleDidChange(const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    const auto contentChanges = params["contentChanges"].asArray();
    if (contentChanges.empty()) {
        return;
    }

    const std::string text = contentChanges.front()["text"].getString();
    reindexDocument(uri, text, false);
    if (auto doc = getDocument(uri)) {
        upsertModuleCacheEntry(doc->get().filePath, doc->get().text);
    }
    validateDocument(uri);
}

void Server::handleDidClose(const JSONValue& params) {
    const std::string uri = params["textDocument"]["uri"].getString();
    auto doc = getDocument(uri);
    if (!doc) {
        return;
    }

    const std::string filePath = doc->get().filePath;
    clearDocumentIndex(doc->get());
    documents.erase(uri);

    if (!filePath.empty()) {
        std::error_code ec;
        if (fs::exists(filePath, ec) && !ec) {
            refreshModuleFile(filePath);
        } else {
            removeModuleFile(filePath);
        }
    }

    sendDiagnostics(uri, {});
}

void Server::handleDidChangeWatchedFiles(const JSONValue& params) {
    if (!params.contains("changes") || !params["changes"].isArray()) {
        return;
    }

    for (const auto& change : params["changes"].asArray()) {
        const std::string uri = change["uri"].getString();
        const int type = static_cast<int>(change["type"].getNumber());
        const fs::path path = uriToPath(uri);
        if (path.empty()) {
            continue;
        }

        if (type == 3) {
            removeModuleFile(path);
            continue;
        }

        if (auto openDoc = getDocument(uri); openDoc && !openDoc->get().filePath.empty()) {
            upsertModuleCacheEntry(openDoc->get().filePath, openDoc->get().text);
            continue;
        }

        refreshModuleFile(path);
    }
}

void Server::clearDocumentIndex(const DocumentState& doc) {
    if (!doc.moduleName.empty()) {
        auto it = moduleUriByName.find(doc.moduleName);
        if (it != moduleUriByName.end() && it->second == doc.uri) {
            moduleUriByName.erase(it);
        }
    }

    if (!doc.filePath.empty()) {
        const std::string canonicalPath = canonicalPathString(doc.filePath);
        auto it = moduleUriByPath.find(canonicalPath);
        if (it != moduleUriByPath.end() && it->second == doc.uri) {
            moduleUriByPath.erase(it);
        }
    }
}

void Server::indexDocument(const DocumentState& doc) {
    if (!doc.moduleName.empty()) {
        moduleUriByName[doc.moduleName] = doc.uri;
    }
    if (!doc.filePath.empty()) {
        moduleUriByPath[canonicalPathString(doc.filePath)] = doc.uri;
    }
}

std::optional<std::reference_wrapper<DocumentState>> Server::getDocument(const std::string& uri) {
    auto it = documents.find(uri);
    if (it == documents.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::reference_wrapper<const DocumentState>> Server::getDocument(const std::string& uri) const {
    auto it = documents.find(uri);
    if (it == documents.end()) {
        return std::nullopt;
    }
    return std::cref(it->second);
}

void Server::indexWorkspaceModules() {
    for (const auto& dir : getModuleSearchDirectories(nullptr)) {
        ensureModuleDirectoryIndexed(dir);
    }
}

void Server::ensureModuleDirectoryIndexed(const fs::path& dir) {
    if (dir.empty()) {
        return;
    }

    const std::string key = canonicalPathString(dir);
    if (!indexedModuleDirectories.insert(key).second) {
        return;
    }

    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        return;
    }

    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".ins") {
            continue;
        }
        refreshModuleFile(it->path());
    }
}

void Server::upsertModuleCacheEntry(const fs::path& path, const std::string& text) {
    if (path.empty()) {
        return;
    }

    const std::string canonicalPath = canonicalPathString(path);
    removeModuleFile(canonicalPath);

    const std::string moduleName = detail::inferModuleNameFromText(text, fs::path(canonicalPath));
    if (moduleName.empty()) {
        return;
    }

    moduleCacheByPath[canonicalPath] = ModuleCacheEntry{moduleName, canonicalPath};
    modulePathsByName[moduleName].insert(canonicalPath);
}

void Server::refreshModuleFile(const fs::path& path) {
    if (path.empty()) {
        return;
    }

    const std::string canonicalPath = canonicalPathString(path);
    std::error_code ec;
    if (!fs::exists(canonicalPath, ec) || ec || fs::is_directory(canonicalPath, ec)) {
        removeModuleFile(canonicalPath);
        return;
    }

    if (fs::path(canonicalPath).extension() != ".ins") {
        removeModuleFile(canonicalPath);
        return;
    }

    const std::string text = Utilities::readFile(canonicalPath);
    if (text.empty()) {
        removeModuleFile(canonicalPath);
        return;
    }

    upsertModuleCacheEntry(canonicalPath, text);
}

void Server::removeModuleFile(const fs::path& path) {
    if (path.empty()) {
        return;
    }

    const std::string canonicalPath = canonicalPathString(path);
    auto it = moduleCacheByPath.find(canonicalPath);
    if (it == moduleCacheByPath.end()) {
        return;
    }

    auto byName = modulePathsByName.find(it->second.moduleName);
    if (byName != modulePathsByName.end()) {
        byName->second.erase(canonicalPath);
        if (byName->second.empty()) {
            modulePathsByName.erase(byName);
        }
    }

    moduleCacheByPath.erase(it);
}

std::vector<fs::path> Server::getModuleSearchDirectories(const DocumentState* contextDoc) const {
    std::vector<fs::path> dirs;
    std::set<std::string> seen;

    auto addDir = [&](const fs::path& path) {
        if (path.empty()) {
            return;
        }
        std::error_code ec;
        const fs::path normalized = fs::weakly_canonical(path, ec);
        const fs::path finalPath = ec ? path.lexically_normal() : normalized;
        const std::string key = finalPath.string();
        if (seen.insert(key).second) {
            dirs.push_back(finalPath);
        }
    };

    if (contextDoc && !contextDoc->filePath.empty()) {
        addDir(fs::path(contextDoc->filePath).parent_path());
    }

    for (const auto& root : workspaceRoots) {
        addDir(root);
        addDir(root / "src");
        addDir(root / "lib");
        addDir(root / "std");
        addDir(root / "build" / "libs");
        addDir(root / "build" / "libs" / "std");
    }

    addDir(fs::current_path());
    addDir(fs::current_path() / "std");
    addDir(fs::current_path() / "build" / "libs");
    addDir(fs::current_path() / "build" / "libs" / "std");
    addDir(executableDir / "libs");
    addDir(executableDir / "libs" / "std");

    return dirs;
}

std::vector<fs::path> Server::getCandidateModuleFiles(const std::string& moduleName, const DocumentState* contextDoc) const {
    std::vector<fs::path> files;
    std::set<std::string> seen;

    // `::` is the module-path directory separator: `std::io` -> `std/io`. Try
    // the path-translated spelling, the verbatim name, and the trailing
    // segment (so a flat `io.ins` still resolves when discovered by name).
    std::string relative = moduleName;
    for (std::string::size_type pos = relative.find("::");
         pos != std::string::npos; pos = relative.find("::", pos + 1)) {
        relative.replace(pos, 2, "/");
    }
    std::string lastSegment = moduleName;
    if (auto sep = moduleName.rfind("::"); sep != std::string::npos) {
        lastSegment = moduleName.substr(sep + 2);
    }

    std::vector<std::string> names = {relative, moduleName};
    if (lastSegment != relative && lastSegment != moduleName) {
        names.push_back(lastSegment);
    }

    for (const auto& dir : getModuleSearchDirectories(contextDoc)) {
        for (const auto& name : names) {
            for (const auto& candidate : {
                     dir / (name + ".ins"),
                     dir / name,
                     dir / fs::path(name + ".ins")
                 }) {
                const std::string key = candidate.lexically_normal().string();
                if (seen.insert(key).second) {
                    files.push_back(candidate);
                }
            }
        }
    }

    return files;
}

std::optional<fs::path> Server::resolveModulePath(const std::string& moduleName, const DocumentState* contextDoc) {
    indexWorkspaceModules();
    for (const auto& dir : getModuleSearchDirectories(contextDoc)) {
        ensureModuleDirectoryIndexed(dir);
    }

    // The trailing path segment is the module's own declared name (e.g. `io`
    // for `std::io`), which is how discovered modules are keyed/declared.
    std::string lastSegment = moduleName;
    if (auto sep = moduleName.rfind("::"); sep != std::string::npos) {
        lastSegment = moduleName.substr(sep + 2);
    }

    for (const auto& candidate : getCandidateModuleFiles(moduleName, contextDoc)) {
        const std::string key = canonicalPathString(candidate);
        auto cached = moduleCacheByPath.find(key);
        if (cached != moduleCacheByPath.end() &&
            (cached->second.moduleName == moduleName ||
             cached->second.moduleName == lastSegment)) {
            return fs::path(cached->second.canonicalPath);
        }
        // The file may exist on disk without being indexed yet (e.g. a nested
        // `libs/std/io.ins` reached via the `::` path translation).
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }

    auto byName = modulePathsByName.find(moduleName);
    if (byName == modulePathsByName.end() || byName->second.empty()) {
        byName = modulePathsByName.find(lastSegment);
    }
    if (byName == modulePathsByName.end() || byName->second.empty()) {
        return std::nullopt;
    }

    for (const auto& dir : getModuleSearchDirectories(contextDoc)) {
        for (const auto& path : byName->second) {
            if (pathWithinDirectory(fs::path(path), dir)) {
                return fs::path(path);
            }
        }
    }

    return fs::path(*byName->second.begin());
}

void Server::loadWorkspaceDocuments() {
    indexWorkspaceModules();

    for (const auto& [canonicalPath, _entry] : moduleCacheByPath) {
        if (moduleUriByPath.find(canonicalPath) != moduleUriByPath.end()) {
            continue;
        }

        const std::string text = Utilities::readFile(canonicalPath);
        if (text.empty()) {
            continue;
        }

        reindexDocument(pathToUri(canonicalPath), text, true);
    }
}

std::vector<std::string> Server::collectAvailableModules(const DocumentState* contextDoc) {
    for (const auto& dir : getModuleSearchDirectories(contextDoc)) {
        ensureModuleDirectoryIndexed(dir);
    }

    std::set<std::string> modules;
    for (const auto& [name, _uri] : moduleUriByName) {
        modules.insert(name);
    }
    for (const auto& [name, _paths] : modulePathsByName) {
        modules.insert(name);
    }

    return {modules.begin(), modules.end()};
}

} // namespace LSP

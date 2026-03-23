#pragma once

#include <filesystem>
#include <memory>
#include <unordered_set>

#include "dash/ast/ast.hpp"

namespace dash::frontend {

class SourceLoader {
public:
    explicit SourceLoader(std::filesystem::path mainFile);

    [[nodiscard]] std::unique_ptr<ast::Program> loadProgram();

private:
    [[nodiscard]] std::unique_ptr<ast::Program> loadFile(const std::filesystem::path& path);
    std::filesystem::path resolveImportPath(const std::string& importName, bool global, const std::filesystem::path& importerDirectory) const;
    [[nodiscard]] std::string readFile(const std::filesystem::path& path) const;

    std::filesystem::path mainFile_;
    std::filesystem::path mainDirectory_;
    std::unordered_set<std::string> loadedFiles_;
    std::unordered_set<std::string> activeFiles_;
};

} // namespace dash::frontend

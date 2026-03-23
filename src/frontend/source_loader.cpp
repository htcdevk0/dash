#include "dash/frontend/source_loader.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <utility>

#include "dash/core/diagnostic.hpp"
#include "dash/frontend/lexer.hpp"
#include "dash/frontend/parser.hpp"
#include "dash/ast/ast.hpp"

namespace dash::frontend
{

    SourceLoader::SourceLoader(std::filesystem::path mainFile)
        : mainFile_(std::filesystem::absolute(std::move(mainFile))),
          mainDirectory_(mainFile_.parent_path()) {}

    std::unique_ptr<ast::Program> SourceLoader::loadProgram()
    {
        return loadFile(mainFile_);
    }

    std::unique_ptr<ast::Program> SourceLoader::loadFile(const std::filesystem::path &path)
    {

        const auto absolutePath = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        const auto key = absolutePath.string();

        if (loadedFiles_.contains(key))
        {
            auto program = std::make_unique<ast::Program>();
            program->location = core::SourceLocation{key, 1, 1};
            return program;
        }

        if (activeFiles_.contains(key))
        {
            core::throwDiagnostic(
                core::SourceLocation{key, 1, 1},
                "cyclic import detected");
        }

        activeFiles_.insert(key);

        const auto source = readFile(absolutePath);

        static const std::regex importLocal("^\\s*import\\s+\"([^\"]+)\"\\s*;\\s*$");
        static const std::regex importGlobal("^\\s*import\\s+\\[([^\\]]+)\\]\\s*;\\s*$");

        std::stringstream input(source);
        std::string line;

        struct ImportEntry
        {
            std::string name;
            bool isGlobal;
        };

        std::vector<ImportEntry> imports;
        std::string strippedSource;
        bool inBlockComment = false;

        while (std::getline(input, line))
        {
            std::string importScanLine;
            importScanLine.reserve(line.size());

            for (std::size_t i = 0; i < line.size();)
            {
                if (inBlockComment)
                {
                    if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '/')
                    {
                        inBlockComment = false;
                        i += 2;
                    }
                    else
                    {
                        ++i;
                    }
                    continue;
                }

                if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*')
                {
                    inBlockComment = true;
                    i += 2;
                    continue;
                }

                if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
                {
                    break;
                }

                importScanLine.push_back(line[i]);
                ++i;
            }

            std::smatch match;

            if (std::regex_match(importScanLine, match, importLocal))
            {
                imports.push_back({match[1].str(), false});
                strippedSource.push_back('\n');
                continue;
            }

            if (std::regex_match(importScanLine, match, importGlobal))
            {
                imports.push_back({match[1].str(), true});
                strippedSource.push_back('\n');
                continue;
            }

            strippedSource += line;
            strippedSource.push_back('\n');
        }

        auto combined = std::make_unique<ast::Program>();
        combined->location = core::SourceLocation{key, 1, 1};

        for (const auto &import : imports)
        {

            auto resolved = resolveImportPath(import.name, import.isGlobal, absolutePath.parent_path());

            auto importedProgram = loadFile(resolved);

            for (auto &decl : importedProgram->declarations)
            {
                combined->declarations.push_back(std::move(decl));
            }
        }

        Lexer lexer(key, strippedSource);
        auto tokens = lexer.tokenize();

        Parser parser(std::move(tokens), key);
        auto currentProgram = parser.parseProgram();

        for (auto &decl : currentProgram->declarations)
        {
            combined->declarations.push_back(std::move(decl));
        }

        activeFiles_.erase(key);
        loadedFiles_.insert(key);

        return combined;
    }

    std::filesystem::path SourceLoader::resolveImportPath(
        const std::string &importName,
        bool global,
        const std::filesystem::path &importerDirectory) const
    {

        std::filesystem::path path(importName);

        if (!global)
        {

            auto resolved = importerDirectory / path;

            if (resolved.extension().empty())
            {
                resolved += ".ds";
            }

            return resolved;
        }

        auto resolvedProject = mainDirectory_;
        while (!resolvedProject.empty())
        {
            auto candidate = resolvedProject / "stdlib" / "c" / path;
            if (candidate.extension().empty())
            {
                candidate += ".ds";
            }
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }
            if (resolvedProject == resolvedProject.root_path())
            {
                break;
            }
            resolvedProject = resolvedProject.parent_path();
        }

        const char *home = std::getenv("HOME");
        if (!home || std::string(home).empty())
        {
            core::throwDiagnostic(
                core::SourceLocation{mainFile_.string(), 1, 1},
                "HOME not set; cannot resolve global imports");
        }

        auto resolved = std::filesystem::path(home) / ".dash" / "imports" / path;
        if (resolved.extension().empty())
        {
            resolved += ".ds";
        }
        return resolved;
    }

    std::string SourceLoader::readFile(const std::filesystem::path &path) const
    {

        std::ifstream in(path, std::ios::binary);

        if (!in)
        {
            core::throwDiagnostic(
                core::SourceLocation{path.string(), 1, 1},
                "failed to open source file");
        }

        std::string content{
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
        if (content.size() >= 3 &&
            (unsigned char)content[0] == 0xEF &&
            (unsigned char)content[1] == 0xBB &&
            (unsigned char)content[2] == 0xBF)
        {
            content.erase(0, 3);
        }

        return content;
    }

} // namespace dash::frontend

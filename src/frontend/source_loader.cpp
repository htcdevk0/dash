#include "dash/frontend/source_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "dash/core/diagnostic.hpp"
#include "dash/frontend/lexer.hpp"
#include "dash/frontend/parser.hpp"
#include "dash/ast/ast.hpp"

namespace dash::frontend
{
    namespace
    {
        struct ImportEntry
        {
            std::string name;
            bool isGlobal {false};
        };

        struct UseEntry
        {
            std::string name;
            bool isGlobal {false};
            bool importAll {false};
            std::vector<std::string> symbols;
        };

        struct ParsedDirectives
        {
            std::vector<ImportEntry> imports;
            std::vector<UseEntry> uses;
            std::string strippedSource;
        };

        [[nodiscard]] std::string trim(std::string text)
        {
            auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }

        [[nodiscard]] std::vector<std::string> splitSymbols(const std::string &text, const core::SourceLocation &location)
        {
            std::vector<std::string> result;
            std::stringstream input(text);
            std::string item;
            while (std::getline(input, item, ','))
            {
                auto symbol = trim(item);
                if (symbol.empty())
                {
                    core::throwDiagnostic(location, "empty symbol in use list");
                }
                if (symbol.find("::") != std::string::npos)
                {
                    core::throwDiagnostic(location, "use cannot import nested namespace selectors; import the top-level namespace instead");
                }
                result.push_back(std::move(symbol));
            }
            return result;
        }

        [[nodiscard]] ParsedDirectives parseDirectives(const std::string &source, const std::string &filePath)
        {
            static const std::regex importLocal("^\\s*import\\s+\"([^\"]+)\"\\s*;\\s*$");
            static const std::regex importGlobal("^\\s*import\\s+\\[([^\\]]+)\\]\\s*;\\s*$");
            static const std::regex useLocal("^\\s*use\\s+\"([^\"]+)\"\\s*\\{\\s*([^}]*)\\s*\\}\\s*;\\s*$");
            static const std::regex useGlobal("^\\s*use\\s+\\[([^\\]]+)\\]\\s*\\{\\s*([^}]*)\\s*\\}\\s*;\\s*$");

            ParsedDirectives parsed;
            std::stringstream input(source);
            std::string line;
            bool inBlockComment = false;
            std::size_t lineNumber = 0;

            while (std::getline(input, line))
            {
                ++lineNumber;
                std::string scanLine;
                scanLine.reserve(line.size());

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

                    scanLine.push_back(line[i]);
                    ++i;
                }

                std::smatch match;
                const auto location = core::SourceLocation{filePath, static_cast<int>(lineNumber), 1};

                if (std::regex_match(scanLine, match, importLocal))
                {
                    parsed.imports.push_back({match[1].str(), false});
                    parsed.strippedSource.push_back('\n');
                    continue;
                }

                if (std::regex_match(scanLine, match, importGlobal))
                {
                    parsed.imports.push_back({match[1].str(), true});
                    parsed.strippedSource.push_back('\n');
                    continue;
                }

                if (std::regex_match(scanLine, match, useLocal))
                {
                    UseEntry entry;
                    entry.name = match[1].str();
                    entry.isGlobal = false;
                    const auto selectors = trim(match[2].str());
                    entry.importAll = selectors == "*";
                    if (!entry.importAll)
                    {
                        entry.symbols = splitSymbols(selectors, location);
                    }
                    parsed.uses.push_back(std::move(entry));
                    parsed.strippedSource.push_back('\n');
                    continue;
                }

                if (std::regex_match(scanLine, match, useGlobal))
                {
                    UseEntry entry;
                    entry.name = match[1].str();
                    entry.isGlobal = true;
                    const auto selectors = trim(match[2].str());
                    entry.importAll = selectors == "*";
                    if (!entry.importAll)
                    {
                        entry.symbols = splitSymbols(selectors, location);
                    }
                    parsed.uses.push_back(std::move(entry));
                    parsed.strippedSource.push_back('\n');
                    continue;
                }

                parsed.strippedSource += line;
                parsed.strippedSource.push_back('\n');
            }

            return parsed;
        }

        [[nodiscard]] std::string declName(const ast::Decl &decl)
        {
            if (const auto *fn = dynamic_cast<const ast::FunctionDecl *>(&decl))
            {
                return fn->name;
            }
            if (const auto *ext = dynamic_cast<const ast::ExternDecl *>(&decl))
            {
                return ext->name;
            }
            if (const auto *global = dynamic_cast<const ast::GlobalVarDecl *>(&decl))
            {
                return global->name;
            }
            if (const auto *klass = dynamic_cast<const ast::ClassDecl *>(&decl))
            {
                return klass->name;
            }
            if (const auto *en = dynamic_cast<const ast::EnumDecl *>(&decl))
            {
                return en->name;
            }
            return {};
        }
    } // namespace

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
            core::throwDiagnostic(core::SourceLocation{key, 1, 1}, "cyclic import detected");
        }

        activeFiles_.insert(key);

        const auto source = readFile(absolutePath);
        const auto parsed = parseDirectives(source, key);

        auto combined = std::make_unique<ast::Program>();
        combined->location = core::SourceLocation{key, 1, 1};

        for (const auto &import : parsed.imports)
        {
            auto resolved = resolveImportPath(import.name, import.isGlobal, absolutePath.parent_path());
            auto importedProgram = loadFile(resolved);
            for (auto &decl : importedProgram->declarations)
            {
                combined->declarations.push_back(std::move(decl));
            }
        }

        for (const auto &use : parsed.uses)
        {
            auto resolved = resolveImportPath(use.name, use.isGlobal, absolutePath.parent_path());
            const auto useSource = readFile(resolved);
            const auto useParsed = parseDirectives(useSource, resolved.string());

            Lexer useLexer(resolved.string(), useParsed.strippedSource);
            auto useTokens = useLexer.tokenize();
            Parser useParser(std::move(useTokens), resolved.string());
            auto useProgram = useParser.parseProgram();

            std::unordered_set<std::string> remaining(use.symbols.begin(), use.symbols.end());
            std::unordered_set<std::string> resolvedNamespaces;
            for (auto &decl : useProgram->declarations)
            {
                const auto name = declName(*decl);
                if (name.empty())
                {
                    continue;
                }

                bool selected = use.importAll || remaining.contains(name);
                std::string matchedNamespace;
                if (!selected)
                {
                    for (const auto &symbol : use.symbols)
                    {
                        const std::string prefix = symbol + "::";
                        if (name.rfind(prefix, 0) == 0)
                        {
                            selected = true;
                            matchedNamespace = symbol;
                            break;
                        }
                    }
                }

                if (selected)
                {
                    combined->declarations.push_back(std::move(decl));
                    if (!matchedNamespace.empty())
                        resolvedNamespaces.insert(matchedNamespace);
                    else
                        remaining.erase(name);
                }
            }

            if (!use.importAll)
            {
                for (const auto &ns : resolvedNamespaces)
                    remaining.erase(ns);
            }

            if (!use.importAll && !remaining.empty())
            {
                std::string missing;
                bool first = true;
                for (const auto &name : remaining)
                {
                    if (!first)
                    {
                        missing += ", ";
                    }
                    first = false;
                    missing += name;
                }
                core::throwDiagnostic(core::SourceLocation{key, 1, 1}, "use could not resolve symbol(s): " + missing);
            }
        }

        Lexer lexer(key, parsed.strippedSource);
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
            core::throwDiagnostic(core::SourceLocation{mainFile_.string(), 1, 1},
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
            core::throwDiagnostic(core::SourceLocation{path.string(), 1, 1}, "failed to open source file");
        }

        std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
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

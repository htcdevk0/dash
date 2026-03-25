#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dash/codegen/codegen.hpp"
#include "dash/core/diagnostic.hpp"
#include "dash/frontend/source_loader.hpp"
#include "dash/sema/analyzer.hpp"

#define RESET "\033[0m"
#define BOLD "\033[1m"
#define DIM "\033[2m"

#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"

namespace
{

    constexpr const char *kDashVersion = "5.1.0LL";

    enum class CommandMode
    {
        Legacy,
        Build,
        Run,
    };

    struct CommandLineOptions
    {
        CommandMode mode{CommandMode::Legacy};
        std::string inputPath;
        std::string outputPath;
        bool emitLLVM{false};
        bool emitObjectOnly{false};
        bool emitShared{false};
        std::string clangPath{"clang"};
        std::vector<std::string> linkProfiles;
        std::vector<std::string> extraLinkArgs;
        bool useDashRuntime{false};
        bool smartLinking{false};
    };

    [[nodiscard]] bool looksLikeLinkInput(const std::string &arg)
    {
        const auto path = std::filesystem::path(arg);
        const auto ext = path.extension().string();
        return ext == ".so" || ext == ".dll" || ext == ".dylib" || ext == ".a" || ext == ".o" || ext == ".obj" || ext == ".lib";
    }

    [[nodiscard]] std::string quoteShell(const std::string &value)
    {
        std::string out = "\"";
        for (const char c : value)
        {
            if (c == '\\' || c == '"')
                out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    [[nodiscard]] CommandLineOptions parseCommandLine(int argc, char **argv)
    {
        CommandLineOptions options;

        int startIndex = 1;
        if (argc > 1)
        {
            const std::string first = argv[1];
            if (first == "build")
            {
                options.mode = CommandMode::Build;
                startIndex = 2;
            }
            else if (first == "run")
            {
                options.mode = CommandMode::Run;
                startIndex = 2;
            }
        }

        for (int i = startIndex; i < argc; ++i)
        {
            const std::string arg = argv[i];

            if (arg == "--help" || arg == "-h")
            {
                std::cout
                    << BOLD << CYAN
                    << "Dash Compiler" << RESET << " "
                    << DIM << "(htcdevk0)" << RESET << "\n\n"

                    << BOLD << "Usage:" << RESET << "\n"
                    << "  " << GREEN << "dash" << RESET << " <input.ds> [options]\n"
                    << "  " << GREEN << "dash build" << RESET << " <input.ds> [options]\n"
                    << "  " << GREEN << "dash run" << RESET << " <input.ds> [options]\n\n"

                    << BOLD << BLUE << "General:" << RESET << "\n"
                    << "  " << YELLOW << "--version, -v" << RESET << "    Show compiler version\n"
                    << "  " << YELLOW << "--license, -lic" << RESET << "  Show license\n"
                    << "  " << YELLOW << "--author, -aut" << RESET << "   Show author (htcdevk0)\n\n"

                    << BOLD << BLUE << "Build:" << RESET << "\n"
                    << "  " << YELLOW << "-o <path>" << RESET << "         Output path\n"
                    << "  " << YELLOW << "--emit-llvm" << RESET << "       Emit LLVM IR (.ll)\n"
                    << "  " << YELLOW << "-obj / -c" << RESET << "        Emit object file\n"
                    << "  " << YELLOW << "--shared" << RESET << "          Build shared library (.so/.dll/.dylib)\n"
                    << "  " << YELLOW << "--clang <path>" << RESET << "    Use custom clang for linking\n\n"

                    << BOLD << BLUE << "Linking:" << RESET << "\n"
                    << "  " << YELLOW << "-L=<profile>" << RESET << "     Extra link profile (e.g. gtk4)\n"
                    << "  " << YELLOW << "-cl <file>" << RESET << "       Statically link object/archive\n"
                    << "  " << YELLOW << "-ld<path>" << RESET << "         Add linker search directory\n"
                    << "  " << YELLOW << "-l<name>" << RESET << "         Link native library\n\n"

                    << BOLD << BLUE << "Runtime:" << RESET << "\n"
                    << "  " << YELLOW << "-d" << RESET << "                Link shared libs from ~/.dash/lib\n"
                    << "  " << YELLOW << "-sl" << RESET << "               Smart static/dynamic linking\n\n"

                    << BOLD << BLUE << "Native:" << RESET << "\n"
                    << "  " << YELLOW << "<file.so|.dll|.dylib|.a|.o>" << RESET
                    << "  Add native object/library to link\n\n"

                    << DIM << "Repositories:\n"
                    << "  "
                    << DIM << "https://github.com/" << RESET
                    << GREEN << "htcdevk0" << RESET << "/"
                    << CYAN << "dash" << RESET << "\n"

                    << "  "
                    << DIM << "https://github.com/" << RESET
                    << GREEN << "htcdevk0" << RESET << "/"
                    << CYAN << "dash-stdlib" << RESET << "\n"
                    
                    << "  "
                    << DIM << "https://github.com/" << RESET
                    << GREEN << "htcdevk0" << RESET << "/"
                    << CYAN << "dashtup" << RESET << "\n";
                    
                std::exit(0);
            }

            if (arg == "--version" || arg == "-v")
            {
                std::cout << "Dash Programming Language version: " << kDashVersion << "\n";
                std::exit(0);
            }

            if (arg == "--license" || arg == "-lic")
            {
                std::cout
                    << "Dash Programming Language\n"
                    << "Copyright (c) htcdevk0\n"
                    << "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
                    << "This is free software: you are free to change and redistribute it.\n"
                    << "There is NO WARRANTY, to the extent permitted by law.\n\n"
                    << "Full license:\n"
                    << "https://github.com/htcdevk0/dash/master/LICENSE\n";
                std::exit(0);
            }

            if (arg == "--author" || arg == "-aut")
            {
                std::cout << "Dash Programming language\nMade by: htcdevk0\n";
                std::exit(0);
            }

            if (arg == "--emit-llvm")
            {
                options.emitLLVM = true;
                continue;
            }

            if (arg == "-obj" || arg == "-c")
            {
                options.emitObjectOnly = true;
                continue;
            }

            if (arg == "--shared")
            {
                options.emitShared = true;
                continue;
            }

            if (arg == "-d")
            {
                options.useDashRuntime = true;
                continue;
            }

            if (arg == "-sl")
            {
                options.smartLinking = true;
                continue;
            }

            if (arg == "-o")
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error("missing value after -o");
                }
                options.outputPath = argv[++i];
                continue;
            }

            if (arg == "--clang")
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error("missing value after --clang");
                }
                options.clangPath = argv[++i];
                continue;
            }

            if (arg == "-cl")
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error("missing value after -cl");
                }
                options.extraLinkArgs.push_back(argv[++i]);
                continue;
            }

            if (arg.rfind("-cl", 0) == 0 && arg.size() > 3)
            {
                std::string input = arg.substr(3);
                if (!input.empty() && input[0] == '=')
                {
                    input.erase(input.begin());
                }
                if (input.empty())
                {
                    throw std::runtime_error("missing value after -cl");
                }
                options.extraLinkArgs.push_back(input);
                continue;
            }

            if (arg.rfind("-L=", 0) == 0)
            {
                const std::string profile = arg.substr(3);
                if (profile != "gtk4")
                {
                    throw std::runtime_error("unsupported -L profile: " + profile + " (currently only gtk4)");
                }
                options.linkProfiles.push_back(profile);
                continue;
            }

            if (arg == "-ld")
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error("missing value after -ld");
                }
                options.extraLinkArgs.push_back(std::string("-L") + argv[++i]);
                continue;
            }

            if (arg.rfind("-ld", 0) == 0 && arg.size() > 3)
            {
                std::string directory = arg.substr(3);
                if (!directory.empty() && directory[0] == '=')
                {
                    directory.erase(directory.begin());
                }
                if (directory.empty())
                {
                    throw std::runtime_error("missing value after -ld");
                }
                options.extraLinkArgs.push_back(std::string("-L") + directory);
                continue;
            }

            if (!arg.empty() && arg[0] == '-')
            {
                if (arg.rfind("-l", 0) == 0 && arg.size() > 2)
                {
                    options.extraLinkArgs.push_back(arg);
                    continue;
                }
                throw std::runtime_error("unknown option: " + arg);
            }

            if (options.inputPath.empty())
            {
                options.inputPath = arg;
                continue;
            }

            if (looksLikeLinkInput(arg))
            {
                options.extraLinkArgs.push_back(arg);
                continue;
            }

            throw std::runtime_error("only one input file is supported right now");
        }

        if (options.inputPath.empty())
        {
            throw std::runtime_error("no input file provided");
        }

        if (options.outputPath.empty())
        {
            const auto input = std::filesystem::path(options.inputPath);
            if (options.emitLLVM)
            {
                options.outputPath = (std::filesystem::path("build") / input.stem()).string() + ".ll";
            }
            else if (options.emitObjectOnly)
            {
                options.outputPath = (std::filesystem::path("build") / input.stem()).string() + ".o";
            }
            else if (options.emitShared)
            {
#if defined(_WIN32)
                options.outputPath = (std::filesystem::path("build") / input.stem()).string() + ".dll";
#elif defined(__APPLE__)
                options.outputPath = (std::filesystem::path("build") / input.stem()).string() + ".dylib";
#else
                options.outputPath = (std::filesystem::path("build") / input.stem()).string() + ".so";
#endif
            }
            else if (options.mode == CommandMode::Build || options.mode == CommandMode::Run)
            {
                options.outputPath = input.stem().string();
            }
            else
            {
                options.outputPath = (std::filesystem::path("build") / input.stem()).string();
            }
        }

        return options;
    }

    void printDiagnostic(const dash::core::DiagnosticError &error)
    {
        const auto &location = error.location();
        std::cerr << location.file << ':' << location.line << ':' << location.column << ": error: " << error.what() << '\n';
    }

} // namespace

int main(int argc, char **argv)
{
    try
    {
        const auto cli = parseCommandLine(argc, argv);

        dash::frontend::SourceLoader loader(cli.inputPath);
        auto program = loader.loadProgram();

        dash::sema::Analyzer analyzer;
        analyzer.setSharedBuild(cli.emitShared);
        analyzer.setEntryPointRequired(!cli.emitObjectOnly);
        analyzer.analyze(*program);

        dash::codegen::CodeGenerator codegen;
        codegen.compile(*program, dash::codegen::CompileOptions{
                                      cli.inputPath,
                                      cli.outputPath,
                                      cli.emitLLVM,
                                      cli.emitObjectOnly,
                                      cli.emitShared,
                                      cli.clangPath,
                                      cli.linkProfiles,
                                      cli.extraLinkArgs,
                                      cli.useDashRuntime,
                                      cli.smartLinking,
                                  });

        if (cli.mode == CommandMode::Run)
        {
            if (cli.emitLLVM || cli.emitObjectOnly || cli.emitShared)
            {
                throw std::runtime_error("dash run requires an executable output");
            }

            std::filesystem::path outputPath(cli.outputPath);
            std::string command;
            if (outputPath.is_relative() && outputPath.parent_path().empty())
                command = std::string("./") + outputPath.string();
            else
                command = quoteShell(outputPath.string());

            if (std::system(command.c_str()) != 0)
                return 1;
        }

        return 0;
    }
    catch (const dash::core::DiagnosticError &error)
    {
        printDiagnostic(error);
        return 1;
    }
    catch (const std::exception &error)
    {
        std::cerr << "dash: fatal error: " << error.what() << '\n';
        return 1;
    }
}

#include "dash/codegen/codegen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <system_error>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include "dash/core/diagnostic.hpp"

namespace dash::codegen
{

    namespace
    {

        constexpr const char *kDashArtifactSignature = "Built with Dash Programming Language (Dash 5.0.0LL) | htcdevk0";

        [[nodiscard]] std::string escapeString(const std::string &value)
        {
            std::string out;
            out.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                if (value[i] == '\\' && i + 1 < value.size())
                {
                    const char next = value[i + 1];
                    switch (next)
                    {
                    case 'n':
                        out.push_back('\n');
                        ++i;
                        continue;
                    case 't':
                        out.push_back('\t');
                        ++i;
                        continue;
                    case 'r':
                        out.push_back('\r');
                        ++i;
                        continue;
                    case '"':
                        out.push_back('"');
                        ++i;
                        continue;
                    case '\\':
                        out.push_back('\\');
                        ++i;
                        continue;
                    default:
                        break;
                    }
                }
                out.push_back(value[i]);
            }
            return out;
        }

        [[nodiscard]] llvm::Value *castMainAbiArgument(llvm::IRBuilder<> &builder, llvm::LLVMContext &context,
                                                       llvm::Value *value, const core::TypeRef &type)
        {
            if (type.isInt())
            {
                return builder.CreateSExtOrTrunc(value, llvm::Type::getInt64Ty(context), "main.arg.cast");
            }
            if (type.isUInt())
            {
                if (value->getType()->isPointerTy())
                {
                    return builder.CreatePtrToInt(value, llvm::Type::getInt64Ty(context), "main.argv.cast");
                }
                return builder.CreateZExtOrTrunc(value, llvm::Type::getInt64Ty(context), "main.arg.cast");
            }
            return value;
        }

        [[nodiscard]] std::uint64_t variadicTagForType(const core::TypeRef &type)
        {
            if (type.isPointer())
                return 6;
            switch (type.kind)
            {
            case core::BuiltinTypeKind::String:
                return 1;
            case core::BuiltinTypeKind::Int:
                return 2;
            case core::BuiltinTypeKind::Double:
                return 3;
            case core::BuiltinTypeKind::Bool:
                return 4;
            case core::BuiltinTypeKind::UInt:
                return 5;
            case core::BuiltinTypeKind::Char:
                return 7;
            default:
                return 0;
            }
        }

        constexpr std::uint64_t VariadicRefFlag = (1ull << 63);
        [[nodiscard]] std::uint64_t variadicBaseTag(std::uint64_t tag)
        {
            return tag & ~VariadicRefFlag;
        }

        [[nodiscard]] std::string mangleMethodName(const std::string &className, const std::string &methodName)
        {
            return className + "." + methodName;
        }

        [[nodiscard]] std::string sanitizeDashAbiFragment(const std::string &value)
        {
            std::string out;
            out.reserve(value.size());
            for (const char c : value)
            {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                    out.push_back(c);
                else
                    out.push_back('_');
            }
            return out;
        }

        [[nodiscard]] std::string encodeDashAbiType(const core::TypeRef &type)
        {
            std::string out;
            switch (type.kind)
            {
            case core::BuiltinTypeKind::Void:
                out = "v";
                break;
            case core::BuiltinTypeKind::Bool:
                out = "b";
                break;
            case core::BuiltinTypeKind::Int:
                out = "i";
                break;
            case core::BuiltinTypeKind::UInt:
                out = "u";
                break;
            case core::BuiltinTypeKind::Double:
                out = "d";
                break;
            case core::BuiltinTypeKind::Char:
                out = "c";
                break;
            case core::BuiltinTypeKind::String:
                out = "s";
                break;
            case core::BuiltinTypeKind::Class:
                out = "C" + std::to_string(type.name.size()) + "_" + sanitizeDashAbiFragment(type.name);
                break;
            case core::BuiltinTypeKind::Array:
            {
                core::TypeRef elem{};
                elem.kind = type.elementKind;
                elem.name = type.elementName;
                out = "A_" + encodeDashAbiType(elem) + "_" + (type.hasArraySize ? std::to_string(type.arraySize) : std::string("dyn"));
                break;
            }
            case core::BuiltinTypeKind::Unknown:
                out = "x";
                break;
            }
            if (type.pointerDepth > 0)
                out = "P" + std::to_string(type.pointerDepth) + "_" + out;
            return out;
        }

        [[nodiscard]] std::string dashAbiModuleName(const core::SourceLocation &location)
        {
            if (!location.file.empty())
            {
                const auto stem = std::filesystem::path(location.file).stem().string();
                if (!stem.empty())
                    return sanitizeDashAbiFragment(stem);
            }
            return "module";
        }

        [[nodiscard]] std::string mangleDashAbiFunctionName(const core::SourceLocation &location, const std::string &name, const std::vector<dash::ast::Parameter> &parameters, const core::TypeRef &returnType)
        {
            std::string out = "__dash_fn__" + dashAbiModuleName(location) + "__" + sanitizeDashAbiFragment(name) + "__";
            if (parameters.empty())
                out += "0__";
            for (const auto &param : parameters)
            {
                if (param.isVariadic)
                    out += "var__";
                else
                    out += encodeDashAbiType(param.type) + "__";
            }
            out += "ret__" + encodeDashAbiType(returnType);
            return out;
        }

        [[nodiscard]] std::string mangleDashAbiGlobalName(const core::SourceLocation &location, const std::string &name, const core::TypeRef &type)
        {
            return "__dash_var__" + dashAbiModuleName(location) + "__" + sanitizeDashAbiFragment(name) + "__" + encodeDashAbiType(type);
        }

        [[nodiscard]] std::string dashAbiFunctionUnqualifiedKey(const std::string &name, const std::vector<dash::ast::Parameter> &parameters, const core::TypeRef &returnType)
        {
            std::string out = sanitizeDashAbiFragment(name) + "__";
            if (parameters.empty())
                out += "0__";
            for (const auto &param : parameters)
            {
                if (param.isVariadic)
                    out += "var__";
                else
                    out += encodeDashAbiType(param.type) + "__";
            }
            out += "ret__" + encodeDashAbiType(returnType);
            return out;
        }

        [[nodiscard]] std::string dashAbiFunctionLogicalKey(const core::SourceLocation &location, const std::string &name, const std::vector<dash::ast::Parameter> &parameters, const core::TypeRef &returnType)
        {
            return dashAbiModuleName(location) + "::__" + dashAbiFunctionUnqualifiedKey(name, parameters, returnType);
        }

        [[nodiscard]] std::string dashAbiGlobalUnqualifiedKey(const std::string &name, const core::TypeRef &type)
        {
            return sanitizeDashAbiFragment(name) + "__" + encodeDashAbiType(type);
        }

        [[nodiscard]] std::string dashAbiGlobalLogicalKey(const core::SourceLocation &location, const std::string &name, const core::TypeRef &type)
        {
            return dashAbiModuleName(location) + "::__" + dashAbiGlobalUnqualifiedKey(name, type);
        }

        [[nodiscard]] std::optional<std::string> dashAbiFunctionUnqualifiedKeyFromSymbol(const std::string &symbol)
        {
            constexpr std::string_view prefix = "__dash_fn__";
            if (!symbol.starts_with(prefix))
                return std::nullopt;
            const auto rest = symbol.substr(prefix.size());
            const auto moduleSep = rest.find("__");
            if (moduleSep == std::string::npos || moduleSep + 2 >= rest.size())
                return std::nullopt;
            return rest.substr(moduleSep + 2);
        }

        [[nodiscard]] std::optional<std::string> dashAbiGlobalUnqualifiedKeyFromSymbol(const std::string &symbol)
        {
            constexpr std::string_view prefix = "__dash_var__";
            if (!symbol.starts_with(prefix))
                return std::nullopt;
            const auto rest = symbol.substr(prefix.size());
            const auto moduleSep = rest.find("__");
            if (moduleSep == std::string::npos || moduleSep + 2 >= rest.size())
                return std::nullopt;
            return rest.substr(moduleSep + 2);
        }

        [[nodiscard]] bool isSharedLibraryExtension(const std::filesystem::path &path)
        {
#if defined(_WIN32)
            return path.extension() == ".dll";
#elif defined(__APPLE__)
            const auto ext = path.extension().string();
            return ext == ".dylib" || ext == ".so";
#else
            return path.extension() == ".so";
#endif
        }

        [[nodiscard]] bool isStaticLinkInputExtension(const std::filesystem::path &path)
        {
            const auto ext = path.extension().string();
            return ext == ".o" || ext == ".a" || ext == ".obj" || ext == ".lib";
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

        [[nodiscard]] std::string normalizePathString(const std::string &value)
        {
            if (value.empty())
                return {};
            std::error_code ec;
            const auto normalized = std::filesystem::weakly_canonical(std::filesystem::path(value), ec);
            if (ec)
                return std::filesystem::absolute(std::filesystem::path(value), ec).string();
            return normalized.string();
        }

        struct StaticLinkInputInfo
        {
            std::filesystem::path path;
            std::unordered_set<std::string> definedSymbols;
            std::unordered_set<std::string> undefinedSymbols;
        };

        struct DashSupportLinkPlan
        {
            std::vector<std::filesystem::path> linkInputs;
            bool usesSharedDashLibs{false};
        };

        [[nodiscard]] StaticLinkInputInfo scanStaticLinkInput(const std::filesystem::path &path)
        {
            StaticLinkInputInfo info;
            info.path = path;

            const bool shared = isSharedLibraryExtension(path);
            const std::string command = shared
                ? "(nm -D " + quoteShell(path.string()) + " 2>/dev/null; nm " + quoteShell(path.string()) + " 2>/dev/null)"
                : "nm " + quoteShell(path.string()) + " 2>/dev/null";
            std::FILE *pipe = popen(command.c_str(), "r");
            if (pipe == nullptr)
                return info;

            char buffer[4096];
            while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
            {
                std::string line(buffer);
                std::istringstream stream(line);
                std::vector<std::string> tokens;
                std::string token;
                while (stream >> token)
                    tokens.push_back(token);
                if (tokens.size() < 2)
                    continue;

                std::string typeToken;
                std::string symbolToken;
                if (tokens.size() >= 3)
                {
                    typeToken = tokens[tokens.size() - 2];
                    symbolToken = tokens.back();
                }
                else
                {
                    typeToken = tokens.front();
                    symbolToken = tokens.back();
                }

                if (typeToken.size() != 1)
                    continue;

                const char type = typeToken[0];
                if (type == 'U')
                {
                    info.undefinedSymbols.insert(symbolToken);
                    continue;
                }
                if (!std::isalpha(static_cast<unsigned char>(type)) && type != '?')
                    continue;
                info.definedSymbols.insert(symbolToken);
            }

            pclose(pipe);
            return info;
        }

        [[nodiscard]] std::filesystem::path dashSharedSearchDir()
        {
            const char *home = std::getenv("HOME");
            if (home == nullptr || *home == '\0')
                return {};
            return std::filesystem::path(home) / ".dash" / "lib";
        }

        [[nodiscard]] std::filesystem::path equivalentDashSharedLibrary(const std::filesystem::path &staticPath)
        {
            const auto libDir = dashSharedSearchDir();
            if (libDir.empty())
                return {};

            const auto sharedPath = libDir / (staticPath.stem().string() + ".so");
            std::error_code ec;
            if (std::filesystem::exists(sharedPath, ec) && std::filesystem::is_regular_file(sharedPath, ec))
                return std::filesystem::absolute(sharedPath);
            return {};
        }

        [[nodiscard]] DashSupportLinkPlan discoverDashSupportLinkPlan(const std::unordered_set<std::string> &requiredSymbols, bool smartLinking)
        {
            DashSupportLinkPlan plan;
            if (requiredSymbols.empty())
                return plan;

            const char *home = std::getenv("HOME");
            if (home == nullptr || *home == '\0')
                return plan;

            const auto staticDir = std::filesystem::path(home) / ".dash" / "static";
            std::error_code ec;
            if (!std::filesystem::exists(staticDir, ec) || !std::filesystem::is_directory(staticDir, ec))
                return plan;

            std::vector<StaticLinkInputInfo> candidates;
            for (const auto &entry : std::filesystem::directory_iterator(staticDir, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;
                if (!isStaticLinkInputExtension(entry.path()))
                    continue;
                candidates.push_back(scanStaticLinkInput(std::filesystem::absolute(entry.path())));
            }

            std::sort(candidates.begin(), candidates.end(), [](const StaticLinkInputInfo &lhs, const StaticLinkInputInfo &rhs)
                      { return lhs.path < rhs.path; });

            std::unordered_set<std::string> unresolved = requiredSymbols;
            std::unordered_set<std::string> provided;
            std::vector<bool> selected(candidates.size(), false);

            bool changed = true;
            while (changed)
            {
                changed = false;
                for (std::size_t i = 0; i < candidates.size(); ++i)
                {
                    if (selected[i])
                        continue;

                    std::unordered_set<std::string> requiredCoverage;
                    for (const auto &symbol : candidates[i].definedSymbols)
                    {
                        if (unresolved.contains(symbol))
                            requiredCoverage.insert(symbol);
                    }
                    if (requiredCoverage.empty())
                        continue;

                    selected[i] = true;
                    changed = true;

                    bool usedStaticInput = true;
                    if (smartLinking)
                    {
                        const auto sharedPath = equivalentDashSharedLibrary(candidates[i].path);
                        if (!sharedPath.empty())
                        {
                            const auto sharedInfo = scanStaticLinkInput(sharedPath);
                            bool sharedCoversAll = true;
                            bool sharedCoversAny = false;
                            for (const auto &symbol : requiredCoverage)
                            {
                                if (sharedInfo.definedSymbols.contains(symbol))
                                {
                                    sharedCoversAny = true;
                                }
                                else
                                {
                                    sharedCoversAll = false;
                                }
                            }

                            if (sharedCoversAll)
                            {
                                plan.linkInputs.push_back(sharedPath);
                                plan.usesSharedDashLibs = true;
                                usedStaticInput = false;

                                for (const auto &symbol : sharedInfo.definedSymbols)
                                {
                                    provided.insert(symbol);
                                    unresolved.erase(symbol);
                                }
                            }
                            else if (sharedCoversAny)
                            {
                                plan.linkInputs.push_back(sharedPath);
                                plan.usesSharedDashLibs = true;
                            }
                        }
                    }

                    if (usedStaticInput)
                    {
                        plan.linkInputs.push_back(candidates[i].path);
                        for (const auto &symbol : candidates[i].definedSymbols)
                        {
                            provided.insert(symbol);
                            unresolved.erase(symbol);
                        }

                        for (const auto &symbol : candidates[i].undefinedSymbols)
                        {
                            if (!provided.contains(symbol))
                                unresolved.insert(symbol);
                        }
                    }
                }
            }

            return plan;
        }

        [[nodiscard]] std::vector<std::filesystem::path> discoverDashRuntimeLibraries()
        {
            std::vector<std::filesystem::path> libraries;
            const char *home = std::getenv("HOME");
            if (home == nullptr || *home == '\0')
                return libraries;

            const auto libDir = std::filesystem::path(home) / ".dash" / "lib";
            std::error_code ec;
            if (!std::filesystem::exists(libDir, ec) || !std::filesystem::is_directory(libDir, ec))
                return libraries;

            for (const auto &entry : std::filesystem::directory_iterator(libDir, ec))
            {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;
                if (isSharedLibraryExtension(entry.path()))
                    libraries.push_back(std::filesystem::absolute(entry.path()));
            }

            std::sort(libraries.begin(), libraries.end());
            return libraries;
        }

        [[nodiscard]] std::string dashRuntimeSearchDir()
        {
            const char *home = std::getenv("HOME");
            if (home == nullptr || *home == '\0')
                return {};
            return (std::filesystem::path(home) / ".dash" / "lib").string();
        }

        [[nodiscard]] char firstPrintfSpecifier(const std::string &format)
        {
            for (std::size_t i = 0; i + 1 < format.size(); ++i)
            {
                if (format[i] != '%')
                    continue;
                if (format[i + 1] == '%')
                {
                    ++i;
                    continue;
                }
                std::size_t j = i + 1;
                while (j < format.size())
                {
                    const char c = format[j];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                        return c;
                    ++j;
                }
                break;
            }
            return '\0';
        }

        [[nodiscard]] core::TypeRef makeUIntType()
        {
            return core::TypeRef{core::BuiltinTypeKind::UInt, ""};
        }

        [[nodiscard]] core::TypeRef makeArrayElementType(const core::TypeRef &type)
        {
            if (!type.isArray())
                return core::TypeRef{};
            return core::TypeRef{type.elementKind, type.elementName};
        }

        [[nodiscard]] llvm::Constant *makeArrayConstantElement(llvm::LLVMContext &context, llvm::Module &module, llvm::Type *elementLLVMType, const ast::Expr &expr, const std::string &name)
        {
            if (const auto *literal = dynamic_cast<const ast::IntegerLiteralExpr *>(&expr))
                return llvm::ConstantInt::get(elementLLVMType, literal->value, !literal->forceUnsigned);
            if (const auto *literal = dynamic_cast<const ast::DoubleLiteralExpr *>(&expr))
                return llvm::ConstantFP::get(elementLLVMType, literal->value);
            if (const auto *literal = dynamic_cast<const ast::BoolLiteralExpr *>(&expr))
                return llvm::ConstantInt::get(elementLLVMType, literal->value ? 1 : 0, false);
            if (const auto *literal = dynamic_cast<const ast::CharLiteralExpr *>(&expr))
                return llvm::ConstantInt::get(elementLLVMType, literal->value, false);
            if (const auto *literal = dynamic_cast<const ast::StringLiteralExpr *>(&expr))
            {
                auto *stringConstant = llvm::ConstantDataArray::getString(context, escapeString(literal->value), true);
                auto *stringGlobal = new llvm::GlobalVariable(
                    module,
                    stringConstant->getType(),
                    true,
                    llvm::GlobalValue::PrivateLinkage,
                    stringConstant,
                    name + ".str");
                stringGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                stringGlobal->setAlignment(llvm::Align(1));
                auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0);
                return llvm::ConstantExpr::getInBoundsGetElementPtr(
                    stringConstant->getType(),
                    stringGlobal,
                    llvm::ArrayRef<llvm::Constant *>{zero, zero});
            }
            return nullptr;
        }
        [[nodiscard]] llvm::Value *emitTrapIf(llvm::IRBuilder<> &builder, llvm::Module &module, llvm::LLVMContext &context, llvm::Function *function, llvm::Value *condition)
        {
            auto *okBlock = llvm::BasicBlock::Create(context, "array.bounds.ok", function);
            auto *trapBlock = llvm::BasicBlock::Create(context, "array.bounds.trap", function);
            builder.CreateCondBr(condition, okBlock, trapBlock);
            builder.SetInsertPoint(trapBlock);
            auto trap = llvm::Intrinsic::getOrInsertDeclaration(&module, llvm::Intrinsic::trap);
            builder.CreateCall(trap);
            builder.CreateUnreachable();
            builder.SetInsertPoint(okBlock);
            return nullptr;
        }

    } // namespace

    CodeGenerator::CodeGenerator()
    {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    }

    void CodeGenerator::compile(ast::Program &program, const CompileOptions &options)
    {
        emitShared_ = options.emitShared;
        rootInputPath_ = normalizePathString(options.inputPath);
        initializeModule(std::filesystem::path(options.inputPath).stem().string());
        emitDashSignature();
        populateManualDashAbiLinkSymbols(options.extraLinkArgs);

        for (const auto &decl : program.declarations)
        {
            if (const auto *global = dynamic_cast<const ast::GlobalVarDecl *>(decl.get()))
            {
                if (!global->isExtern)
                {
                    const auto symbolName = mangleDashAbiGlobalName(global->location, global->name, global->type);
                    dashAbiGlobalDefinitionSymbols_[dashAbiGlobalLogicalKey(global->location, global->name, global->type)] = symbolName;
                    const auto uq = dashAbiGlobalUnqualifiedKey(global->name, global->type);
                    if (const auto it = dashAbiGlobalUniqueSymbols_.find(uq); it == dashAbiGlobalUniqueSymbols_.end())
                        dashAbiGlobalUniqueSymbols_[uq] = symbolName;
                    else if (it->second != symbolName)
                        it->second.clear();
                }
                continue;
            }
            if (const auto *function = dynamic_cast<const ast::FunctionDecl *>(decl.get()))
            {
                if (function->name != "main")
                {
                    const auto symbolName = mangleDashAbiFunctionName(function->location, function->name, function->parameters, function->returnType);
                    dashAbiFunctionDefinitionSymbols_[dashAbiFunctionLogicalKey(function->location, function->name, function->parameters, function->returnType)] = symbolName;
                    const auto uq = dashAbiFunctionUnqualifiedKey(function->name, function->parameters, function->returnType);
                    if (const auto it = dashAbiFunctionUniqueSymbols_.find(uq); it == dashAbiFunctionUniqueSymbols_.end())
                        dashAbiFunctionUniqueSymbols_[uq] = symbolName;
                    else if (it->second != symbolName)
                        it->second.clear();
                }
                continue;
            }
        }

        for (auto &decl : program.declarations)
        {
            if (auto *klass = dynamic_cast<ast::ClassDecl *>(decl.get()))
            {
                declareClass(*klass);
            }
            else if (auto *enumDecl = dynamic_cast<ast::EnumDecl *>(decl.get()))
            {
                auto &items = enums_[enumDecl->name];
                for (const auto &item : enumDecl->items)
                    items[item.name] = item.value;
            }
        }

        for (auto &decl : program.declarations)
        {
            if (auto *global = dynamic_cast<ast::GlobalVarDecl *>(decl.get()))
            {
                declareGlobal(*global);
            }
        }

        for (const auto &decl : program.declarations)
        {
            if (const auto *externDecl = dynamic_cast<const ast::ExternDecl *>(decl.get()))
            {
                declareFunction(*externDecl);
            }
        }

        for (const auto &decl : program.declarations)
        {
            if (const auto *function = dynamic_cast<const ast::FunctionDecl *>(decl.get()))
            {
                declareFunction(*function);
            }
        }

        for (auto &decl : program.declarations)
        {
            if (auto *klass = dynamic_cast<ast::ClassDecl *>(decl.get()))
            {
                emitClassMethods(*klass);
            }
        }

        for (const auto &decl : program.declarations)
        {
            if (const auto *function = dynamic_cast<const ast::FunctionDecl *>(decl.get()))
            {
                emitFunction(*function);
            }
        }

        if (llvm::verifyModule(*module_, &llvm::errs()))
        {
            core::throwDiagnostic(core::SourceLocation{options.inputPath, 1, 1}, "LLVM module verification failed");
        }

        if (options.emitLLVM)
        {
            emitLLVMToFile(options.outputPath);
            return;
        }

        std::filesystem::path outputPath = options.outputPath;
        if (options.emitObjectOnly)
        {
            emitObjectFile(outputPath.string(), options.emitShared);
            return;
        }

        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(outputPath.parent_path());
        }
        const auto objectPath = outputPath.parent_path() / (outputPath.stem().string() + ".o");
        emitObjectFile(objectPath.string(), options.emitShared);
        if (options.emitShared)
            linkSharedLibrary(objectPath.string(), outputPath.string(), options.linkProfiles, options.extraLinkArgs, options.useDashRuntime, options.smartLinking);
        else
            linkExecutable(objectPath.string(), outputPath.string(), options.linkProfiles, options.extraLinkArgs, options.useDashRuntime, options.smartLinking);
        std::error_code removeError;
        std::filesystem::remove(objectPath, removeError);
    }

    void CodeGenerator::initializeModule(const std::string &moduleName)
    {
        module_ = std::make_unique<llvm::Module>(moduleName, context_);
        builder_ = std::make_unique<llvm::IRBuilder<>>(context_);
        functions_.clear();
        functionAbis_.clear();
        functionSymbolNames_.clear();
        dashAbiFunctionDefinitionSymbols_.clear();
        dashAbiFunctionUniqueSymbols_.clear();
        manualDashAbiFunctionUniqueSymbols_.clear();
        manualDashAbiModuleNames_.clear();
        dashAbiGlobalDefinitionSymbols_.clear();
        dashAbiGlobalUniqueSymbols_.clear();
        manualDashAbiGlobalUniqueSymbols_.clear();
        classes_.clear();
        functionParameterTypes_.clear();
        globals_.clear();
        globalTypes_.clear();
        globalSymbolNames_.clear();
        externalGlobalSymbols_.clear();
        manualLinkFunctionSymbols_.clear();
        manualLinkGlobalSymbols_.clear();
        usedExternCSymbols_.clear();
        localScopes_.clear();
        localTypeScopes_.clear();
        currentClass_ = nullptr;
        arrayType_ = llvm::StructType::create(context_, "dash.array");
        arrayType_->setBody({
            llvm::PointerType::get(context_, 0),
            llvm::Type::getInt64Ty(context_),
            llvm::Type::getInt64Ty(context_),
            llvm::Type::getInt1Ty(context_)});
        mallocFunction_ = nullptr;
        freeFunction_ = nullptr;
        reallocFunction_ = nullptr;
        memmoveFunction_ = nullptr;
        snprintfFunction_ = nullptr;
        strlenFunction_ = nullptr;
        dashInterpUIntLenFunction_ = nullptr;
        dashInterpWriteUIntFunction_ = nullptr;
        dashInterpWriteIntFunction_ = nullptr;
        powFunction_ = nullptr;

        module_->setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));

        std::string error;
        const llvm::Target *target = llvm::TargetRegistry::lookupTarget(module_->getTargetTriple(), error);
        if (target == nullptr)
        {
            core::throwDiagnostic(core::SourceLocation{}, "failed to resolve LLVM target: " + error);
        }

        llvm::TargetOptions options;
        const auto relocModel = emitShared_ ? std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_) : std::nullopt;
        auto targetMachine = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(module_->getTargetTriple(), "generic", "", options, relocModel));
        module_->setDataLayout(targetMachine->createDataLayout());
    }

    void CodeGenerator::emitDashSignature()
    {
        auto *signatureData = llvm::ConstantDataArray::getString(context_, kDashArtifactSignature, true);
        auto *signatureGlobal = new llvm::GlobalVariable(
            *module_,
            signatureData->getType(),
            true,
            llvm::GlobalValue::PrivateLinkage,
            signatureData,
            "__dash_signature");
        signatureGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        signatureGlobal->setAlignment(llvm::Align(1));
        signatureGlobal->setSection(".dash.signature");

        llvm::appendToUsed(*module_, {signatureGlobal});
        llvm::appendToCompilerUsed(*module_, {signatureGlobal});

        module_->addModuleFlag(llvm::Module::Warning, "dash.signature.present", 1U);
        module_->addModuleFlag(
            llvm::Module::Warning,
            "dash.signature.text",
            llvm::MDString::get(context_, kDashArtifactSignature));

        auto *named = module_->getOrInsertNamedMetadata("dash.signature");
        named->addOperand(llvm::MDNode::get(context_, {
            llvm::MDString::get(context_, "artifact"),
            llvm::MDString::get(context_, kDashArtifactSignature)}));
    }

    void CodeGenerator::declareClass(ast::ClassDecl &decl)
    {
        ClassLayout layout;
        layout.isStatic = decl.isStatic;
        layout.isGroup = decl.isGroup;
        layout.type = llvm::StructType::create(context_, decl.name);

        std::vector<llvm::Type *> fieldTypes;
        fieldTypes.reserve(decl.fields.size());
        std::size_t runtimeFieldIndex = 0;

        for (const auto &field : decl.fields)
        {
            layout.fieldTypes[field.name] = field.type;
            layout.fieldInitializers[field.name] = field.initializer;
            layout.fieldMutable[field.name] = field.isMutable;

            if (field.isExtern)
            {
                const std::string symbolName = field.abi == "c"
                    ? field.name
                    : ([&]() -> std::string {
                        const auto key = dashAbiGlobalLogicalKey(field.location, field.name, field.type);
                        if (const auto it = dashAbiGlobalDefinitionSymbols_.find(key); it != dashAbiGlobalDefinitionSymbols_.end())
                            return it->second;
                        const auto uq = dashAbiGlobalUnqualifiedKey(field.name, field.type);
                        if (const auto it = dashAbiGlobalUniqueSymbols_.find(uq); it != dashAbiGlobalUniqueSymbols_.end() && !it->second.empty())
                            return it->second;
                        return mangleDashAbiGlobalName(field.location, field.name, field.type);
                    })();
                auto *global = module_->getGlobalVariable(symbolName);
                if (global == nullptr)
                {
                    global = new llvm::GlobalVariable(
                        *module_,
                        toLLVMType(field.type),
                        !field.isMutable,
                        llvm::GlobalValue::ExternalLinkage,
                        nullptr,
                        symbolName);
                }
                layout.staticFields[field.name] = global;
                continue;
            }

            fieldTypes.push_back(toLLVMType(field.type));
            layout.fieldIndices[field.name] = runtimeFieldIndex++;
            layout.fieldOrder.push_back(field.name);
        }

        layout.type->setBody(fieldTypes, false);

        for (auto &method : decl.methods)
            layout.methods[method.name] = &method;

        classes_[decl.name] = layout;
        auto &storedLayout = classes_.at(decl.name);

        if (decl.isStatic)
        {
            core::TypeRef classType{};
            classType.kind = core::BuiltinTypeKind::Class;
            classType.name = decl.name;
            storedLayout.staticInstance = new llvm::GlobalVariable(
                *module_,
                storedLayout.type,
                false,
                llvm::GlobalValue::InternalLinkage,
                emitConstantDefaultValue(classType, nullptr, decl.name + ".instance.init"),
                decl.name + ".instance");
        }

        for (auto &method : decl.methods)
        {
            std::vector<llvm::Type *> parameterTypes;
            bool isVarArg = false;

            if (method.isExtern)
            {
                const bool isExternC = method.abi == "c";
                if (!decl.isStatic)
                    parameterTypes.push_back(llvm::PointerType::getUnqual(storedLayout.type));

                for (const auto &param : method.parameters)
                {
                    if (isExternC && param.isVariadic)
                    {
                        isVarArg = true;
                        break;
                    }
                    if (isExternC && param.type.isArray())
                        parameterTypes.push_back(llvm::PointerType::get(context_, 0));
                    else if (param.isVariadic)
                    {
                        parameterTypes.push_back(llvm::Type::getInt64Ty(context_));
                        parameterTypes.push_back(llvm::PointerType::get(context_, 0));
                    }
                    else
                        parameterTypes.push_back(toLLVMType(param.type));
                }

                const auto key = mangleMethodName(decl.name, method.name);
                const std::string symbolName = isExternC
                    ? method.name
                    : ([&]() -> std::string {
                        const auto key = dashAbiFunctionLogicalKey(method.location, method.name, method.parameters, method.returnType);
                        if (const auto it = dashAbiFunctionDefinitionSymbols_.find(key); it != dashAbiFunctionDefinitionSymbols_.end())
                            return it->second;
                        const auto uq = dashAbiFunctionUnqualifiedKey(method.name, method.parameters, method.returnType);
                        if (const auto it = dashAbiFunctionUniqueSymbols_.find(uq); it != dashAbiFunctionUniqueSymbols_.end() && !it->second.empty())
                            return it->second;
                        return mangleDashAbiFunctionName(method.location, method.name, method.parameters, method.returnType);
                    })();
                auto *functionType = llvm::FunctionType::get(toLLVMType(method.returnType), parameterTypes, isVarArg);
                auto *function = module_->getFunction(symbolName);
                if (function == nullptr)
                    function = llvm::Function::Create(functionType, llvm::Function::ExternalLinkage, symbolName, module_.get());
                function->setCallingConv(llvm::CallingConv::C);

                auto argIt = function->arg_begin();
                if (!decl.isStatic)
                {
                    argIt->setName("self");
                    ++argIt;
                }
                for (const auto &param : method.parameters)
                {
                    if (isExternC && param.isVariadic)
                        break;
                    if (!isExternC && param.isVariadic)
                    {
                        argIt->setName(param.name + "_size");
                        ++argIt;
                        argIt->setName(param.name + "_data");
                        ++argIt;
                        continue;
                    }
                    argIt->setName(param.name);
                    ++argIt;
                }

                functions_[key] = function;
                functionAbis_[key] = method.abi.empty() ? "dash" : method.abi;
                functionSymbolNames_[key] = symbolName;
                continue;
            }

            parameterTypes.push_back(llvm::PointerType::getUnqual(storedLayout.type));
            for (const auto &param : method.parameters)
            {
                if (param.isVariadic)
                {
                    parameterTypes.push_back(llvm::Type::getInt64Ty(context_));
                    parameterTypes.push_back(llvm::PointerType::get(context_, 0));
                    continue;
                }
                parameterTypes.push_back(toLLVMType(param.type));
            }

            auto *functionType = llvm::FunctionType::get(toLLVMType(method.returnType), parameterTypes, false);
            auto *function = llvm::Function::Create(functionType, llvm::Function::InternalLinkage, mangleMethodName(decl.name, method.name), module_.get());
            function->setCallingConv(llvm::CallingConv::C);

            auto argIt = function->arg_begin();
            argIt->setName("self");
            ++argIt;

            for (const auto &param : method.parameters)
            {
                if (param.isVariadic)
                {
                    argIt->setName(param.name + "_size");
                    ++argIt;
                    argIt->setName(param.name + "_data");
                    ++argIt;
                }
                else
                {
                    argIt->setName(param.name);
                    ++argIt;
                }
            }

            functions_[mangleMethodName(decl.name, method.name)] = function;
            functionSymbolNames_[mangleMethodName(decl.name, method.name)] = mangleMethodName(decl.name, method.name);
        }
    }

    void CodeGenerator::declareGlobal(ast::GlobalVarDecl &decl)
    {
        if (globals_.contains(decl.name))
            core::throwDiagnostic(decl.location, "duplicate global declaration: " + decl.name);

        const auto storageType = decl.type.kind == core::BuiltinTypeKind::Unknown
                                     ? core::TypeRef{core::BuiltinTypeKind::UInt, ""}
                                     : decl.type;

        if (decl.isExtern)
        {
            const bool isRootExtern = !rootInputPath_.empty() && normalizePathString(decl.location.file) == rootInputPath_;
            const std::string symbolName = decl.abi == "c"
                ? decl.name
                : ([&]() -> std::string {
                    const auto uq = dashAbiGlobalUnqualifiedKey(decl.name, storageType);
                    if (isRootExtern)
                    {
                        if (const auto it = manualDashAbiGlobalUniqueSymbols_.find(uq); it != manualDashAbiGlobalUniqueSymbols_.end() && !it->second.empty())
                            return it->second;
                        std::string guessed;
                        for (const auto &moduleName : manualDashAbiModuleNames_)
                        {
                            const auto candidate = std::string("__dash_var__") + moduleName + "__" + uq;
                            if (guessed.empty())
                                guessed = candidate;
                            else if (guessed != candidate)
                            {
                                guessed.clear();
                                break;
                            }
                        }
                        if (!guessed.empty())
                            return guessed;
                    }
                    const auto key = dashAbiGlobalLogicalKey(decl.location, decl.name, storageType);
                    if (const auto it = dashAbiGlobalDefinitionSymbols_.find(key); it != dashAbiGlobalDefinitionSymbols_.end())
                        return it->second;
                    if (const auto it = dashAbiGlobalUniqueSymbols_.find(uq); it != dashAbiGlobalUniqueSymbols_.end() && !it->second.empty())
                        return it->second;
                    if (isRootExtern)
                        core::throwDiagnostic(decl.location, "unresolved Dash extern global '" + decl.name + "'; pass the matching .o/.so explicitly in the dash command line");
                    return mangleDashAbiGlobalName(decl.location, decl.name, storageType);
                })();
            auto *global = module_->getGlobalVariable(symbolName);
            if (global == nullptr)
            {
                global = new llvm::GlobalVariable(
                    *module_,
                    toLLVMType(storageType),
                    !decl.isMutable,
                    llvm::GlobalValue::ExternalLinkage,
                    nullptr,
                    symbolName);
            }
            globals_[decl.name] = global;
            globalTypes_[decl.name] = storageType;
            globalSymbolNames_[decl.name] = symbolName;
            externalGlobalSymbols_.insert(symbolName);
            if (!rootInputPath_.empty() && normalizePathString(decl.location.file) == rootInputPath_)
                manualLinkGlobalSymbols_.insert(symbolName);
            return;
        }

        llvm::Constant *initializer = nullptr;

        if (storageType.isArray())
        {
            auto *dataPtrType = llvm::PointerType::get(context_, 0);
            auto *i64Type = llvm::Type::getInt64Ty(context_);
            auto *boolType = llvm::Type::getInt1Ty(context_);
            auto *nullData = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(dataPtrType));
            auto *zero64 = llvm::ConstantInt::get(i64Type, 0);
            auto *falseValue = llvm::ConstantInt::get(boolType, 0);

            if (const auto *array = dynamic_cast<const ast::ArrayLiteralExpr *>(decl.initializer.get()))
            {
                const auto elementType = makeArrayElementType(storageType);
                auto *elementLLVMType = toLLVMType(elementType);
                std::vector<llvm::Constant *> elementConstants;
                elementConstants.reserve(array->elements.size());
                for (std::size_t i = 0; i < array->elements.size(); ++i)
                {
                    auto *elementConstant = makeArrayConstantElement(context_, *module_, elementLLVMType, *array->elements[i], decl.name + ".arr." + std::to_string(i));
                    if (elementConstant == nullptr)
                        core::throwDiagnostic(decl.location, "global array initializers currently require literal elements");
                    elementConstants.push_back(elementConstant);
                }

                llvm::Constant *dataPointer = nullData;
                if (!elementConstants.empty())
                {
                    auto *arrayType = llvm::ArrayType::get(elementLLVMType, elementConstants.size());
                    auto *dataInit = llvm::ConstantArray::get(arrayType, elementConstants);
                    auto *dataGlobal = new llvm::GlobalVariable(
                        *module_,
                        arrayType,
                        !decl.isMutable,
                        llvm::GlobalValue::PrivateLinkage,
                        dataInit,
                        decl.name + ".arr.data");
                    auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0);
                    auto *typedPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(arrayType, dataGlobal, llvm::ArrayRef<llvm::Constant *>{zero, zero});
                    dataPointer = llvm::ConstantExpr::getPointerCast(typedPtr, dataPtrType);
                }

                const auto sizeValue = static_cast<std::uint64_t>(array->elements.size());
                initializer = llvm::ConstantStruct::get(arrayType_, dataPointer,
                    llvm::ConstantInt::get(i64Type, sizeValue),
                    llvm::ConstantInt::get(i64Type, storageType.hasArraySize ? storageType.arraySize : sizeValue),
                    falseValue);
            }
            else
            {
                initializer = llvm::ConstantStruct::get(arrayType_, nullData, zero64, zero64, falseValue);
            }
        }
        else if (decl.initializer)
        {
            if (storageType.kind == core::BuiltinTypeKind::Class)
            {
                if (const auto *groupLiteral = dynamic_cast<const ast::ArrayLiteralExpr *>(decl.initializer.get()))
                {
                    const auto &layout = classes_.at(storageType.name);
                    std::vector<llvm::Constant *> fieldConstants;
                    fieldConstants.reserve(layout.fieldOrder.size());
                    for (std::size_t i = 0; i < layout.fieldOrder.size(); ++i)
                    {
                        const auto &fieldName = layout.fieldOrder[i];
                        auto *fieldConstant = makeArrayConstantElement(context_, *module_, toLLVMType(layout.fieldTypes.at(fieldName)), *groupLiteral->elements[i], decl.name + ".grp." + std::to_string(i));
                        if (fieldConstant == nullptr)
                            core::throwDiagnostic(decl.location, "global group initializers currently require literal elements");
                        fieldConstants.push_back(fieldConstant);
                    }
                    initializer = llvm::ConstantStruct::get(layout.type, fieldConstants);
                }
            }
            if (initializer == nullptr)
            {
                initializer = emitConstantDefaultValue(storageType, decl.initializer.get(), decl.name);
                if (initializer == nullptr)
                    core::throwDiagnostic(decl.location, "global initializers currently require a constant literal");
            }
        }
        else
        {
            if (storageType.kind == core::BuiltinTypeKind::Class)
                initializer = emitConstantDefaultValue(storageType, nullptr, decl.name + ".default");
            else
                initializer = llvm::Constant::getNullValue(toLLVMType(storageType));
        }

        const std::string symbolName = decl.name == "main" ? decl.name : mangleDashAbiGlobalName(decl.location, decl.name, storageType);
        llvm::GlobalValue::LinkageTypes linkage = llvm::GlobalValue::ExternalLinkage;
        if (emitShared_ && !decl.isExport)
            linkage = llvm::GlobalValue::InternalLinkage;

        auto *global = new llvm::GlobalVariable(
            *module_,
            toLLVMType(storageType),
            !decl.isMutable,
            linkage,
            initializer,
            symbolName);

        if (emitShared_ && decl.isExport)
        {
            global->setVisibility(llvm::GlobalValue::DefaultVisibility);
#if defined(_WIN32)
            global->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
#endif
        }
        else if (emitShared_)
        {
            global->setVisibility(llvm::GlobalValue::HiddenVisibility);
        }

        globals_[decl.name] = global;
        globalTypes_[decl.name] = decl.type;
        globalSymbolNames_[decl.name] = symbolName;
    }

    void CodeGenerator::emitClassMethods(ast::ClassDecl &decl)
    {
        auto &layout = classes_.at(decl.name);
        auto &storedLayout = classes_.at(decl.name);

        for (const auto &method : decl.methods)
        {
            if (method.isExtern)
                continue;
            currentFunction_ = functions_.at(mangleMethodName(decl.name, method.name));
            currentReturnType_ = method.returnType;
            currentClass_ = &layout;
            localScopes_.clear();
            localTypeScopes_.clear();
            pushLocalScope();

            auto *entry = llvm::BasicBlock::Create(context_, "entry", currentFunction_);
            builder_->SetInsertPoint(entry);

            auto argIt = currentFunction_->arg_begin();
            llvm::Value *selfArg = &*argIt++;
            auto *selfAlloca = createEntryAlloca(currentFunction_, llvm::PointerType::getUnqual(storedLayout.type), "self");
            builder_->CreateStore(selfArg, selfAlloca);
            declareLocal("self", selfAlloca, core::TypeRef{core::BuiltinTypeKind::Class, decl.name});

            for (const auto &param : method.parameters)
            {
                if (param.isVariadic)
                {
                    llvm::Value *sizeArg = &*argIt++;
                    llvm::Value *dataArg = &*argIt++;

                    auto *sizeAlloca = createEntryAlloca(currentFunction_, llvm::Type::getInt64Ty(context_), param.name + "_size");
                    auto *dataAlloca = createEntryAlloca(currentFunction_, llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(context_)), param.name + "_data");
                    builder_->CreateStore(sizeArg, sizeAlloca);
                    builder_->CreateStore(dataArg, dataAlloca);
                    declareLocal(param.name + "_size", sizeAlloca, core::TypeRef{core::BuiltinTypeKind::UInt});
                    declareLocal(param.name + "_data", dataAlloca, core::TypeRef{core::BuiltinTypeKind::UInt});
                    continue;
                }

                llvm::Value *arg = &*argIt++;
                auto *alloca = createEntryAlloca(currentFunction_, toLLVMType(param.type), param.name);
                builder_->CreateStore(arg, alloca);
                declareLocal(param.name, alloca, param.type);
            }

            emitBlock(*method.body, false);

            if (builder_->GetInsertBlock()->getTerminator() == nullptr)
            {
                if (method.returnType.isVoid())
                    builder_->CreateRetVoid();
                else
                    builder_->CreateRet(emitValueOrDefault(method.returnType, nullptr));
            }

            popLocalScope();
            currentClass_ = nullptr;
        }
    }

    void CodeGenerator::declareFunction(const ast::ExternDecl &decl)
    {
        std::vector<llvm::Type *> parameterTypes;
        std::vector<core::TypeRef> parameterKinds;
        const bool isExternC = decl.abi == "c";
        bool isVarArg = false;

        for (const auto &param : decl.parameters)
        {
            if (isExternC && param.isVariadic)
            {
                isVarArg = true;
                break;
            }

            if (isExternC && param.type.isArray())
                parameterTypes.push_back(llvm::PointerType::get(context_, 0));
            else if (param.isVariadic)
            {
                parameterTypes.push_back(llvm::Type::getInt64Ty(context_));
                parameterTypes.push_back(llvm::PointerType::get(context_, 0));
            }
            else
                parameterTypes.push_back(toLLVMType(param.type));

            if (!param.isVariadic)
                parameterKinds.push_back(param.type);
        }

        auto *functionType =
            llvm::FunctionType::get(
                toLLVMType(decl.returnType),
                parameterTypes,
                isVarArg);

        const bool isRootExtern = !rootInputPath_.empty() && normalizePathString(decl.location.file) == rootInputPath_;
        const std::string symbolName = isExternC
            ? decl.name
            : ([&]() -> std::string {
                const auto uq = dashAbiFunctionUnqualifiedKey(decl.name, decl.parameters, decl.returnType);
                if (isRootExtern)
                {
                    if (const auto it = manualDashAbiFunctionUniqueSymbols_.find(uq); it != manualDashAbiFunctionUniqueSymbols_.end() && !it->second.empty())
                        return it->second;
                    std::string guessed;
                    for (const auto &moduleName : manualDashAbiModuleNames_)
                    {
                        const auto candidate = std::string("__dash_fn__") + moduleName + "__" + uq;
                        if (guessed.empty())
                            guessed = candidate;
                        else if (guessed != candidate)
                        {
                            guessed.clear();
                            break;
                        }
                    }
                    if (!guessed.empty())
                        return guessed;
                }
                const auto key = dashAbiFunctionLogicalKey(decl.location, decl.name, decl.parameters, decl.returnType);
                if (const auto it = dashAbiFunctionDefinitionSymbols_.find(key); it != dashAbiFunctionDefinitionSymbols_.end())
                    return it->second;
                if (const auto it = dashAbiFunctionUniqueSymbols_.find(uq); it != dashAbiFunctionUniqueSymbols_.end() && !it->second.empty())
                    return it->second;
                if (isRootExtern)
                    core::throwDiagnostic(decl.location, "unresolved Dash extern function '" + decl.name + "'; pass the matching .o/.so explicitly in the dash command line");
                return mangleDashAbiFunctionName(decl.location, decl.name, decl.parameters, decl.returnType);
            })();
        if (isRootExtern)
            manualLinkFunctionSymbols_.insert(symbolName);
        llvm::Function *function = module_->getFunction(symbolName);
        if (function == nullptr)
        {
            function = llvm::Function::Create(
                functionType,
                llvm::Function::ExternalLinkage,
                symbolName,
                module_.get());
        }

        function->setCallingConv(llvm::CallingConv::C);

        auto argIt = function->arg_begin();

        for (const auto &param : decl.parameters)
        {
            if (isExternC && param.isVariadic)
                break;

            if (!isExternC && param.isVariadic)
            {
                argIt->setName(param.name + "_size");
                ++argIt;
                argIt->setName(param.name + "_data");
                ++argIt;
                continue;
            }

            argIt->setName(param.name);
            ++argIt;
        }

        functions_[decl.name] = function;
        functionAbis_[decl.name] = decl.abi.empty() ? "dash" : decl.abi;
        functionSymbolNames_[decl.name] = symbolName;
        functionParameterTypes_[decl.name] = std::move(parameterKinds);
    }

    void CodeGenerator::declareFunction(const ast::FunctionDecl &decl)
    {
        std::vector<llvm::Type *> parameterTypes;
        llvm::Type *returnType = toLLVMType(decl.returnType);

        if (decl.name == "main")
        {
            returnType = llvm::Type::getInt32Ty(context_);
            if (!decl.parameters.empty())
                parameterTypes.push_back(llvm::Type::getInt32Ty(context_));
            if (decl.parameters.size() >= 2)
                parameterTypes.push_back(llvm::PointerType::get(context_, 0));
        }
        else
        {
            for (const auto &param : decl.parameters)
            {
                if (param.isVariadic)
                {
                    parameterTypes.push_back(llvm::Type::getInt64Ty(context_));
                    parameterTypes.push_back(llvm::PointerType::get(context_, 0));
                    continue;
                }

                parameterTypes.push_back(toLLVMType(param.type));
            }
        }

        auto *functionType =
            llvm::FunctionType::get(returnType, parameterTypes, false);

        const std::string symbolName = decl.name == "main" ? decl.name : mangleDashAbiFunctionName(decl.location, decl.name, decl.parameters, decl.returnType);
        llvm::GlobalValue::LinkageTypes linkage = llvm::Function::ExternalLinkage;
        if (emitShared_ && decl.name != "main" && !decl.isExport)
            linkage = llvm::Function::InternalLinkage;

        auto *function = module_->getFunction(symbolName);
        if (function == nullptr)
        {
            function = llvm::Function::Create(
                functionType,
                linkage,
                symbolName,
                module_.get());
        }
        else
        {
            function->setLinkage(linkage);
        }

        function->setCallingConv(llvm::CallingConv::C);
        if (emitShared_ && decl.isExport)
        {
            function->setVisibility(llvm::GlobalValue::DefaultVisibility);
#if defined(_WIN32)
            function->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
#endif
        }
        else if (emitShared_ && decl.name != "main")
        {
            function->setVisibility(llvm::GlobalValue::HiddenVisibility);
        }

        auto argIt = function->arg_begin();

        for (const auto &param : decl.parameters)
        {
            if (param.isVariadic)
            {
                argIt->setName(param.name + "_size");
                ++argIt;

                argIt->setName(param.name + "_data");
                ++argIt;
            }
            else
            {
                argIt->setName(param.name);
                ++argIt;
            }
        }

        functions_[decl.name] = function;
        functionAbis_[decl.name] = "dash";
        functionSymbolNames_[decl.name] = symbolName;

        std::vector<core::TypeRef> parameterKinds;
        for (const auto &param : decl.parameters)
        {
            if (!param.isVariadic)
                parameterKinds.push_back(param.type);
        }

        functionParameterTypes_[decl.name] = std::move(parameterKinds);
    }

    void CodeGenerator::emitFunction(const ast::FunctionDecl &decl)
    {
        currentFunction_ = functions_.at(decl.name);
        currentReturnType_ = decl.returnType;
        localScopes_.clear();
        localTypeScopes_.clear();
        pushLocalScope();

        auto *entry = llvm::BasicBlock::Create(context_, "entry", currentFunction_);
        builder_->SetInsertPoint(entry);

        std::size_t index = 0;
        auto argIt = currentFunction_->arg_begin();

        for (const auto &param : decl.parameters)
        {
            if (param.isVariadic)
            {
                llvm::Value *sizeArg = &*argIt++;
                llvm::Value *dataArg = &*argIt++;

                auto *sizeAlloca = createEntryAlloca(
                    currentFunction_,
                    llvm::Type::getInt64Ty(context_),
                    param.name + "_size");

                auto *dataAlloca = createEntryAlloca(
                    currentFunction_,
                    llvm::PointerType::getUnqual(llvm::Type::getInt64Ty(context_)),
                    param.name + "_data");

                builder_->CreateStore(sizeArg, sizeAlloca);
                builder_->CreateStore(dataArg, dataAlloca);

                declareLocal(param.name + "_size",
                             sizeAlloca,
                             core::TypeRef{core::BuiltinTypeKind::UInt});

                declareLocal(param.name + "_data",
                             dataAlloca,
                             core::TypeRef{core::BuiltinTypeKind::UInt});

                continue;
            }

            llvm::Value *arg = &*argIt++;

            llvm::Type *storageType = toLLVMType(param.type);
            auto *alloca = createEntryAlloca(currentFunction_, storageType, param.name);

            if (decl.name == "main" && param.type.isArray())
            {
                llvm::Value *result = llvm::UndefValue::get(arrayType_);
                result = builder_->CreateInsertValue(result, builder_->CreatePointerCast(arg, llvm::PointerType::get(context_, 0)), {0});
                llvm::Value *argcValue = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0);
                if (!decl.parameters.empty())
                {
                    auto *argcAlloca = lookupLocal(decl.parameters[0].name, decl.parameters[0].location);
                    argcValue = builder_->CreateLoad(toLLVMType(decl.parameters[0].type), argcAlloca);
                    argcValue = builder_->CreateIntCast(argcValue, llvm::Type::getInt64Ty(context_), false);
                }
                result = builder_->CreateInsertValue(result, argcValue, {1});
                result = builder_->CreateInsertValue(result, argcValue, {2});
                result = builder_->CreateInsertValue(result, llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), 0), {3});
                builder_->CreateStore(result, alloca);
                declareLocal(param.name, alloca, param.type);
                continue;
            }

            llvm::Value *storedValue = arg;

            if (decl.name == "main")
            {
                storedValue = castMainAbiArgument(*builder_, context_, storedValue, param.type);
            }

            storedValue = castValue(storedValue, param.type, param.type);

            builder_->CreateStore(storedValue, alloca);

            declareLocal(param.name, alloca, param.type);
        }

        emitBlock(*decl.body, false);

        if (builder_->GetInsertBlock()->getTerminator() == nullptr)
        {
            if (decl.name == "main")
            {
                builder_->CreateRet(llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0));
            }
            else if (decl.returnType.isVoid())
            {
                builder_->CreateRetVoid();
            }
            else
            {
                builder_->CreateRet(emitValueOrDefault(decl.returnType, nullptr));
            }
        }

        popLocalScope();
    }

    void CodeGenerator::emitBlock(const ast::BlockStmt &block, bool createScope)
    {
        if (createScope)
        {
            pushLocalScope();
        }

        for (const auto &stmt : block.statements)
        {
            if (builder_->GetInsertBlock()->getTerminator() != nullptr)
            {
                break;
            }
            emitStatement(*stmt);
        }

        if (createScope)
        {
            popLocalScope();
        }
    }

    void CodeGenerator::emitStatement(const ast::Stmt &stmt)
    {
        if (const auto *block = dynamic_cast<const ast::BlockStmt *>(&stmt))
        {
            emitBlock(*block);
            return;
        }
        if (const auto *switchStmt = dynamic_cast<const ast::SwitchStmt *>(&stmt))
        {
            emitStatement(*switchStmt->lowered);
            return;
        }
        if (const auto *groupStmt = dynamic_cast<const ast::DeclGroupStmt *>(&stmt))
        {
            for (const auto &sub : groupStmt->statements)
                emitStatement(*sub);
            return;
        }
        if (const auto *matchStmt = dynamic_cast<const ast::MatchStmt *>(&stmt))
        {
            emitStatement(*matchStmt->lowered);
            return;
        }

        if (const auto *variable = dynamic_cast<const ast::VariableDeclStmt *>(&stmt))
        {
            const auto storageType = variable->type.kind == core::BuiltinTypeKind::Unknown
                                         ? core::TypeRef{core::BuiltinTypeKind::UInt, ""}
                                         : variable->type;
            auto *alloca = createEntryAlloca(currentFunction_, toLLVMType(storageType), variable->name);

            if (variable->initializer)
            {
                llvm::Value *value = nullptr;
                if (storageType.isArray())
                {
                    const auto *arrayLiteral = dynamic_cast<const ast::ArrayLiteralExpr *>(variable->initializer.get());
                    if (arrayLiteral == nullptr)
                        core::throwDiagnostic(variable->location, "array variables currently require an array literal initializer");
                    value = emitArrayLiteralValue(*arrayLiteral, storageType);
                }
                else if (storageType.kind == core::BuiltinTypeKind::Class)
                {
                    if (const auto *groupLiteral = dynamic_cast<const ast::ArrayLiteralExpr *>(variable->initializer.get()))
                    {
                        value = emitGroupLiteralValue(*groupLiteral, storageType);
                    }
                    else
                    {
                        value = emitExpr(*variable->initializer);
                        value = castValue(value, variable->initializer->inferredType, storageType);
                    }
                }
                else
                {
                    value = emitExpr(*variable->initializer);
                    value = castValue(value, variable->initializer->inferredType, storageType);
                }
                builder_->CreateStore(value, alloca);
                if (variable->type.kind == core::BuiltinTypeKind::Unknown)
                {
                    if (const auto *indexExpr = dynamic_cast<const ast::IndexExpr *>(variable->initializer.get()))
                    {
                        if (const auto *var = dynamic_cast<const ast::VariableExpr *>(indexExpr->object.get()))
                        {
                            if (std::any_of(localTypeScopes_.rbegin(), localTypeScopes_.rend(), [&](const auto &scope){ return scope.contains(var->name + "_size") && scope.contains(var->name + "_data"); }))
                            {
                                auto *tagAlloca = createEntryAlloca(currentFunction_, llvm::Type::getInt64Ty(context_), variable->name + "__tag");
                                auto *baseAlloca = lookupLocal(var->name + "_data", variable->location);
                                auto *base = builder_->CreateLoad(llvm::PointerType::get(context_, 0), baseAlloca, var->name + ".data.tagcopybase");
                                llvm::Value *index = emitExpr(*indexExpr->index);
                                index = builder_->CreateIntCast(index, llvm::Type::getInt64Ty(context_), false);
                                auto *tagIndex = builder_->CreateMul(index, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 2));
                                auto *ptr = builder_->CreateGEP(llvm::Type::getInt64Ty(context_), base, tagIndex);
                                auto *tagValue = builder_->CreateLoad(llvm::Type::getInt64Ty(context_), ptr, var->name + ".tag.copy");
                                builder_->CreateStore(tagValue, tagAlloca);
                                declareLocal(variable->name + "__tag", tagAlloca, core::TypeRef{core::BuiltinTypeKind::UInt, ""});
                                declareLocal(variable->name, alloca, variable->type);
                                return;
                            }
                        }
                    }
                }
            }
            else
            {
                llvm::Value *defaultValue = nullptr;
                if (storageType.kind == core::BuiltinTypeKind::Class)
                    defaultValue = emitDefaultClassValue(storageType);
                else
                    defaultValue = llvm::Constant::getNullValue(toLLVMType(storageType));
                builder_->CreateStore(defaultValue, alloca);
            }

            declareLocal(variable->name, alloca, variable->type);

            if (variable->type.kind == core::BuiltinTypeKind::Unknown && variable->initializer)
            {
                auto *tagAlloca = createEntryAlloca(currentFunction_, llvm::Type::getInt64Ty(context_), variable->name + "__tag");
                llvm::Value *tagValue = emitRuntimeVariadicTag(*variable->initializer);
                builder_->CreateStore(tagValue, tagAlloca);
                declareLocal(variable->name + "__tag", tagAlloca, core::TypeRef{core::BuiltinTypeKind::UInt, ""});
            }
            return;
        }

        if (const auto *assignment = dynamic_cast<const ast::AssignmentStmt *>(&stmt))
        {
            llvm::Value *value = emitExpr(*assignment->value);
            for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it)
            {
                if (const auto found = it->find(assignment->name); found != it->end())
                {
                    auto *alloca = found->second;
                    const auto targetType = lookupLocalType(assignment->name, assignment->location);
                    value = castValue(value, assignment->value->inferredType, targetType);
                    builder_->CreateStore(value, alloca);
                    if (targetType.kind == core::BuiltinTypeKind::Unknown)
                    {
                        auto *tagAlloca = lookupLocal(assignment->name + "__tag", assignment->location);
                        if (const auto *indexExpr = dynamic_cast<const ast::IndexExpr *>(assignment->value.get()))
                        {
                            if (const auto *var = dynamic_cast<const ast::VariableExpr *>(indexExpr->object.get()))
                            {
                                if (std::any_of(localTypeScopes_.rbegin(), localTypeScopes_.rend(), [&](const auto &scope){ return scope.contains(var->name + "_size") && scope.contains(var->name + "_data"); }))
                                {
                                    auto *baseAlloca = lookupLocal(var->name + "_data", assignment->location);
                                    auto *base = builder_->CreateLoad(llvm::PointerType::get(context_, 0), baseAlloca, var->name + ".data.tagcopybase");
                                    llvm::Value *index = emitExpr(*indexExpr->index);
                                    index = builder_->CreateIntCast(index, llvm::Type::getInt64Ty(context_), false);
                                    auto *tagIndex = builder_->CreateMul(index, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 2));
                                    auto *ptr = builder_->CreateGEP(llvm::Type::getInt64Ty(context_), base, tagIndex);
                                    auto *tagValue = builder_->CreateLoad(llvm::Type::getInt64Ty(context_), ptr, var->name + ".tag.copy");
                                    builder_->CreateStore(tagValue, tagAlloca);
                                    return;
                                }
                            }
                        }
                        builder_->CreateStore(emitRuntimeVariadicTag(*assignment->value), tagAlloca);
                    }
                    return;
                }
            }
            if (const auto found = globals_.find(assignment->name); found != globals_.end())
            {
                value = castValue(value, assignment->value->inferredType, globalTypes_.at(assignment->name));
                builder_->CreateStore(value, found->second);
                return;
            }
            core::throwDiagnostic(assignment->location, "unknown variable: " + assignment->name);
        }

        if (const auto *assignment = dynamic_cast<const ast::MemberAssignmentStmt *>(&stmt))
        {
            const auto objectType = assignment->object->inferredType;
            const auto &layout = classes_.at(objectType.name);
            auto *value = emitExpr(*assignment->value);
            value = castValue(value, assignment->value->inferredType, layout.fieldTypes.at(assignment->member));
            if (const auto staticField = layout.staticFields.find(assignment->member); staticField != layout.staticFields.end())
            {
                usedExternCSymbols_.insert(staticField->second->getName().str());
                builder_->CreateStore(value, staticField->second);
                return;
            }
            auto *fieldPtr = emitObjectPointer(*assignment->object);
            const auto fieldIndex = layout.fieldIndices.at(assignment->member);
            auto *memberPtr = builder_->CreateStructGEP(layout.type, fieldPtr, static_cast<unsigned>(fieldIndex), assignment->member + ".ptr");
            builder_->CreateStore(value, memberPtr);
            return;
        }

        if (const auto *derefAssign = dynamic_cast<const ast::DerefAssignmentStmt *>(&stmt))
        {
            llvm::Value *pointerValue = emitExpr(*derefAssign->pointer);
            llvm::Value *value = emitExpr(*derefAssign->value);
            llvm::Type *storeType = toLLVMType(derefAssign->value->inferredType);
            if (derefAssign->pointer->inferredType.isPointer())
            {
                storeType = toLLVMType(derefAssign->pointer->inferredType.pointeeType());
            }
            if (pointerValue->getType()->isIntegerTy())
            {
                pointerValue = builder_->CreateIntToPtr(pointerValue, llvm::PointerType::getUnqual(storeType), "deref.assign.ptr");
            }
            else if (pointerValue->getType()->isPointerTy() && pointerValue->getType() != llvm::PointerType::getUnqual(storeType))
            {
                pointerValue = builder_->CreatePointerCast(pointerValue, llvm::PointerType::getUnqual(storeType), "deref.assign.cast");
            }
            if (derefAssign->pointer->inferredType.isPointer())
            {
                value = castValue(value, derefAssign->value->inferredType, derefAssign->pointer->inferredType.pointeeType());
            }
            builder_->CreateStore(value, pointerValue);
            return;
        }
        if (const auto *ret = dynamic_cast<const ast::ReturnStmt *>(&stmt))
        {
            if (ret->value)
            {
                llvm::Value *value = emitExpr(*ret->value);
                value = castValue(value, ret->value->inferredType, currentReturnType_);
                if (currentFunction_->getName() == "main")
                {
                    value = builder_->CreateIntCast(value, llvm::Type::getInt32Ty(context_), true, "main.ret.cast");
                }
                builder_->CreateRet(value);
            }
            else
            {
                builder_->CreateRetVoid();
            }
            return;
        }

        if (const auto *ifStmt = dynamic_cast<const ast::IfStmt *>(&stmt))
        {
            llvm::Value *condition = emitExpr(*ifStmt->condition);
            if (condition->getType()->isIntegerTy() && !condition->getType()->isIntegerTy(1))
            {
                condition = builder_->CreateICmpNE(
                    condition,
                    llvm::ConstantInt::get(condition->getType(), 0),
                    "if.cond.bool");
            }
            else if (condition->getType()->isPointerTy())
            {
                condition = builder_->CreateICmpNE(
                    condition,
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(condition->getType())),
                    "if.cond.bool");
            }
            auto *thenBlock = llvm::BasicBlock::Create(context_, "if.then", currentFunction_);
            auto *mergeBlock = llvm::BasicBlock::Create(context_, "if.end", currentFunction_);
            llvm::BasicBlock *elseBlock = mergeBlock;

            if (ifStmt->elseBranch != nullptr)
            {
                elseBlock = llvm::BasicBlock::Create(context_, "if.else", currentFunction_);
            }

            builder_->CreateCondBr(condition, thenBlock, elseBlock);

            builder_->SetInsertPoint(thenBlock);
            emitBlock(*ifStmt->thenBlock);
            if (builder_->GetInsertBlock()->getTerminator() == nullptr)
            {
                builder_->CreateBr(mergeBlock);
            }

            if (ifStmt->elseBranch != nullptr)
            {
                builder_->SetInsertPoint(elseBlock);
                emitStatement(*ifStmt->elseBranch);
                if (builder_->GetInsertBlock()->getTerminator() == nullptr)
                {
                    builder_->CreateBr(mergeBlock);
                }
            }

            builder_->SetInsertPoint(mergeBlock);
            return;
        }

        if (const auto *whileStmt = dynamic_cast<const ast::WhileStmt *>(&stmt))
        {
            auto *conditionBlock = llvm::BasicBlock::Create(context_, "while.cond", currentFunction_);
            auto *bodyBlock = llvm::BasicBlock::Create(context_, "while.body", currentFunction_);
            auto *endBlock = llvm::BasicBlock::Create(context_, "while.end", currentFunction_);

            builder_->CreateBr(conditionBlock);

            builder_->SetInsertPoint(conditionBlock);
            llvm::Value *condition = emitExpr(*whileStmt->condition);
            if (condition->getType()->isIntegerTy() && !condition->getType()->isIntegerTy(1))
            {
                condition = builder_->CreateICmpNE(
                    condition,
                    llvm::ConstantInt::get(condition->getType(), 0),
                    "while.cond.bool");
            }
            else if (condition->getType()->isPointerTy())
            {
                condition = builder_->CreateICmpNE(
                    condition,
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(condition->getType())),
                    "while.cond.bool");
            }
            builder_->CreateCondBr(condition, bodyBlock, endBlock);

            builder_->SetInsertPoint(bodyBlock);
            emitBlock(*whileStmt->body);
            if (builder_->GetInsertBlock()->getTerminator() == nullptr)
            {
                builder_->CreateBr(conditionBlock);
            }

            builder_->SetInsertPoint(endBlock);
            return;
        }

        if (const auto *doWhileStmt = dynamic_cast<const ast::DoWhileStmt *>(&stmt))
        {
            auto *bodyBlock = llvm::BasicBlock::Create(context_, "do.body", currentFunction_);
            auto *conditionBlock = llvm::BasicBlock::Create(context_, "do.cond", currentFunction_);
            auto *endBlock = llvm::BasicBlock::Create(context_, "do.end", currentFunction_);

            builder_->CreateBr(bodyBlock);

            builder_->SetInsertPoint(bodyBlock);
            emitBlock(*doWhileStmt->body);
            if (builder_->GetInsertBlock()->getTerminator() == nullptr)
            {
                builder_->CreateBr(conditionBlock);
            }

            builder_->SetInsertPoint(conditionBlock);
            llvm::Value *condition = emitExpr(*doWhileStmt->condition);
            if (condition->getType()->isIntegerTy() && !condition->getType()->isIntegerTy(1))
            {
                condition = builder_->CreateICmpNE(
                    condition,
                    llvm::ConstantInt::get(condition->getType(), 0),
                    "do.cond.bool");
            }
            else if (condition->getType()->isPointerTy())
            {
                condition = builder_->CreateICmpNE(
                    condition,
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(condition->getType())),
                    "do.cond.bool");
            }
            builder_->CreateCondBr(condition, bodyBlock, endBlock);

            builder_->SetInsertPoint(endBlock);
            return;
        }

        if (const auto *forStmt = dynamic_cast<const ast::ForStmt *>(&stmt))
        {
            pushLocalScope();
            if (forStmt->initializer != nullptr)
            {
                emitStatement(*forStmt->initializer);
            }

            auto *conditionBlock = llvm::BasicBlock::Create(context_, "for.cond", currentFunction_);
            auto *bodyBlock = llvm::BasicBlock::Create(context_, "for.body", currentFunction_);
            auto *stepBlock = llvm::BasicBlock::Create(context_, "for.step", currentFunction_);
            auto *endBlock = llvm::BasicBlock::Create(context_, "for.end", currentFunction_);

            builder_->CreateBr(conditionBlock);

            builder_->SetInsertPoint(conditionBlock);
            llvm::Value *condition = nullptr;
            if (forStmt->condition != nullptr)
            {
                condition = emitExpr(*forStmt->condition);
            }
            else
            {
                condition = llvm::ConstantInt::getTrue(context_);
            }
            if (condition->getType()->isIntegerTy() && !condition->getType()->isIntegerTy(1))
            {
                condition = builder_->CreateICmpNE(
                    condition,
                    llvm::ConstantInt::get(condition->getType(), 0),
                    "for.cond.bool");
            }
            else if (condition->getType()->isPointerTy())
            {
                condition = builder_->CreateICmpNE(
                    condition,
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(condition->getType())),
                    "for.cond.bool");
            }
            builder_->CreateCondBr(condition, bodyBlock, endBlock);

            builder_->SetInsertPoint(bodyBlock);
            emitBlock(*forStmt->body, false);
            if (builder_->GetInsertBlock()->getTerminator() == nullptr)
            {
                builder_->CreateBr(stepBlock);
            }

            builder_->SetInsertPoint(stepBlock);
            if (forStmt->increment != nullptr)
            {
                emitStatement(*forStmt->increment);
            }
            if (builder_->GetInsertBlock()->getTerminator() == nullptr)
            {
                builder_->CreateBr(conditionBlock);
            }

            builder_->SetInsertPoint(endBlock);
            popLocalScope();
            return;
        }

        if (dynamic_cast<const ast::BreakStmt *>(&stmt) != nullptr)
        {
            return;
        }
        if (const auto *exprStmt = dynamic_cast<const ast::ExprStmt *>(&stmt))
        {
            (void)emitExpr(*exprStmt->expr);
            return;
        }

        core::throwDiagnostic(stmt.location, "unsupported statement during LLVM emission");
    }

    llvm::Value *CodeGenerator::emitExpr(const ast::Expr &expr)
    {
        if (const auto *literal = dynamic_cast<const ast::IntegerLiteralExpr *>(&expr))
        {
            if (literal->forceUnsigned)
                return llvm::ConstantInt::get(toLLVMType(expr.inferredType), literal->value, false);
            return llvm::ConstantInt::get(toLLVMType(expr.inferredType), literal->value, true);
        }
        if (const auto *array = dynamic_cast<const ast::ArrayLiteralExpr *>(&expr))
        {
            return emitArrayLiteralValue(*array, expr.inferredType);
        }
        if (dynamic_cast<const ast::NullLiteralExpr *>(&expr) != nullptr)
        {
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0, false);
        }
        if (const auto *extract = dynamic_cast<const ast::ExtractDataExpr *>(&expr))
        {
            const auto operandType = extract->operand->inferredType;
            if (operandType.isArray())
            {
                auto *arrayAddress = emitAddressOf(*extract->operand);
                auto *data = emitArrayDataPointer(arrayAddress, operandType);
                return builder_->CreatePtrToInt(data, llvm::Type::getInt64Ty(context_), "exdt.array");
            }
            if (operandType.isClass())
            {
                auto *objectPtr = emitObjectPointer(*extract->operand);
                return builder_->CreatePtrToInt(objectPtr, llvm::Type::getInt64Ty(context_), "exdt.object");
            }
            auto *value = emitExpr(*extract->operand);
            if (value->getType()->isPointerTy())
                return builder_->CreatePtrToInt(value, llvm::Type::getInt64Ty(context_), "exdt.ptr");
            if (value->getType()->isIntegerTy(64))
                return value;
            if (value->getType()->isIntegerTy())
                return builder_->CreateIntCast(value, llvm::Type::getInt64Ty(context_), false, "exdt.int");
            if (value->getType()->isDoubleTy())
                return builder_->CreateBitCast(value, llvm::Type::getInt64Ty(context_), "exdt.double");
            core::throwDiagnostic(expr.location, "unsupported operand for exdt during LLVM emission");
        }
        if (const auto *parenExpr = dynamic_cast<const ast::ParenExpr *>(&expr))
        {
            if (parenExpr->preferVariadicCount)
            {
                if (const auto *var = dynamic_cast<const ast::VariableExpr *>(parenExpr->operand.get()))
                {
                    for (auto it = localTypeScopes_.rbegin(); it != localTypeScopes_.rend(); ++it)
                    {
                        if (it->contains(var->name + "_size") && it->contains(var->name + "_data"))
                        {
                            auto *sizeAlloca = lookupLocal(var->name + "_size", expr.location);
                            return builder_->CreateLoad(sizeAlloca->getAllocatedType(), sizeAlloca, var->name + ".paren.size");
                        }
                    }
                }
            }
            return emitExpr(*parenExpr->operand);
        }
        if (const auto *cast = dynamic_cast<const ast::CastExpr *>(&expr))
        {
            const auto fromType = cast->operand->inferredType;
            const auto toType = cast->targetType;

            if (fromType.isClass())
            {
                const auto &sourceLayout = classes_.at(fromType.name);
                auto value = emitExpr(*cast->operand);
                if (sourceLayout.fieldOrder.size() == 1)
                {
                    const auto &fieldName = sourceLayout.fieldOrder[0];
                    const auto &fieldType = sourceLayout.fieldTypes.at(fieldName);
                    auto *fieldValue = builder_->CreateExtractValue(value, {0}, fieldName + ".cast.extract");
                    if (toType.isClass())
                    {
                        const auto &targetLayout = classes_.at(toType.name);
                        if (targetLayout.fieldOrder.size() == 1)
                        {
                            const auto &targetFieldName = targetLayout.fieldOrder[0];
                            const auto &targetFieldType = targetLayout.fieldTypes.at(targetFieldName);
                            auto *castedField = castValue(fieldValue, fieldType, targetFieldType);
                            llvm::Value *result = llvm::UndefValue::get(targetLayout.type);
                            result = builder_->CreateInsertValue(result, castedField, {0}, toType.name + ".cast.pack");
                            return result;
                        }
                    }
                    return castValue(fieldValue, fieldType, toType);
                }
            }

            if (toType.isClass())
            {
                const auto &targetLayout = classes_.at(toType.name);
                if (targetLayout.fieldOrder.size() == 1)
                {
                    const auto &targetFieldName = targetLayout.fieldOrder[0];
                    const auto &targetFieldType = targetLayout.fieldTypes.at(targetFieldName);
                    auto *value = emitExpr(*cast->operand);
                    auto *castedField = castValue(value, fromType, targetFieldType);
                    llvm::Value *result = llvm::UndefValue::get(targetLayout.type);
                    result = builder_->CreateInsertValue(result, castedField, {0}, toType.name + ".cast.pack");
                    return result;
                }
            }

            auto *value = emitExpr(*cast->operand);
            return castValue(value, fromType, toType);
        }
        if (const auto *sizeExpr = dynamic_cast<const ast::SizeExpr *>(&expr))
        {
            if (sizeExpr->object->inferredType.isArray())
                return emitArraySizeValue(emitAddressOf(*sizeExpr->object));
            if (const auto *var = dynamic_cast<const ast::VariableExpr *>(sizeExpr->object.get()))
            {
                auto *sizeAlloca = lookupLocal(var->name + "_size", expr.location);
                return builder_->CreateLoad(sizeAlloca->getAllocatedType(), sizeAlloca, var->name + ".size");
            }
        }
        if (const auto *indexExpr = dynamic_cast<const ast::IndexExpr *>(&expr))
        {
            if (indexExpr->object->inferredType.isArray())
            {
                auto *arrayAddress = emitAddressOf(*indexExpr->object);
                auto *dataPtr = emitArrayDataPointer(arrayAddress, indexExpr->object->inferredType);
                auto elementType = makeArrayElementType(indexExpr->object->inferredType);
                auto *typedPtr = builder_->CreatePointerCast(dataPtr, llvm::PointerType::getUnqual(toLLVMType(elementType)));
                auto *indexValue = emitExpr(*indexExpr->index);
                indexValue = builder_->CreateIntCast(indexValue, llvm::Type::getInt64Ty(context_), false);
                auto *elementPtr = builder_->CreateGEP(toLLVMType(elementType), typedPtr, indexValue, "array.idx.ptr");
                return builder_->CreateLoad(toLLVMType(elementType), elementPtr, "array.idx");
            }
            if (const auto *var = dynamic_cast<const ast::VariableExpr *>(indexExpr->object.get()))
            {
                auto *baseAlloca = lookupLocal(var->name + "_data", expr.location);
                auto *base = builder_->CreateLoad(llvm::PointerType::get(context_, 0), baseAlloca, var->name + ".data");
                llvm::Value *index = emitExpr(*indexExpr->index);
                index = builder_->CreateIntCast(index, llvm::Type::getInt64Ty(context_), false);
                auto *payloadIndex = builder_->CreateAdd(builder_->CreateMul(index, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 2)), llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1));
                auto *ptr = builder_->CreateGEP(llvm::Type::getInt64Ty(context_), base, payloadIndex);
                return builder_->CreateLoad(llvm::Type::getInt64Ty(context_), ptr);
            }
        }
        if (const auto *push = dynamic_cast<const ast::ArrayPushExpr *>(&expr))
        {
            auto *arrayAddress = emitAddressOf(*push->array);
            const auto arrayType = push->array->inferredType;
            auto *currentSize = emitArraySizeValue(arrayAddress);
            llvm::Value *indexValue = currentSize;
            if (push->index)
            {
                indexValue = emitExpr(*push->index);
                indexValue = builder_->CreateIntCast(indexValue, llvm::Type::getInt64Ty(context_), false);
                auto *indexInRange = builder_->CreateICmpULE(indexValue, currentSize);
                (void)emitTrapIf(*builder_, *module_, context_, currentFunction_, indexInRange);
            }
            auto *minCapacity = builder_->CreateAdd(currentSize, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1));
            auto *dataPtr = ensureMutableArrayStorage(arrayAddress, arrayType, minCapacity);
            auto elementType = makeArrayElementType(arrayType);
            auto *elementLLVMType = toLLVMType(elementType);
            auto *typedData = builder_->CreatePointerCast(dataPtr, llvm::PointerType::getUnqual(elementLLVMType));
            auto *tailCount = builder_->CreateSub(currentSize, indexValue);
            auto elementSize = module_->getDataLayout().getTypeAllocSize(elementLLVMType);
            auto *elementSizeValue = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), static_cast<std::uint64_t>(elementSize));
            auto *shouldMove = builder_->CreateICmpUGT(tailCount, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0));
            auto *moveBlock = llvm::BasicBlock::Create(context_, "array.push.move", currentFunction_);
            auto *storeBlock = llvm::BasicBlock::Create(context_, "array.push.store", currentFunction_);
            builder_->CreateCondBr(shouldMove, moveBlock, storeBlock);
            builder_->SetInsertPoint(moveBlock);
            auto *src = builder_->CreateGEP(elementLLVMType, typedData, indexValue);
            auto *dst = builder_->CreateGEP(elementLLVMType, typedData, builder_->CreateAdd(indexValue, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1)));
            auto *bytes = builder_->CreateMul(tailCount, elementSizeValue);
            builder_->CreateCall(getOrCreateMemmove(), {builder_->CreatePointerCast(dst, llvm::PointerType::get(context_, 0)), builder_->CreatePointerCast(src, llvm::PointerType::get(context_, 0)), bytes});
            builder_->CreateBr(storeBlock);
            builder_->SetInsertPoint(storeBlock);
            auto *slot = builder_->CreateGEP(elementLLVMType, typedData, indexValue);
            auto *value = emitExpr(*push->value);
            value = castValue(value, push->value->inferredType, elementType);
            builder_->CreateStore(value, slot);
            storeArraySizeValue(arrayAddress, builder_->CreateAdd(currentSize, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1)));
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), 0);
        }
        if (const auto *insert = dynamic_cast<const ast::ArrayInsertExpr *>(&expr))
        {
            auto *arrayAddress = emitAddressOf(*insert->array);
            const auto arrayType = insert->array->inferredType;
            auto *currentSize = emitArraySizeValue(arrayAddress);
            auto *indexValue = emitExpr(*insert->index);
            indexValue = builder_->CreateIntCast(indexValue, llvm::Type::getInt64Ty(context_), false);
            auto *indexInRange = builder_->CreateICmpULE(indexValue, currentSize);
            (void)emitTrapIf(*builder_, *module_, context_, currentFunction_, indexInRange);
            auto *minCapacity = builder_->CreateAdd(currentSize, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1));
            auto *dataPtr = ensureMutableArrayStorage(arrayAddress, arrayType, minCapacity);
            auto elementType = makeArrayElementType(arrayType);
            auto *elementLLVMType = toLLVMType(elementType);
            auto *typedData = builder_->CreatePointerCast(dataPtr, llvm::PointerType::getUnqual(elementLLVMType));
            auto *tailCount = builder_->CreateSub(currentSize, indexValue);
            auto elementSize = module_->getDataLayout().getTypeAllocSize(elementLLVMType);
            auto *elementSizeValue = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), static_cast<std::uint64_t>(elementSize));
            auto *shouldMove = builder_->CreateICmpUGT(tailCount, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0));
            auto *moveBlock = llvm::BasicBlock::Create(context_, "array.insert.move", currentFunction_);
            auto *storeBlock = llvm::BasicBlock::Create(context_, "array.insert.store", currentFunction_);
            builder_->CreateCondBr(shouldMove, moveBlock, storeBlock);
            builder_->SetInsertPoint(moveBlock);
            auto *src = builder_->CreateGEP(elementLLVMType, typedData, indexValue);
            auto *dst = builder_->CreateGEP(elementLLVMType, typedData, builder_->CreateAdd(indexValue, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1)));
            auto *bytes = builder_->CreateMul(tailCount, elementSizeValue);
            builder_->CreateCall(getOrCreateMemmove(), {builder_->CreatePointerCast(dst, llvm::PointerType::get(context_, 0)), builder_->CreatePointerCast(src, llvm::PointerType::get(context_, 0)), bytes});
            builder_->CreateBr(storeBlock);
            builder_->SetInsertPoint(storeBlock);
            auto *slot = builder_->CreateGEP(elementLLVMType, typedData, indexValue);
            auto *value = emitExpr(*insert->value);
            value = castValue(value, insert->value->inferredType, elementType);
            builder_->CreateStore(value, slot);
            storeArraySizeValue(arrayAddress, builder_->CreateAdd(currentSize, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1)));
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), 0);
        }
        if (const auto *set = dynamic_cast<const ast::ArraySetExpr *>(&expr))
        {
            auto *arrayAddress = emitAddressOf(*set->array);
            const auto arrayType = set->array->inferredType;
            auto *currentSize = emitArraySizeValue(arrayAddress);
            auto *indexValue = emitExpr(*set->index);
            indexValue = builder_->CreateIntCast(indexValue, llvm::Type::getInt64Ty(context_), false);
            auto *indexInRange = builder_->CreateICmpULT(indexValue, currentSize);
            (void)emitTrapIf(*builder_, *module_, context_, currentFunction_, indexInRange);
            auto *dataPtr = ensureMutableArrayStorage(arrayAddress, arrayType, currentSize);
            auto elementType = makeArrayElementType(arrayType);
            auto *elementLLVMType = toLLVMType(elementType);
            auto *typedData = builder_->CreatePointerCast(dataPtr, llvm::PointerType::getUnqual(elementLLVMType));
            auto *slot = builder_->CreateGEP(elementLLVMType, typedData, indexValue);
            auto *value = emitExpr(*set->value);
            value = castValue(value, set->value->inferredType, elementType);
            builder_->CreateStore(value, slot);
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), 0);
        }
        if (const auto *rem = dynamic_cast<const ast::ArrayRemoveExpr *>(&expr))
        {
            auto *arrayAddress = emitAddressOf(*rem->array);
            const auto arrayType = rem->array->inferredType;
            auto *currentSize = emitArraySizeValue(arrayAddress);
            auto *indexValue = emitExpr(*rem->index);
            indexValue = builder_->CreateIntCast(indexValue, llvm::Type::getInt64Ty(context_), false);
            auto *indexInRange = builder_->CreateICmpULT(indexValue, currentSize);
            (void)emitTrapIf(*builder_, *module_, context_, currentFunction_, indexInRange);
            auto *dataPtr = ensureMutableArrayStorage(arrayAddress, arrayType, currentSize);
            auto elementType = makeArrayElementType(arrayType);
            auto *elementLLVMType = toLLVMType(elementType);
            auto *typedData = builder_->CreatePointerCast(dataPtr, llvm::PointerType::getUnqual(elementLLVMType));
            auto *afterCount = builder_->CreateSub(builder_->CreateSub(currentSize, indexValue), llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1));
            auto *shouldMove = builder_->CreateICmpUGT(afterCount, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0));
            auto *moveBlock = llvm::BasicBlock::Create(context_, "array.rem.move", currentFunction_);
            auto *doneBlock = llvm::BasicBlock::Create(context_, "array.rem.done", currentFunction_);
            builder_->CreateCondBr(shouldMove, moveBlock, doneBlock);
            builder_->SetInsertPoint(moveBlock);
            auto *src = builder_->CreateGEP(elementLLVMType, typedData, builder_->CreateAdd(indexValue, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1)));
            auto *dst = builder_->CreateGEP(elementLLVMType, typedData, indexValue);
            auto elementSize = module_->getDataLayout().getTypeAllocSize(elementLLVMType);
            auto *bytes = builder_->CreateMul(afterCount, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), static_cast<std::uint64_t>(elementSize)));
            builder_->CreateCall(getOrCreateMemmove(), {builder_->CreatePointerCast(dst, llvm::PointerType::get(context_, 0)), builder_->CreatePointerCast(src, llvm::PointerType::get(context_, 0)), bytes});
            builder_->CreateBr(doneBlock);
            builder_->SetInsertPoint(doneBlock);
            storeArraySizeValue(arrayAddress, builder_->CreateSub(currentSize, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1)));
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), 0);
        }
        if (const auto *v = dynamic_cast<const ast::VariadicSizeExpr *>(&expr))
        {
            auto *sizeAlloca = lookupLocal(v->name + "_size", expr.location);
            return builder_->CreateLoad(sizeAlloca->getAllocatedType(), sizeAlloca, v->name + ".size");
        }
        if (const auto *v = dynamic_cast<const ast::VariadicIndexExpr *>(&expr))
        {
            auto *baseAlloca = lookupLocal(v->name + "_data", expr.location);
            auto *base = builder_->CreateLoad(llvm::PointerType::get(context_, 0), baseAlloca, v->name + ".data");
            llvm::Value *index = emitExpr(*v->index);
            index = builder_->CreateIntCast(index, llvm::Type::getInt64Ty(context_), false);
            auto *payloadIndex = builder_->CreateAdd(builder_->CreateMul(index, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 2)), llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1));
            auto *ptr = builder_->CreateGEP(llvm::Type::getInt64Ty(context_), base, payloadIndex);
            return builder_->CreateLoad(llvm::Type::getInt64Ty(context_), ptr);
        }
        if (const auto *literal = dynamic_cast<const ast::DoubleLiteralExpr *>(&expr))
            return llvm::ConstantFP::get(toLLVMType(expr.inferredType), literal->value);
        if (const auto *literal = dynamic_cast<const ast::BoolLiteralExpr *>(&expr))
            return llvm::ConstantInt::get(toLLVMType(expr.inferredType), literal->value ? 1 : 0, false);
        if (const auto *literal = dynamic_cast<const ast::CharLiteralExpr *>(&expr))
            return llvm::ConstantInt::get(toLLVMType(expr.inferredType), literal->value, false);
        if (const auto *literal = dynamic_cast<const ast::StringLiteralExpr *>(&expr))
            return builder_->CreateGlobalString(escapeString(literal->value));
        if (const auto *interpolated = dynamic_cast<const ast::InterpolatedStringExpr *>(&expr))
            return emitInterpolatedStringValue(*interpolated);
        if (const auto *variable = dynamic_cast<const ast::VariableExpr *>(&expr))
        {
            bool foundLocal = false;
            llvm::AllocaInst *alloca = nullptr;
            core::TypeRef storageType{};
            for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it)
            {
                if (const auto found = it->find(variable->name); found != it->end())
                {
                    alloca = found->second;
                    foundLocal = true;
                    break;
                }
            }
            if (foundLocal)
            {
                storageType = lookupLocalType(variable->name, variable->location);
                llvm::Value *loaded = builder_->CreateLoad(alloca->getAllocatedType(), alloca, variable->name + ".load");
                if (storageType.isClass() && alloca->getAllocatedType()->isPointerTy())
                    loaded = builder_->CreateLoad(toLLVMType(storageType), loaded, variable->name + ".objload");
                if (storageType.kind == core::BuiltinTypeKind::Unknown && expr.inferredType.kind != core::BuiltinTypeKind::Unknown)
                    return castValue(loaded, core::TypeRef{core::BuiltinTypeKind::UInt, ""}, expr.inferredType);
                return loaded;
            }
            if (const auto globalIt = globals_.find(variable->name); globalIt != globals_.end())
            {
                if (const auto symIt = globalSymbolNames_.find(variable->name);
                    symIt != globalSymbolNames_.end() && externalGlobalSymbols_.contains(symIt->second) && !manualLinkGlobalSymbols_.contains(symIt->second))
                    usedExternCSymbols_.insert(symIt->second);
                llvm::Value *loaded = builder_->CreateLoad(globalIt->second->getValueType(), globalIt->second, variable->name + ".gload");
                const auto storageType = globalTypes_.at(variable->name);
                if (storageType.kind == core::BuiltinTypeKind::Unknown && expr.inferredType.kind != core::BuiltinTypeKind::Unknown)
                    return castValue(loaded, core::TypeRef{core::BuiltinTypeKind::UInt, ""}, expr.inferredType);
                return loaded;
            }
            if (expr.inferredType.isClass())
            {
                const auto classIt = classes_.find(expr.inferredType.name);
                if (classIt != classes_.end() && classIt->second.isStatic && classIt->second.staticInstance != nullptr)
                    return builder_->CreateLoad(classIt->second.type, classIt->second.staticInstance, variable->name + ".static.load");
            }
            core::throwDiagnostic(variable->location, "unknown variable: " + variable->name);
        }
        if (const auto *member = dynamic_cast<const ast::MemberExpr *>(&expr))
        {
            if (const auto *var = dynamic_cast<const ast::VariableExpr *>(member->object.get()))
            {
                if (const auto enumIt = enums_.find(var->name); enumIt != enums_.end())
                {
                    const auto itemIt = enumIt->second.find(member->member);
                    if (itemIt == enumIt->second.end())
                        core::throwDiagnostic(member->location, "unknown enum item: " + member->member);
                    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), static_cast<std::uint64_t>(itemIt->second), true);
                }
            }
            const auto &layout = classes_.at(member->object->inferredType.name);
            if (const auto staticField = layout.staticFields.find(member->member); staticField != layout.staticFields.end())
            {
                usedExternCSymbols_.insert(staticField->second->getName().str());
                return builder_->CreateLoad(toLLVMType(expr.inferredType), staticField->second, member->member + ".extern.load");
            }
            auto *fieldPtr = emitObjectPointer(*member->object);
            const auto fieldIndex = layout.fieldIndices.at(member->member);
            auto *memberPtr = builder_->CreateStructGEP(layout.type, fieldPtr, static_cast<unsigned>(fieldIndex), member->member + ".ptr");
            return builder_->CreateLoad(toLLVMType(expr.inferredType), memberPtr, member->member + ".load");
        }
        if (const auto *ternary = dynamic_cast<const ast::TernaryExpr *>(&expr))
        {
            llvm::Value *condition = emitExpr(*ternary->condition);
            if (condition->getType()->isIntegerTy() && !condition->getType()->isIntegerTy(1))
            {
                condition = builder_->CreateICmpNE(condition, llvm::ConstantInt::get(condition->getType(), 0), "ternary.cond.bool");
            }
            else if (condition->getType()->isPointerTy())
            {
                condition = builder_->CreateICmpNE(condition, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(condition->getType())), "ternary.cond.bool");
            }

            auto *thenBlock = llvm::BasicBlock::Create(context_, "ternary.then", currentFunction_);
            auto *elseBlock = llvm::BasicBlock::Create(context_, "ternary.else", currentFunction_);
            auto *mergeBlock = llvm::BasicBlock::Create(context_, "ternary.merge", currentFunction_);

            builder_->CreateCondBr(condition, thenBlock, elseBlock);

            builder_->SetInsertPoint(thenBlock);
            llvm::Value *thenValue = emitExpr(*ternary->thenExpr);
            thenValue = castValue(thenValue, ternary->thenExpr->inferredType, expr.inferredType);
            auto *thenEnd = builder_->GetInsertBlock();
            if (thenEnd->getTerminator() == nullptr)
                builder_->CreateBr(mergeBlock);

            builder_->SetInsertPoint(elseBlock);
            llvm::Value *elseValue = emitExpr(*ternary->elseExpr);
            elseValue = castValue(elseValue, ternary->elseExpr->inferredType, expr.inferredType);
            auto *elseEnd = builder_->GetInsertBlock();
            if (elseEnd->getTerminator() == nullptr)
                builder_->CreateBr(mergeBlock);

            builder_->SetInsertPoint(mergeBlock);
            auto *phi = builder_->CreatePHI(toLLVMType(expr.inferredType), 2, "ternary.result");
            phi->addIncoming(thenValue, thenEnd);
            phi->addIncoming(elseValue, elseEnd);
            return phi;
        }

        if (const auto *method = dynamic_cast<const ast::MethodCallExpr *>(&expr))
        {
            const auto classType = method->object->inferredType;
            const auto &layout = classes_.at(classType.name);
            const auto mangled = mangleMethodName(classType.name, method->method);
            auto *callee = functions_.at(mangled);
            const auto *decl = layout.methods.at(method->method);
            std::vector<llvm::Value *> args;
            const bool variadic = !decl->parameters.empty() && decl->parameters.back().isVariadic;
            const std::size_t fixedCount = variadic ? decl->parameters.size() - 1 : decl->parameters.size();
            const auto appendPackedDashMethodVariadics = [&]() {
                std::vector<llvm::Value *> vargs;
                for (std::size_t i = fixedCount; i < method->arguments.size(); ++i)
                {
                    llvm::Value *value = emitExpr(*method->arguments[i]);
                    vargs.push_back(emitRuntimeVariadicTag(*method->arguments[i]));
                    if (value->getType()->isIntegerTy()) value = builder_->CreateIntCast(value, llvm::Type::getInt64Ty(context_), false);
                    else if (value->getType()->isPointerTy()) value = builder_->CreatePtrToInt(value, llvm::Type::getInt64Ty(context_));
                    else if (value->getType()->isDoubleTy()) value = builder_->CreateBitCast(value, llvm::Type::getInt64Ty(context_));
                    vargs.push_back(value);
                }
                args.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), vargs.size() / 2));
                if (vargs.empty())
                {
                    args.push_back(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(llvm::PointerType::get(context_, 0))));
                    return;
                }
                auto *arrayType = llvm::ArrayType::get(llvm::Type::getInt64Ty(context_), static_cast<std::uint64_t>(vargs.size()));
                auto *stackBuffer = createEntryAlloca(currentFunction_, arrayType, method->method + ".vargs.stack");
                auto *typedBase = builder_->CreateInBoundsGEP(
                    arrayType,
                    stackBuffer,
                    {
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0),
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0)
                    },
                    method->method + ".vargs.base");
                for (std::size_t i = 0; i < vargs.size(); ++i)
                {
                    auto *ptr = builder_->CreateGEP(llvm::Type::getInt64Ty(context_), typedBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), i));
                    builder_->CreateStore(vargs[i], ptr);
                }
                args.push_back(builder_->CreatePointerCast(typedBase, llvm::PointerType::get(context_, 0)));
            };
            const ast::VariadicForwardExpr *forwardExpr = nullptr;
            if (variadic && method->arguments.size() == fixedCount + 1)
                forwardExpr = dynamic_cast<const ast::VariadicForwardExpr *>(method->arguments.back().get());

            if (!decl->isExtern)
                args.push_back(emitObjectPointer(*method->object));
            else if (!layout.isStatic)
                args.push_back(emitObjectPointer(*method->object));

            for (std::size_t i = 0; i < std::min(method->arguments.size(), fixedCount); ++i)
            {
                llvm::Value *value = emitExpr(*method->arguments[i]);
                if (decl->isExtern && decl->abi == "c" && decl->parameters[i].type.isArray() && callee->isDeclaration())
                {
                    auto *arrayAddr = emitAddressOf(*method->arguments[i]);
                    value = emitArrayDataPointer(arrayAddr, method->arguments[i]->inferredType);
                }
                else
                {
                    value = castValue(value, method->arguments[i]->inferredType, decl->parameters[i].type);
                }
                args.push_back(value);
            }
            if (variadic)
            {
                if (decl->isExtern && decl->abi == "c")
                {
                    if (forwardExpr != nullptr)
                    {
                        auto *sizeAlloca = lookupLocal(forwardExpr->name + "_size", forwardExpr->location);
                        auto *dataAlloca = lookupLocal(forwardExpr->name + "_data", forwardExpr->location);
                        auto *sizeValue = builder_->CreateLoad(sizeAlloca->getAllocatedType(), sizeAlloca, forwardExpr->name + ".fwd.size");
                        auto *dataValue = builder_->CreateLoad(llvm::PointerType::get(context_, 0), dataAlloca, forwardExpr->name + ".fwd.data");
                        std::vector<llvm::Type *> forwardedTypes;
                        auto *calleeType = callee->getFunctionType();
                        for (unsigned i = 0; i < calleeType->getNumParams(); ++i)
                            forwardedTypes.push_back(calleeType->getParamType(i));
                        forwardedTypes.push_back(llvm::Type::getInt64Ty(context_));
                        forwardedTypes.push_back(llvm::PointerType::get(context_, 0));
                        auto *forwardType = llvm::FunctionType::get(calleeType->getReturnType(), forwardedTypes, false);
                        auto *forwardCallee = builder_->CreatePointerCast(callee, llvm::PointerType::getUnqual(forwardType), mangled + ".variadic.forward");
                        args.push_back(sizeValue);
                        args.push_back(dataValue);
                        if (decl->isExtern)
                            usedExternCSymbols_.insert(functionSymbolNames_.at(mangled));
                        return builder_->CreateCall(forwardType, forwardCallee, args, expr.inferredType.isVoid() ? "" : method->method + ".call");
                    }
                    for (std::size_t i = fixedCount; i < method->arguments.size(); ++i)
                        args.push_back(emitExpr(*method->arguments[i]));
                }
                else
                {
                    if (forwardExpr != nullptr)
                    {
                        auto *sizeAlloca = lookupLocal(forwardExpr->name + "_size", forwardExpr->location);
                        auto *dataAlloca = lookupLocal(forwardExpr->name + "_data", forwardExpr->location);
                        args.push_back(builder_->CreateLoad(sizeAlloca->getAllocatedType(), sizeAlloca, forwardExpr->name + ".fwd.size"));
                        args.push_back(builder_->CreateLoad(llvm::PointerType::get(context_, 0), dataAlloca, forwardExpr->name + ".fwd.data"));
                    }
                    else
                    {
                        appendPackedDashMethodVariadics();
                    }
                }
            }
            if (decl->isExtern)
                usedExternCSymbols_.insert(functionSymbolNames_.at(mangleMethodName(classType.name, method->method)));
            auto *callResult = builder_->CreateCall(callee, args, expr.inferredType.isVoid() ? "" : method->method + ".call");
            return callResult;
        }

        if (const auto *isExpr = dynamic_cast<const ast::IsTypeExpr *>(&expr))
        {
            const auto varType = lookupLocalType(isExpr->variable, isExpr->location);
            if (varType.kind == core::BuiltinTypeKind::Unknown)
            {
                auto *tagAlloca = lookupLocal(isExpr->variable + "__tag", isExpr->location);
                auto *tag = builder_->CreateLoad(llvm::Type::getInt64Ty(context_), tagAlloca, isExpr->variable + ".tag.load");
                auto *maskedTag = builder_->CreateAnd(tag, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), ~VariadicRefFlag), isExpr->variable + ".tag.mask");
                return builder_->CreateICmpEQ(maskedTag, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), variadicTagForType(isExpr->type)), isExpr->variable + ".is");
            }
            const bool result = (varType.kind == isExpr->type.kind);
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), result ? 1 : 0, false);
        }
        if (const auto *unary = dynamic_cast<const ast::UnaryExpr *>(&expr))
        {
            if (unary->op == '&')
                return emitAddressOf(*unary->operand);
            llvm::Value *operand = emitExpr(*unary->operand);
            if (unary->op == '*')
            {
                llvm::Type *pointeeType = expr.inferredType.kind == core::BuiltinTypeKind::Unknown ? llvm::Type::getInt64Ty(context_) : toLLVMType(expr.inferredType);
                if (operand->getType()->isIntegerTy())
                    operand = builder_->CreateIntToPtr(operand, llvm::PointerType::getUnqual(pointeeType), "deref.ptr");
                else if (operand->getType()->isPointerTy() && operand->getType() != llvm::PointerType::getUnqual(pointeeType))
                    operand = builder_->CreatePointerCast(operand, llvm::PointerType::getUnqual(pointeeType), "deref.cast");
                return builder_->CreateLoad(pointeeType, operand, "dereftmp");
            }
            if (unary->op == '!')
            {
                const auto operandType = unary->operand->inferredType;
                if (operandType.isBool())
                    return builder_->CreateNot(operand, "nottmp");
                if (operandType.isDouble())
                    return builder_->CreateFCmpOEQ(operand, llvm::ConstantFP::get(llvm::Type::getDoubleTy(context_), 0.0), "nottmp");
                return builder_->CreateICmpEQ(castValue(operand, operandType, core::TypeRef{core::BuiltinTypeKind::Int, ""}), llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0), "nottmp");
            }
            if (expr.inferredType.isDouble())
                return builder_->CreateFNeg(operand, "negtmp");
            return builder_->CreateNeg(operand, "negtmp");
        }
        if (const auto *binary = dynamic_cast<const ast::BinaryExpr *>(&expr))
        {
            auto toLogicalBool = [&](llvm::Value *value) -> llvm::Value *
            {
                if (value->getType()->isIntegerTy(1))
                    return value;
                if (value->getType()->isIntegerTy())
                    return builder_->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "logic.bool");
                if (value->getType()->isPointerTy())
                    return builder_->CreateICmpNE(value, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(value->getType())), "logic.bool");
                if (value->getType()->isDoubleTy())
                    return builder_->CreateFCmpONE(value, llvm::ConstantFP::get(value->getType(), 0.0), "logic.bool");
                core::throwDiagnostic(expr.location, "logical operator received unsupported operand type during LLVM emission");
            };

            if (binary->op == "||")
            {
                llvm::Value *lhsRaw = emitExpr(*binary->left);
                llvm::Value *lhsBool = toLogicalBool(lhsRaw);

                auto *rhsBlock = llvm::BasicBlock::Create(context_, "or.rhs", currentFunction_);
                auto *mergeBlock = llvm::BasicBlock::Create(context_, "or.end", currentFunction_);
                auto *lhsBlock = builder_->GetInsertBlock();

                builder_->CreateCondBr(lhsBool, mergeBlock, rhsBlock);

                builder_->SetInsertPoint(rhsBlock);
                llvm::Value *rhsRaw = emitExpr(*binary->right);
                llvm::Value *rhsBool = toLogicalBool(rhsRaw);
                auto *rhsEndBlock = builder_->GetInsertBlock();
                builder_->CreateBr(mergeBlock);

                builder_->SetInsertPoint(mergeBlock);
                auto *phi = builder_->CreatePHI(llvm::Type::getInt1Ty(context_), 2, "ortmp");
                phi->addIncoming(llvm::ConstantInt::getTrue(context_), lhsBlock);
                phi->addIncoming(rhsBool, rhsEndBlock);
                return phi;
            }

            if (binary->op == "&&")
            {
                llvm::Value *lhsRaw = emitExpr(*binary->left);
                llvm::Value *lhsBool = toLogicalBool(lhsRaw);

                auto *rhsBlock = llvm::BasicBlock::Create(context_, "and.rhs", currentFunction_);
                auto *mergeBlock = llvm::BasicBlock::Create(context_, "and.end", currentFunction_);
                auto *lhsBlock = builder_->GetInsertBlock();

                builder_->CreateCondBr(lhsBool, rhsBlock, mergeBlock);

                builder_->SetInsertPoint(rhsBlock);
                llvm::Value *rhsRaw = emitExpr(*binary->right);
                llvm::Value *rhsBool = toLogicalBool(rhsRaw);
                auto *rhsEndBlock = builder_->GetInsertBlock();
                builder_->CreateBr(mergeBlock);

                builder_->SetInsertPoint(mergeBlock);
                auto *phi = builder_->CreatePHI(llvm::Type::getInt1Ty(context_), 2, "andtmp");
                phi->addIncoming(llvm::ConstantInt::getFalse(context_), lhsBlock);
                phi->addIncoming(rhsBool, rhsEndBlock);
                return phi;
            }

            llvm::Value *lhs = emitExpr(*binary->left);
            llvm::Value *rhs = emitExpr(*binary->right);
            if (binary->op == "+" || binary->op == "-" || binary->op == "*" || binary->op == "/" || binary->op == "<<" || binary->op == ">>")
            {
                const auto lhsType = binary->left->inferredType;
                const auto rhsType = binary->right->inferredType;
                const bool lhsPointerLike = lhsType.isPointer() || lhsType.isString();
                const bool rhsPointerLike = rhsType.isPointer() || rhsType.isString();
                const bool multiplicativeAlias = binary->op == "<<" || binary->op == ">>";
                const bool isMulLike = binary->op == "*" || binary->op == "<<";
                const bool isDivLike = binary->op == "/" || binary->op == ">>";

                if (!multiplicativeAlias && (binary->op == "+" || binary->op == "-") && lhsPointerLike && (rhsType.isInt() || rhsType.isUInt()))
                {
                    auto *lhsInt = builder_->CreatePtrToInt(lhs, llvm::Type::getInt64Ty(context_), "ptrarith.lhs");
                    auto *rhsInt = castValue(rhs, rhsType, core::TypeRef{core::BuiltinTypeKind::UInt, ""});
                    auto *resultInt = (binary->op == "+") ? builder_->CreateAdd(lhsInt, rhsInt, "ptrarith.add") : builder_->CreateSub(lhsInt, rhsInt, "ptrarith.sub");
                    return builder_->CreateIntToPtr(resultInt, toLLVMType(expr.inferredType), "ptrarith.result");
                }
                if (!multiplicativeAlias && binary->op == "+" && (lhsType.isInt() || lhsType.isUInt()) && rhsPointerLike)
                {
                    auto *lhsInt = castValue(lhs, lhsType, core::TypeRef{core::BuiltinTypeKind::UInt, ""});
                    auto *rhsInt = builder_->CreatePtrToInt(rhs, llvm::Type::getInt64Ty(context_), "ptrarith.rhs");
                    auto *resultInt = builder_->CreateAdd(lhsInt, rhsInt, "ptrarith.add");
                    return builder_->CreateIntToPtr(resultInt, toLLVMType(expr.inferredType), "ptrarith.result");
                }
                if (!multiplicativeAlias && binary->op == "-" && lhsPointerLike && rhsPointerLike && lhsType == rhsType)
                {
                    auto *lhsInt = builder_->CreatePtrToInt(lhs, llvm::Type::getInt64Ty(context_), "ptrdiff.lhs");
                    auto *rhsInt = builder_->CreatePtrToInt(rhs, llvm::Type::getInt64Ty(context_), "ptrdiff.rhs");
                    return builder_->CreateSub(lhsInt, rhsInt, "ptrdiff");
                }

                lhs = castValue(lhs, binary->left->inferredType, expr.inferredType);
                rhs = castValue(rhs, binary->right->inferredType, expr.inferredType);
                if (expr.inferredType.isDouble())
                {
                    if (binary->op == "+") return builder_->CreateFAdd(lhs, rhs, "faddtmp");
                    if (binary->op == "-") return builder_->CreateFSub(lhs, rhs, "fsubtmp");
                    if (isMulLike) return builder_->CreateFMul(lhs, rhs, "fmultmp");
                    if (isDivLike) return builder_->CreateFDiv(lhs, rhs, "fdivtmp");
                }
                if (binary->op == "+") return builder_->CreateAdd(lhs, rhs, "addtmp");
                if (binary->op == "-") return builder_->CreateSub(lhs, rhs, "subtmp");
                if (isMulLike) return builder_->CreateMul(lhs, rhs, "multmp");
                if (isDivLike) return expr.inferredType.isUInt() ? builder_->CreateUDiv(lhs, rhs, "udivtmp") : builder_->CreateSDiv(lhs, rhs, "sdivtmp");
            }
            if (binary->op == "%")
            {
                lhs = castValue(lhs, binary->left->inferredType, expr.inferredType);
                rhs = castValue(rhs, binary->right->inferredType, expr.inferredType);
                if (expr.inferredType.isUInt())
                    return builder_->CreateURem(lhs, rhs, "uremtmp");
                return builder_->CreateSRem(lhs, rhs, "sremtmp");
            }
            if (binary->op == "^")
            {
                const auto arithmeticType = core::usualArithmeticType(binary->left->inferredType, binary->right->inferredType);
                auto *lhsPow = castValue(lhs, binary->left->inferredType, arithmeticType);
                auto *rhsPow = castValue(rhs, binary->right->inferredType, arithmeticType);
                auto *lhsDouble = arithmeticType.isDouble() ? lhsPow : castValue(lhsPow, arithmeticType, core::TypeRef{core::BuiltinTypeKind::Double, ""});
                auto *rhsDouble = arithmeticType.isDouble() ? rhsPow : castValue(rhsPow, arithmeticType, core::TypeRef{core::BuiltinTypeKind::Double, ""});
                auto *powValue = builder_->CreateCall(getOrCreatePow(), {lhsDouble, rhsDouble}, "powtmp");
                if (expr.inferredType.isDouble())
                    return powValue;
                return castValue(powValue, core::TypeRef{core::BuiltinTypeKind::Double, ""}, expr.inferredType);
            }
            const auto lhsTypeCmp = binary->left->inferredType;
            const auto rhsTypeCmp = binary->right->inferredType;
            const bool lhsPointerLikeCmp = lhsTypeCmp.isPointer() || lhsTypeCmp.isString();
            const bool rhsPointerLikeCmp = rhsTypeCmp.isPointer() || rhsTypeCmp.isString();
            if ((binary->op == "==" || binary->op == "!=") && ((lhsPointerLikeCmp && rhsPointerLikeCmp) || (lhsPointerLikeCmp && (rhsTypeCmp.isInt() || rhsTypeCmp.isUInt())) || ((lhsTypeCmp.isInt() || lhsTypeCmp.isUInt()) && rhsPointerLikeCmp)))
            {
                auto toPtrInt = [&](llvm::Value *value, const core::TypeRef &type) -> llvm::Value *
                {
                    if (type.isPointer() || type.isString())
                        return builder_->CreatePtrToInt(value, llvm::Type::getInt64Ty(context_), "ptrcmp.int");
                    return castValue(value, type, core::TypeRef{core::BuiltinTypeKind::UInt, ""});
                };
                auto *lhsCmp = toPtrInt(lhs, lhsTypeCmp);
                auto *rhsCmp = toPtrInt(rhs, rhsTypeCmp);
                if (binary->op == "==") return builder_->CreateICmpEQ(lhsCmp, rhsCmp, "ptrcmp.eq");
                return builder_->CreateICmpNE(lhsCmp, rhsCmp, "ptrcmp.ne");
            }

            if (binary->left->inferredType.isDouble() || binary->right->inferredType.isDouble())
            {
                const auto arithmeticType = core::usualArithmeticType(binary->left->inferredType, binary->right->inferredType);
                lhs = castValue(lhs, binary->left->inferredType, arithmeticType);
                rhs = castValue(rhs, binary->right->inferredType, arithmeticType);
                if (binary->op == "<") return builder_->CreateFCmpOLT(lhs, rhs, "fcmp.lt");
                if (binary->op == "<=") return builder_->CreateFCmpOLE(lhs, rhs, "fcmp.le");
                if (binary->op == ">") return builder_->CreateFCmpOGT(lhs, rhs, "fcmp.gt");
                if (binary->op == ">=") return builder_->CreateFCmpOGE(lhs, rhs, "fcmp.ge");
                if (binary->op == "==") return builder_->CreateFCmpOEQ(lhs, rhs, "fcmp.eq");
                if (binary->op == "!=") return builder_->CreateFCmpONE(lhs, rhs, "fcmp.ne");
            }
            if (binary->left->inferredType.isNumeric() && binary->right->inferredType.isNumeric())
            {
                const auto arithmeticType = core::usualArithmeticType(binary->left->inferredType, binary->right->inferredType);
                lhs = castValue(lhs, binary->left->inferredType, arithmeticType);
                rhs = castValue(rhs, binary->right->inferredType, arithmeticType);
                const bool isUnsigned = arithmeticType.isUInt();
                if (binary->op == "<") return isUnsigned ? builder_->CreateICmpULT(lhs, rhs, "icmp.lt") : builder_->CreateICmpSLT(lhs, rhs, "icmp.lt");
                if (binary->op == "<=") return isUnsigned ? builder_->CreateICmpULE(lhs, rhs, "icmp.le") : builder_->CreateICmpSLE(lhs, rhs, "icmp.le");
                if (binary->op == ">") return isUnsigned ? builder_->CreateICmpUGT(lhs, rhs, "icmp.gt") : builder_->CreateICmpSGT(lhs, rhs, "icmp.gt");
                if (binary->op == ">=") return isUnsigned ? builder_->CreateICmpUGE(lhs, rhs, "icmp.ge") : builder_->CreateICmpSGE(lhs, rhs, "icmp.ge");
                if (binary->op == "==") return builder_->CreateICmpEQ(lhs, rhs, "icmp.eq");
                if (binary->op == "!=") return builder_->CreateICmpNE(lhs, rhs, "icmp.ne");
            }
            if (binary->left->inferredType.isBool() && binary->right->inferredType.isBool())
            {
                if (binary->op == "==") return builder_->CreateICmpEQ(lhs, rhs, "icmp.eq");
                if (binary->op == "!=") return builder_->CreateICmpNE(lhs, rhs, "icmp.ne");
            }
            core::throwDiagnostic(expr.location, "unsupported binary operator during LLVM emission");
        }
        if (const auto *call = dynamic_cast<const ast::CallExpr *>(&expr))
        {
            auto *callee = functions_.at(call->callee);
            const auto &parameterTypes = functionParameterTypes_.at(call->callee);
            const auto abiIt = functionAbis_.find(call->callee);
            const std::string functionAbi = abiIt == functionAbis_.end() ? "dash" : abiIt->second;
            if (callee->isDeclaration())
            {
                const auto &symbolName = functionSymbolNames_.at(call->callee);
                if (!manualLinkFunctionSymbols_.contains(symbolName))
                    usedExternCSymbols_.insert(symbolName);
            }
            const bool callbackReference = call->arguments.empty() && !parameterTypes.empty();
            if (callbackReference)
                return builder_->CreatePtrToInt(callee, llvm::Type::getInt64Ty(context_), call->callee + ".fnptr");
            std::vector<llvm::Value *> args;
            const auto appendPackedDashVariadics = [&](std::size_t startIndex) {
                std::vector<llvm::Value *> vargs;
                for (std::size_t i = startIndex; i < call->arguments.size(); ++i)
                {
                    llvm::Value *value = emitExpr(*call->arguments[i]);
                    vargs.push_back(emitRuntimeVariadicTag(*call->arguments[i]));
                    if (value->getType()->isIntegerTy()) value = builder_->CreateIntCast(value, llvm::Type::getInt64Ty(context_), false);
                    else if (value->getType()->isPointerTy()) value = builder_->CreatePtrToInt(value, llvm::Type::getInt64Ty(context_));
                    else if (value->getType()->isDoubleTy()) value = builder_->CreateBitCast(value, llvm::Type::getInt64Ty(context_));
                    vargs.push_back(value);
                }
                args.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), vargs.size() / 2));
                if (vargs.empty())
                {
                    args.push_back(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(llvm::PointerType::get(context_, 0))));
                    return;
                }
                auto *arrayType = llvm::ArrayType::get(llvm::Type::getInt64Ty(context_), static_cast<std::uint64_t>(vargs.size()));
                auto *stackBuffer = createEntryAlloca(currentFunction_, arrayType, call->callee + ".vargs.stack");
                auto *typedBase = builder_->CreateInBoundsGEP(
                    arrayType,
                    stackBuffer,
                    {
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0),
                        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0)
                    },
                    call->callee + ".vargs.base");
                for (std::size_t i = 0; i < vargs.size(); ++i)
                {
                    auto *ptr = builder_->CreateGEP(llvm::Type::getInt64Ty(context_), typedBase, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), i));
                    builder_->CreateStore(vargs[i], ptr);
                }
                args.push_back(builder_->CreatePointerCast(typedBase, llvm::PointerType::get(context_, 0)));
            };
            const std::size_t fixedCount = parameterTypes.size();
            const ast::VariadicForwardExpr *forwardExpr = (call->arguments.size() == fixedCount + 1)
                ? dynamic_cast<const ast::VariadicForwardExpr *>(call->arguments.back().get())
                : nullptr;
            for (std::size_t i = 0; i < std::min(call->arguments.size(), fixedCount); ++i)
            {
                llvm::Value *value = emitExpr(*call->arguments[i]);
                if (functionAbi == "c" && parameterTypes[i].isArray() && callee->isDeclaration())
                {
                    auto *arrayAddr = emitAddressOf(*call->arguments[i]);
                    value = emitArrayDataPointer(arrayAddr, call->arguments[i]->inferredType);
                }
                else
                {
                    value = castValue(value, call->arguments[i]->inferredType, parameterTypes[i]);
                }
                args.push_back(value);
            }
            const bool isExternCVarArg = functionAbi == "c" && callee->isVarArg();
            if (call->arguments.size() > fixedCount)
            {
                if (isExternCVarArg)
                {
                    if (forwardExpr != nullptr)
                    {
                        auto *sizeAlloca = lookupLocal(forwardExpr->name + "_size", forwardExpr->location);
                        auto *dataAlloca = lookupLocal(forwardExpr->name + "_data", forwardExpr->location);
                        auto *sizeValue = builder_->CreateLoad(sizeAlloca->getAllocatedType(), sizeAlloca, forwardExpr->name + ".fwd.size");
                        auto *dataValue = builder_->CreateLoad(llvm::PointerType::get(context_, 0), dataAlloca, forwardExpr->name + ".fwd.data");
                        std::vector<llvm::Type *> forwardedTypes;
                        auto *calleeType = callee->getFunctionType();
                        for (unsigned i = 0; i < calleeType->getNumParams(); ++i)
                            forwardedTypes.push_back(calleeType->getParamType(i));
                        forwardedTypes.push_back(llvm::Type::getInt64Ty(context_));
                        forwardedTypes.push_back(llvm::PointerType::get(context_, 0));
                        auto *forwardType = llvm::FunctionType::get(calleeType->getReturnType(), forwardedTypes, false);
                        auto *forwardCallee = builder_->CreatePointerCast(callee, llvm::PointerType::getUnqual(forwardType), call->callee + ".variadic.forward");
                        args.push_back(sizeValue);
                        args.push_back(dataValue);
                        return builder_->CreateCall(forwardType, forwardCallee, args, expr.inferredType.isVoid() ? "" : call->callee + ".call");
                    }
                    std::string printfFormat;
                    bool hasPrintfFormat = false;
                    const bool isScanf = (call->callee == "scanf");
                    if (call->callee == "printf" && !call->arguments.empty())
                    {
                        if (const auto *fmt = dynamic_cast<const ast::StringLiteralExpr *>(call->arguments[0].get()))
                        {
                            printfFormat = fmt->value;
                            hasPrintfFormat = true;
                        }
                    }
                    for (std::size_t i = fixedCount; i < call->arguments.size(); ++i)
                    {
                        llvm::Value *value = emitExpr(*call->arguments[i]);
                        if (hasPrintfFormat)
                            value = adaptExternVarArgForPrintf(*call->arguments[i], value, printfFormat);
                        else if (isScanf && call->arguments[i]->inferredType.kind == core::BuiltinTypeKind::Unknown && value->getType()->isIntegerTy())
                            value = builder_->CreateIntToPtr(value, llvm::PointerType::get(context_, 0), "scanf.vararg.ptr");
                        args.push_back(value);
                    }
                }
                else
                {
                    if (forwardExpr != nullptr)
                    {
                        auto *sizeAlloca = lookupLocal(forwardExpr->name + "_size", forwardExpr->location);
                        auto *dataAlloca = lookupLocal(forwardExpr->name + "_data", forwardExpr->location);
                        args.push_back(builder_->CreateLoad(sizeAlloca->getAllocatedType(), sizeAlloca, forwardExpr->name + ".fwd.size"));
                        args.push_back(builder_->CreateLoad(llvm::PointerType::get(context_, 0), dataAlloca, forwardExpr->name + ".fwd.data"));
                    }
                    else
                    {
                        appendPackedDashVariadics(fixedCount);
                    }
                }
            }
            auto *callResult = builder_->CreateCall(callee, args, expr.inferredType.isVoid() ? "" : call->callee + ".call");
            return callResult;
        }
        core::throwDiagnostic(expr.location, "unsupported expression during LLVM emission");
    }

    llvm::Value *CodeGenerator::emitAddressOf(const ast::Expr &expr)
    {
        if (const auto *variable = dynamic_cast<const ast::VariableExpr *>(&expr))
        {
            for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it)
            {
                if (const auto found = it->find(variable->name); found != it->end())
                    return found->second;
            }
            if (const auto found = globals_.find(variable->name); found != globals_.end())
                return found->second;
        }
        if (const auto *member = dynamic_cast<const ast::MemberExpr *>(&expr))
        {
            const auto &layout = classes_.at(member->object->inferredType.name);
            if (const auto staticField = layout.staticFields.find(member->member); staticField != layout.staticFields.end())
            {
                usedExternCSymbols_.insert(staticField->second->getName().str());
                return staticField->second;
            }
            auto *fieldPtr = emitObjectPointer(*member->object);
            const auto fieldIndex = layout.fieldIndices.at(member->member);
            return builder_->CreateStructGEP(layout.type, fieldPtr, static_cast<unsigned>(fieldIndex), member->member + ".addr");
        }
        core::throwDiagnostic(expr.location, "address-of currently only supports variables and fields");
    }

    llvm::Value *CodeGenerator::emitObjectPointer(const ast::Expr &expr)
    {
        if (const auto *variable = dynamic_cast<const ast::VariableExpr *>(&expr))
        {
            for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it)
            {
                if (const auto found = it->find(variable->name); found != it->end())
                {
                    auto *alloca = found->second;
                    if (alloca->getAllocatedType()->isPointerTy())
                        return builder_->CreateLoad(alloca->getAllocatedType(), alloca, variable->name + ".self.ptr");
                    return alloca;
                }
            }

            if (const auto globalIt = globals_.find(variable->name); globalIt != globals_.end())
            {
                return globalIt->second;
            }

            if (expr.inferredType.isClass())
            {
                const auto classIt = classes_.find(expr.inferredType.name);
                if (classIt != classes_.end() && classIt->second.isStatic && classIt->second.staticInstance != nullptr)
                    return classIt->second.staticInstance;
            }

            core::throwDiagnostic(variable->location, "unknown class object: " + variable->name);
        }

        if (const auto *member = dynamic_cast<const ast::MemberExpr *>(&expr))
        {
            const auto &layout = classes_.at(member->object->inferredType.name);
            if (const auto staticField = layout.staticFields.find(member->member); staticField != layout.staticFields.end())
            {
                usedExternCSymbols_.insert(staticField->second->getName().str());
                return staticField->second;
            }
            auto *basePtr = emitObjectPointer(*member->object);
            const auto fieldIndex = layout.fieldIndices.at(member->member);
            return builder_->CreateStructGEP(layout.type, basePtr, static_cast<unsigned>(fieldIndex), member->member + ".addr");
        }

        core::throwDiagnostic(expr.location, "expression is not addressable as a class object");
    }

    llvm::Value *CodeGenerator::emitRuntimeVariadicTag(const ast::Expr &expr)
    {
        if (const auto *unary = dynamic_cast<const ast::UnaryExpr *>(&expr))
        {
            if (unary->op == '&')
                return llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), variadicTagForType(unary->operand->inferredType) | VariadicRefFlag);
        }
        if (const auto *indexExpr = dynamic_cast<const ast::IndexExpr *>(&expr))
        {
            if (const auto *var = dynamic_cast<const ast::VariableExpr *>(indexExpr->object.get()))
            {
                for (auto it = localTypeScopes_.rbegin(); it != localTypeScopes_.rend(); ++it)
                {
                    if (it->contains(var->name + "_size"))
                    {
                        auto *baseAlloca = lookupLocal(var->name + "_data", expr.location);
                        auto *base = builder_->CreateLoad(llvm::PointerType::get(context_, 0), baseAlloca, var->name + ".data.tagbase");
                        llvm::Value *index = emitExpr(*indexExpr->index);
                        index = builder_->CreateIntCast(index, llvm::Type::getInt64Ty(context_), false);
                        auto *tagIndex = builder_->CreateMul(index, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 2));
                        auto *ptr = builder_->CreateGEP(llvm::Type::getInt64Ty(context_), base, tagIndex);
                        return builder_->CreateLoad(llvm::Type::getInt64Ty(context_), ptr, var->name + ".tag");
                    }
                }
            }
        }
        if (const auto *var = dynamic_cast<const ast::VariableExpr *>(&expr))
        {
            for (auto it = localTypeScopes_.rbegin(); it != localTypeScopes_.rend(); ++it)
            {
                if (it->contains(var->name + "__tag"))
                {
                    auto *tagAlloca = lookupLocal(var->name + "__tag", expr.location);
                    return builder_->CreateLoad(llvm::Type::getInt64Ty(context_), tagAlloca, var->name + ".tag.load");
                }
            }
        }
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), variadicTagForType(expr.inferredType));
    }

    llvm::Value *CodeGenerator::adaptExternVarArgForPrintf(const ast::Expr &expr, llvm::Value *value, const std::string &format)
    {
        const char spec = firstPrintfSpecifier(format);
        if (expr.inferredType.kind != core::BuiltinTypeKind::Unknown)
        {
            if (spec == 'c' && expr.inferredType.kind == core::BuiltinTypeKind::Char)
                return builder_->CreateIntCast(value, llvm::Type::getInt32Ty(context_), false, "printf.char");
            return value;
        }
        if (spec == 's')
            return value->getType()->isPointerTy() ? value : builder_->CreateIntToPtr(value, llvm::PointerType::get(context_, 0), "printf.vararg.str");
        if (spec == 'f' || spec == 'g' || spec == 'e')
        {
            if (value->getType()->isDoubleTy())
                return value;
            if (value->getType()->isIntegerTy(64))
                return builder_->CreateBitCast(value, llvm::Type::getDoubleTy(context_), "printf.vararg.double");
            if (value->getType()->isIntegerTy())
                return builder_->CreateSIToFP(value, llvm::Type::getDoubleTy(context_), "printf.vararg.double");
        }
        if (spec == 'd' || spec == 'i')
            return builder_->CreateIntCast(value, llvm::Type::getInt32Ty(context_), true, "printf.vararg.int");
        if (spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o')
            return builder_->CreateIntCast(value, llvm::Type::getInt32Ty(context_), false, "printf.vararg.uint");
        if (spec == 'c')
            return builder_->CreateIntCast(value, llvm::Type::getInt32Ty(context_), false, "printf.vararg.char");
        if (spec == 'p')
            return builder_->CreateIntToPtr(value, llvm::PointerType::get(context_, 0), "printf.vararg.ptr");
        return value;
    }

    llvm::Value *CodeGenerator::castValue(llvm::Value *value, const core::TypeRef &from, const core::TypeRef &to)
    {
        if (from == to)
        {
            return value;
        }

        const bool fromPointerLike = from.isPointer() || from.isString();
        const bool toPointerLike = to.isPointer() || to.isString();

        if (to.kind == core::BuiltinTypeKind::Unknown)
        {
            if (value->getType()->isIntegerTy(64))
                return value;
            if (value->getType()->isIntegerTy())
                return builder_->CreateIntCast(value, llvm::Type::getInt64Ty(context_), false, "to.varitem");
            if (value->getType()->isPointerTy())
                return builder_->CreatePtrToInt(value, llvm::Type::getInt64Ty(context_), "to.varitem");
            if (value->getType()->isDoubleTy())
                return builder_->CreateBitCast(value, llvm::Type::getInt64Ty(context_), "to.varitem");
        }

        if (toPointerLike)
        {
            if (value->getType()->isPointerTy())
            {
                if (fromPointerLike && toPointerLike)
                    return builder_->CreatePointerCast(value, toLLVMType(to), "ptrlike.cast");
                return value;
            }
            if (value->getType()->isIntegerTy())
                return builder_->CreateIntToPtr(value, toLLVMType(to), to.isString() ? "inttostr" : "inttoptr");
        }

        if (from.kind == core::BuiltinTypeKind::Unknown)
        {
            if (to.isString())
                return builder_->CreateIntToPtr(value, toLLVMType(to), "varitem.tostr");
            if (to.isDouble())
                return builder_->CreateBitCast(value, toLLVMType(to), "varitem.todouble");
            if (to.isBool())
                return builder_->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "varitem.tobool");
            if (to.isInt() || to.isUInt() || to.isChar())
                return builder_->CreateIntCast(value, toLLVMType(to), to.isInt(), "varitem.toint");
            if (to.isPointer())
                return builder_->CreateIntToPtr(value, toLLVMType(to), "varitem.toptr");
        }

        if (fromPointerLike && (to.isInt() || to.isUInt() || to.isChar()))
        {
            return builder_->CreatePtrToInt(value, toLLVMType(to), from.isString() ? "strtoint" : "ptrtoint");
        }

        if ((from.isInt() || from.isUInt() || from.isChar()) && toPointerLike)
        {
            return builder_->CreateIntToPtr(value, toLLVMType(to), to.isString() ? "inttostr" : "inttoptr");
        }

        if (fromPointerLike && toPointerLike)
        {
            return builder_->CreatePointerCast(value, toLLVMType(to), "ptrcast");
        }

        if ((from.isInt() || from.isChar()) && to.isDouble())
        {
            return builder_->CreateSIToFP(value, toLLVMType(to), "sitofp");
        }

        if (from.isUInt() && to.isDouble())
        {
            return builder_->CreateUIToFP(value, toLLVMType(to), "uitofp");
        }

        if ((from.isInt() || from.isChar()) && to.isUInt())
        {
            return builder_->CreateIntCast(value, toLLVMType(to), false, "inttouint");
        }

        if ((from.isUInt() || from.isChar()) && to.isInt())
        {
            return builder_->CreateIntCast(value, toLLVMType(to), true, "uinttoint");
        }

        if ((from.isInt() || from.isUInt() || from.isChar()) && to.isBool())
        {
            return builder_->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "inttobool");
        }

        if (fromPointerLike && to.isBool())
        {
            return builder_->CreateICmpNE(value, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(value->getType())), "ptrtobool");
        }

        if (from.isBool() && (to.isInt() || to.isUInt() || to.isChar()))
        {
            return builder_->CreateIntCast(value, toLLVMType(to), false, "booltoint");
        }

        if (from.isBool() && to.isDouble())
        {
            return builder_->CreateUIToFP(value, toLLVMType(to), "booltodouble");
        }

        if ((from.isInt() || from.isUInt()) && to.isChar())
        {
            return builder_->CreateIntCast(value, toLLVMType(to), false, "tochar");
        }

        if (from.isDouble() && to.isInt())
        {
            return builder_->CreateFPToSI(value, toLLVMType(to), "fptosi");
        }

        if (from.isDouble() && to.isUInt())
        {
            return builder_->CreateFPToUI(value, toLLVMType(to), "fptoui");
        }

        if (from.isDouble() && to.isChar())
        {
            return builder_->CreateFPToUI(value, toLLVMType(to), "fptochar");
        }

        return value;
    }

    llvm::Type *CodeGenerator::toLLVMType(const core::TypeRef &type)
    {
        if (type.isPointer())
            return llvm::PointerType::get(context_, 0);
        switch (type.kind)
        {
        case core::BuiltinTypeKind::Void:
            return llvm::Type::getVoidTy(context_);
        case core::BuiltinTypeKind::Bool:
            return llvm::Type::getInt1Ty(context_);
        case core::BuiltinTypeKind::Int:
            return llvm::Type::getInt64Ty(context_);
        case core::BuiltinTypeKind::UInt:
            return llvm::Type::getInt64Ty(context_);
        case core::BuiltinTypeKind::Double:
            return llvm::Type::getDoubleTy(context_);
        case core::BuiltinTypeKind::Char:
            return llvm::Type::getInt8Ty(context_);
        case core::BuiltinTypeKind::String:
            return llvm::PointerType::get(context_, 0);
        case core::BuiltinTypeKind::Class:
        {
            const auto it = classes_.find(type.name);
            if (it == classes_.end())
                core::throwDiagnostic(core::SourceLocation{}, "cannot lower unknown class to LLVM: " + type.name);
            return it->second.type;
        }
        case core::BuiltinTypeKind::Array:
            return arrayType_;
        case core::BuiltinTypeKind::Unknown:
            return llvm::Type::getInt64Ty(context_);
        }
        core::throwDiagnostic(core::SourceLocation{}, "cannot lower unknown type to LLVM");
    }

    llvm::AllocaInst *CodeGenerator::createEntryAlloca(llvm::Function *function, llvm::Type *type, const std::string &name)
    {
        llvm::IRBuilder<> entryBuilder(&function->getEntryBlock(), function->getEntryBlock().begin());
        return entryBuilder.CreateAlloca(type, nullptr, name);
    }

    llvm::AllocaInst *CodeGenerator::lookupLocal(const std::string &name, const core::SourceLocation &location) const
    {
        for (auto it = localScopes_.rbegin(); it != localScopes_.rend(); ++it)
        {
            if (const auto found = it->find(name); found != it->end())
            {
                return found->second;
            }
        }
        core::throwDiagnostic(location, "unknown variable: " + name);
    }

    core::TypeRef CodeGenerator::lookupLocalType(const std::string &name, const core::SourceLocation &location) const
    {
        for (auto it = localTypeScopes_.rbegin(); it != localTypeScopes_.rend(); ++it)
        {
            if (const auto found = it->find(name); found != it->end())
            {
                return found->second;
            }
        }
        core::throwDiagnostic(location, "unknown variable: " + name);
    }

    void CodeGenerator::declareLocal(const std::string &name, llvm::AllocaInst *alloca, const core::TypeRef &type)
    {
        localScopes_.back()[name] = alloca;
        localTypeScopes_.back()[name] = type;
    }

    void CodeGenerator::pushLocalScope()
    {
        localScopes_.emplace_back();
        localTypeScopes_.emplace_back();
    }

    void CodeGenerator::popLocalScope()
    {
        localScopes_.pop_back();
        localTypeScopes_.pop_back();
    }


    llvm::Value *CodeGenerator::emitInterpolatedStringValue(const ast::InterpolatedStringExpr &expr)
    {
        auto *i8Type = llvm::Type::getInt8Ty(context_);
        auto *i64Type = llvm::Type::getInt64Ty(context_);
        auto *charPtrType = llvm::PointerType::get(context_, 0);
        auto *nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(charPtrType));
        auto *zero64 = llvm::ConstantInt::get(i64Type, 0);
        auto *one64 = llvm::ConstantInt::get(i64Type, 1);

        auto makeSafeString = [&](llvm::Value *value) -> llvm::Value *
        {
            if (value->getType()->isIntegerTy())
                value = builder_->CreateIntToPtr(value, charPtrType, "istr.str.ptr");
            else if (value->getType()->isPointerTy() && value->getType() != charPtrType)
                value = builder_->CreatePointerCast(value, charPtrType, "istr.str.cast");
            auto *empty = builder_->CreateGlobalStringPtr("");
            auto *isNull = builder_->CreateICmpEQ(value, nullPtr, "istr.str.null");
            return builder_->CreateSelect(isNull, empty, value, "istr.str.safe");
        };

        auto appendBytes = [&](llvm::Value *buffer, llvm::Value *offset, llvm::Value *source, llvm::Value *length) -> llvm::Value *
        {
            if (source->getType()->isIntegerTy())
                source = builder_->CreateIntToPtr(source, charPtrType, "istr.bytes.ptr");
            else if (source->getType()->isPointerTy() && source->getType() != charPtrType)
                source = builder_->CreatePointerCast(source, charPtrType, "istr.bytes.cast");
            auto *dest = builder_->CreateGEP(i8Type, buffer, offset, "istr.dest");
            builder_->CreateCall(getOrCreateMemmove(), {dest, source, length});
            return builder_->CreateAdd(offset, length, "istr.offset.next");
        };

        auto appendFormattedLength = [&](llvm::Value *format, std::vector<llvm::Value *> args) -> llvm::Value *
        {
            std::vector<llvm::Value *> callArgs;
            callArgs.push_back(nullPtr);
            callArgs.push_back(zero64);
            callArgs.push_back(format);
            callArgs.insert(callArgs.end(), args.begin(), args.end());
            auto *written = builder_->CreateCall(getOrCreateSnprintf(), callArgs, "istr.len");
            return builder_->CreateSExtOrTrunc(written, i64Type, "istr.len64");
        };

        auto appendFormattedWrite = [&](llvm::Value *buffer, llvm::Value *offset, llvm::Value *capacity, llvm::Value *format, std::vector<llvm::Value *> args) -> llvm::Value *
        {
            auto *dest = builder_->CreateGEP(i8Type, buffer, offset, "istr.dest");
            auto *remaining = builder_->CreateSub(capacity, offset, "istr.remaining");
            std::vector<llvm::Value *> callArgs;
            callArgs.push_back(dest);
            callArgs.push_back(remaining);
            callArgs.push_back(format);
            callArgs.insert(callArgs.end(), args.begin(), args.end());
            auto *written = builder_->CreateCall(getOrCreateSnprintf(), callArgs, "istr.write");
            return builder_->CreateAdd(offset, builder_->CreateSExtOrTrunc(written, i64Type, "istr.write64"), "istr.offset.next");
        };

        auto lengthForSigned = [&](llvm::Value *value) -> llvm::Value *
        {
            auto *isNegative = builder_->CreateICmpSLT(value, zero64, "istr.int.neg");
            auto *magnitude = builder_->CreateSelect(isNegative,
                                                     builder_->CreateSub(zero64, value, "istr.int.abs"),
                                                     value,
                                                     "istr.int.mag");
            auto *digits = builder_->CreateCall(getOrCreateDashInterpUIntLen(), {magnitude}, "istr.int.digits");
            auto *sign = builder_->CreateZExt(isNegative, i64Type, "istr.int.sign");
            return builder_->CreateAdd(digits, sign, "istr.int.len");
        };

        auto writeSigned = [&](llvm::Value *buffer, llvm::Value *offset, llvm::Value *value) -> llvm::Value *
        {
            auto *dest = builder_->CreateGEP(i8Type, buffer, offset, "istr.dest");
            auto *written = builder_->CreateCall(getOrCreateDashInterpWriteInt(), {dest, value}, "istr.int.write");
            return builder_->CreateAdd(offset, written, "istr.offset.next");
        };

        auto writeUnsigned = [&](llvm::Value *buffer, llvm::Value *offset, llvm::Value *value) -> llvm::Value *
        {
            auto *dest = builder_->CreateGEP(i8Type, buffer, offset, "istr.dest");
            auto *written = builder_->CreateCall(getOrCreateDashInterpWriteUInt(), {dest, value}, "istr.uint.write");
            return builder_->CreateAdd(offset, written, "istr.offset.next");
        };

        auto lengthForValue = [&](const ast::Expr &embedded, llvm::Value *value) -> llvm::Value *
        {
            const auto &type = embedded.inferredType;
            if (type.isString())
                return builder_->CreateCall(getOrCreateStrlen(), {makeSafeString(value)}, "istr.str.len");
            if (type.isBool())
            {
                auto *trueLen = llvm::ConstantInt::get(i64Type, 4);
                auto *falseLen = llvm::ConstantInt::get(i64Type, 5);
                return builder_->CreateSelect(value, trueLen, falseLen, "istr.bool.len");
            }
            if (type.isChar())
                return one64;
            if (type.isDouble())
                return appendFormattedLength(builder_->CreateGlobalStringPtr("%f"), {value});
            if (type.isPointer())
            {
                if (value->getType()->isIntegerTy())
                    value = builder_->CreateIntToPtr(value, charPtrType, "istr.ptr.cast");
                else if (value->getType()->isPointerTy() && value->getType() != charPtrType)
                    value = builder_->CreatePointerCast(value, charPtrType, "istr.ptr.cast");
                return appendFormattedLength(builder_->CreateGlobalStringPtr("%p"), {value});
            }
            if (type.isUInt() || type.kind == core::BuiltinTypeKind::Unknown)
            {
                auto *casted = castValue(value, type, core::TypeRef{core::BuiltinTypeKind::UInt, ""});
                return builder_->CreateCall(getOrCreateDashInterpUIntLen(), {casted}, "istr.uint.len");
            }
            auto *casted = castValue(value, type, core::TypeRef{core::BuiltinTypeKind::Int, ""});
            return lengthForSigned(casted);
        };

        auto writeValue = [&](const ast::Expr &embedded, llvm::Value *value, llvm::Value *buffer, llvm::Value *offset, llvm::Value *capacity) -> llvm::Value *
        {
            const auto &type = embedded.inferredType;
            if (type.isString())
            {
                auto *safe = makeSafeString(value);
                auto *length = builder_->CreateCall(getOrCreateStrlen(), {safe}, "istr.str.len");
                return appendBytes(buffer, offset, safe, length);
            }
            if (type.isBool())
            {
                auto *trueStr = builder_->CreateGlobalStringPtr("true");
                auto *falseStr = builder_->CreateGlobalStringPtr("false");
                auto *selected = builder_->CreateSelect(value, trueStr, falseStr, "istr.bool.str");
                auto *length = builder_->CreateSelect(value,
                                                      llvm::ConstantInt::get(i64Type, 4),
                                                      llvm::ConstantInt::get(i64Type, 5),
                                                      "istr.bool.len");
                return appendBytes(buffer, offset, selected, length);
            }
            if (type.isChar())
            {
                auto *dest = builder_->CreateGEP(i8Type, buffer, offset, "istr.dest");
                auto *casted = builder_->CreateIntCast(value, i8Type, false, "istr.char.cast");
                builder_->CreateStore(casted, dest);
                return builder_->CreateAdd(offset, one64, "istr.offset.next");
            }
            if (type.isDouble())
                return appendFormattedWrite(buffer, offset, capacity, builder_->CreateGlobalStringPtr("%f"), {value});
            if (type.isPointer())
            {
                if (value->getType()->isIntegerTy())
                    value = builder_->CreateIntToPtr(value, charPtrType, "istr.ptr.cast");
                else if (value->getType()->isPointerTy() && value->getType() != charPtrType)
                    value = builder_->CreatePointerCast(value, charPtrType, "istr.ptr.cast");
                return appendFormattedWrite(buffer, offset, capacity, builder_->CreateGlobalStringPtr("%p"), {value});
            }
            if (type.isUInt() || type.kind == core::BuiltinTypeKind::Unknown)
            {
                auto *casted = castValue(value, type, core::TypeRef{core::BuiltinTypeKind::UInt, ""});
                return writeUnsigned(buffer, offset, casted);
            }
            auto *casted = castValue(value, type, core::TypeRef{core::BuiltinTypeKind::Int, ""});
            return writeSigned(buffer, offset, casted);
        };

        std::vector<llvm::Value *> embeddedValues;
        embeddedValues.reserve(expr.expressions.size());
        for (const auto &embedded : expr.expressions)
            embeddedValues.push_back(emitExpr(*embedded));

        llvm::Value *totalLength = zero64;
        for (std::size_t i = 0; i < expr.textSegments.size(); ++i)
        {
            const auto literalText = escapeString(expr.textSegments[i]);
            totalLength = builder_->CreateAdd(totalLength, llvm::ConstantInt::get(i64Type, literalText.size()), "istr.total.literal");
            if (i < expr.expressions.size())
                totalLength = builder_->CreateAdd(totalLength, lengthForValue(*expr.expressions[i], embeddedValues[i]), "istr.total.value");
        }

        auto *capacity = builder_->CreateAdd(totalLength, one64, "istr.capacity");
        auto *buffer = builder_->CreateCall(getOrCreateMalloc(), {capacity}, "istr.buffer");
        llvm::Value *offset = zero64;

        for (std::size_t i = 0; i < expr.textSegments.size(); ++i)
        {
            const auto literalText = escapeString(expr.textSegments[i]);
            if (!literalText.empty())
            {
                auto *literalValue = builder_->CreateGlobalStringPtr(literalText);
                offset = appendBytes(buffer, offset, literalValue, llvm::ConstantInt::get(i64Type, literalText.size()));
            }
            if (i < expr.expressions.size())
                offset = writeValue(*expr.expressions[i], embeddedValues[i], buffer, offset, capacity);
        }

        auto *terminatorPtr = builder_->CreateGEP(i8Type, buffer, offset, "istr.term");
        builder_->CreateStore(llvm::ConstantInt::get(i8Type, 0), terminatorPtr);
        return buffer;
    }

    llvm::Value *CodeGenerator::emitGroupLiteralValue(const ast::ArrayLiteralExpr &expr, const core::TypeRef &targetType)
    {
        const auto &layout = classes_.at(targetType.name);
        llvm::Value *result = llvm::UndefValue::get(layout.type);
        for (std::size_t i = 0; i < layout.fieldOrder.size(); ++i)
        {
            const auto &fieldName = layout.fieldOrder[i];
            const auto &fieldType = layout.fieldTypes.at(fieldName);
            auto *value = emitExpr(*expr.elements[i]);
            value = castValue(value, expr.elements[i]->inferredType, fieldType);
            result = builder_->CreateInsertValue(result, value, {static_cast<unsigned>(i)});
        }
        return result;
    }

    llvm::Value *CodeGenerator::emitArrayLiteralValue(const ast::ArrayLiteralExpr &expr, const core::TypeRef &targetType)
    {
        const auto elementType = makeArrayElementType(targetType);
        auto *elementLLVMType = toLLVMType(elementType);
        const auto elementSize = static_cast<std::uint64_t>(module_->getDataLayout().getTypeAllocSize(elementLLVMType));
        const auto count = static_cast<std::uint64_t>(expr.elements.size());
        llvm::Value *dataPointer = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(llvm::PointerType::get(context_, 0)));
        if (count > 0)
        {
            auto *bytes = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elementSize * count);
            dataPointer = builder_->CreateCall(getOrCreateMalloc(), {bytes}, "array.data");
            auto *typedPointer = builder_->CreatePointerCast(dataPointer, llvm::PointerType::getUnqual(elementLLVMType));
            for (std::size_t i = 0; i < expr.elements.size(); ++i)
            {
                auto *elementPtr = builder_->CreateGEP(elementLLVMType, typedPointer, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), i));
                auto *value = emitExpr(*expr.elements[i]);
                value = castValue(value, expr.elements[i]->inferredType, elementType);
                builder_->CreateStore(value, elementPtr);
            }
        }
        llvm::Value *result = llvm::UndefValue::get(arrayType_);
        result = builder_->CreateInsertValue(result, dataPointer, {0});
        result = builder_->CreateInsertValue(result, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), count), {1});
        result = builder_->CreateInsertValue(result, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), targetType.hasArraySize ? targetType.arraySize : count), {2});
        result = builder_->CreateInsertValue(result, llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), 1), {3});
        return result;
    }

    llvm::Value *CodeGenerator::emitDefaultClassValue(const core::TypeRef &targetType)
    {
        const auto &layout = classes_.at(targetType.name);
        llvm::Value *result = llvm::UndefValue::get(layout.type);
        for (std::size_t i = 0; i < layout.fieldOrder.size(); ++i)
        {
            const auto &fieldName = layout.fieldOrder[i];
            const auto &fieldType = layout.fieldTypes.at(fieldName);
            const ast::Expr *initializer = nullptr;
            if (const auto it = layout.fieldInitializers.find(fieldName); it != layout.fieldInitializers.end())
                initializer = it->second;
            auto *value = emitValueOrDefault(fieldType, initializer);
            result = builder_->CreateInsertValue(result, value, {static_cast<unsigned>(i)});
        }
        return result;
    }

    llvm::Value *CodeGenerator::emitValueOrDefault(const core::TypeRef &targetType, const ast::Expr *initializer)
    {
        if (initializer != nullptr)
        {
            if (targetType.isArray())
            {
                if (const auto *arrayLiteral = dynamic_cast<const ast::ArrayLiteralExpr *>(initializer))
                    return emitArrayLiteralValue(*arrayLiteral, targetType);
            }
            if (targetType.kind == core::BuiltinTypeKind::Class)
            {
                if (const auto *groupLiteral = dynamic_cast<const ast::ArrayLiteralExpr *>(initializer))
                {
                    const auto &klass = classes_.at(targetType.name);
                    if (klass.isGroup)
                        return emitGroupLiteralValue(*groupLiteral, targetType);
                }
            }
            auto *value = emitExpr(*initializer);
            return castValue(value, initializer->inferredType, targetType);
        }

        if (targetType.kind == core::BuiltinTypeKind::Class)
            return emitDefaultClassValue(targetType);
        return llvm::Constant::getNullValue(toLLVMType(targetType));
    }

    llvm::Constant *CodeGenerator::emitConstantDefaultValue(const core::TypeRef &targetType, const ast::Expr *initializer, const std::string &name)
    {
        if (initializer != nullptr)
        {
            if (targetType.isArray())
            {
                const auto *array = dynamic_cast<const ast::ArrayLiteralExpr *>(initializer);
                if (array == nullptr)
                    return nullptr;
                const auto elementType = makeArrayElementType(targetType);
                auto *elementLLVMType = toLLVMType(elementType);
                std::vector<llvm::Constant *> elementConstants;
                elementConstants.reserve(array->elements.size());
                for (std::size_t i = 0; i < array->elements.size(); ++i)
                {
                    auto *elementConstant = makeArrayConstantElement(context_, *module_, elementLLVMType, *array->elements[i], name + ".arr." + std::to_string(i));
                    if (elementConstant == nullptr)
                        return nullptr;
                    elementConstants.push_back(elementConstant);
                }
                auto *dataPtrType = llvm::PointerType::get(context_, 0);
                auto *i64Type = llvm::Type::getInt64Ty(context_);
                auto *boolType = llvm::Type::getInt1Ty(context_);
                auto *nullData = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(dataPtrType));
                llvm::Constant *dataPointer = nullData;
                if (!elementConstants.empty())
                {
                    auto *arrTy = llvm::ArrayType::get(elementLLVMType, elementConstants.size());
                    auto *dataInit = llvm::ConstantArray::get(arrTy, elementConstants);
                    auto *dataGlobal = new llvm::GlobalVariable(*module_, arrTy, true, llvm::GlobalValue::PrivateLinkage, dataInit, name + ".data");
                    auto *zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0);
                    auto *typedPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(arrTy, dataGlobal, llvm::ArrayRef<llvm::Constant *>{zero, zero});
                    dataPointer = llvm::ConstantExpr::getPointerCast(typedPtr, dataPtrType);
                }
                const auto sizeValue = static_cast<std::uint64_t>(array->elements.size());
                return llvm::ConstantStruct::get(arrayType_, dataPointer,
                    llvm::ConstantInt::get(i64Type, sizeValue),
                    llvm::ConstantInt::get(i64Type, targetType.hasArraySize ? targetType.arraySize : sizeValue),
                    llvm::ConstantInt::get(boolType, 0));
            }
            if (targetType.kind == core::BuiltinTypeKind::Class)
            {
                if (const auto *groupLiteral = dynamic_cast<const ast::ArrayLiteralExpr *>(initializer))
                {
                    const auto &klass = classes_.at(targetType.name);
                    std::vector<llvm::Constant *> fieldConstants;
                    fieldConstants.reserve(klass.fieldOrder.size());
                    for (std::size_t i = 0; i < klass.fieldOrder.size(); ++i)
                    {
                        const auto &fieldName = klass.fieldOrder[i];
                        auto *fieldConstant = emitConstantDefaultValue(klass.fieldTypes.at(fieldName), groupLiteral->elements[i].get(), name + ".grp." + std::to_string(i));
                        if (fieldConstant == nullptr)
                            return nullptr;
                        fieldConstants.push_back(fieldConstant);
                    }
                    return llvm::ConstantStruct::get(klass.type, fieldConstants);
                }
                return nullptr;
            }
            if (const auto *literal = dynamic_cast<const ast::IntegerLiteralExpr *>(initializer))
                return llvm::ConstantInt::get(toLLVMType(targetType), literal->value, !literal->forceUnsigned);
            if (const auto *literal = dynamic_cast<const ast::DoubleLiteralExpr *>(initializer))
                return llvm::ConstantFP::get(toLLVMType(targetType), literal->value);
            if (const auto *literal = dynamic_cast<const ast::BoolLiteralExpr *>(initializer))
                return llvm::ConstantInt::get(toLLVMType(targetType), literal->value ? 1 : 0, false);
            if (const auto *literal = dynamic_cast<const ast::CharLiteralExpr *>(initializer))
                return llvm::ConstantInt::get(toLLVMType(targetType), literal->value, false);
            if (const auto *literal = dynamic_cast<const ast::StringLiteralExpr *>(initializer))
                return makeArrayConstantElement(context_, *module_, toLLVMType(targetType), *literal, name + ".str");
            return nullptr;
        }

        if (targetType.kind == core::BuiltinTypeKind::Class)
        {
            const auto &layout = classes_.at(targetType.name);
            std::vector<llvm::Constant *> fieldConstants;
            fieldConstants.reserve(layout.fieldOrder.size());
            for (std::size_t i = 0; i < layout.fieldOrder.size(); ++i)
            {
                const auto &fieldName = layout.fieldOrder[i];
                const ast::Expr *fieldInit = nullptr;
                if (const auto it = layout.fieldInitializers.find(fieldName); it != layout.fieldInitializers.end())
                    fieldInit = it->second;
                auto *fieldConstant = emitConstantDefaultValue(layout.fieldTypes.at(fieldName), fieldInit, name + "." + fieldName);
                if (fieldConstant == nullptr)
                    fieldConstant = llvm::Constant::getNullValue(toLLVMType(layout.fieldTypes.at(fieldName)));
                fieldConstants.push_back(fieldConstant);
            }
            return llvm::ConstantStruct::get(layout.type, fieldConstants);
        }
        return llvm::Constant::getNullValue(toLLVMType(targetType));
    }

    llvm::Value *CodeGenerator::emitArrayDataPointer(llvm::Value *arrayAddress, const core::TypeRef &arrayType)
    {
        auto *dataPtr = builder_->CreateStructGEP(toLLVMType(arrayType), arrayAddress, 0, "array.data.ptr");
        return builder_->CreateLoad(llvm::PointerType::get(context_, 0), dataPtr, "array.data");
    }

    llvm::Value *CodeGenerator::emitArraySizeValue(llvm::Value *arrayAddress)
    {
        auto *sizePtr = builder_->CreateStructGEP(arrayType_, arrayAddress, 1, "array.size.ptr");
        return builder_->CreateLoad(llvm::Type::getInt64Ty(context_), sizePtr, "array.size");
    }

    llvm::Value *CodeGenerator::emitArrayCapacityValue(llvm::Value *arrayAddress)
    {
        auto *capacityPtr = builder_->CreateStructGEP(arrayType_, arrayAddress, 2, "array.cap.ptr");
        return builder_->CreateLoad(llvm::Type::getInt64Ty(context_), capacityPtr, "array.cap");
    }

    llvm::Value *CodeGenerator::emitArrayOwnedValue(llvm::Value *arrayAddress)
    {
        auto *ownedPtr = builder_->CreateStructGEP(arrayType_, arrayAddress, 3, "array.owned.ptr");
        return builder_->CreateLoad(llvm::Type::getInt1Ty(context_), ownedPtr, "array.owned");
    }

    void CodeGenerator::storeArraySizeValue(llvm::Value *arrayAddress, llvm::Value *value)
    {
        auto *sizePtr = builder_->CreateStructGEP(arrayType_, arrayAddress, 1);
        builder_->CreateStore(value, sizePtr);
    }

    void CodeGenerator::storeArrayCapacityValue(llvm::Value *arrayAddress, llvm::Value *value)
    {
        auto *capacityPtr = builder_->CreateStructGEP(arrayType_, arrayAddress, 2);
        builder_->CreateStore(value, capacityPtr);
    }

    void CodeGenerator::storeArrayOwnedValue(llvm::Value *arrayAddress, llvm::Value *value)
    {
        auto *ownedPtr = builder_->CreateStructGEP(arrayType_, arrayAddress, 3);
        builder_->CreateStore(value, ownedPtr);
    }

    void CodeGenerator::storeArrayDataValue(llvm::Value *arrayAddress, llvm::Value *value)
    {
        auto *dataPtr = builder_->CreateStructGEP(arrayType_, arrayAddress, 0);
        builder_->CreateStore(value, dataPtr);
    }

    llvm::Value *CodeGenerator::ensureMutableArrayStorage(llvm::Value *arrayAddress, const core::TypeRef &arrayType, llvm::Value *minCapacity)
    {
        auto *currentData = emitArrayDataPointer(arrayAddress, arrayType);
        auto *currentSize = emitArraySizeValue(arrayAddress);
        auto *currentCapacity = emitArrayCapacityValue(arrayAddress);
        auto *currentOwned = emitArrayOwnedValue(arrayAddress);
        auto *hasEnough = builder_->CreateICmpUGE(currentCapacity, minCapacity);
        auto *ownedEnough = builder_->CreateAnd(currentOwned, hasEnough);
        auto *growBlock = llvm::BasicBlock::Create(context_, "array.ensure.grow", currentFunction_);
        auto *doneBlock = llvm::BasicBlock::Create(context_, "array.ensure.done", currentFunction_);
        builder_->CreateCondBr(ownedEnough, doneBlock, growBlock);
        builder_->SetInsertPoint(growBlock);
        auto *doubleCapacity = builder_->CreateMul(currentCapacity, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 2));
        auto *withOne = builder_->CreateSelect(builder_->CreateICmpEQ(doubleCapacity, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0)), llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1), doubleCapacity);
        auto *newCapacity = builder_->CreateSelect(builder_->CreateICmpUGE(withOne, minCapacity), withOne, minCapacity);
        auto elementType = makeArrayElementType(arrayType);
        auto *elementLLVMType = toLLVMType(elementType);
        auto elementSize = static_cast<std::uint64_t>(module_->getDataLayout().getTypeAllocSize(elementLLVMType));
        auto *bytes = builder_->CreateMul(newCapacity, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elementSize));
        auto *ownedGrowBlock = llvm::BasicBlock::Create(context_, "array.ensure.realloc", currentFunction_);
        auto *copyGrowBlock = llvm::BasicBlock::Create(context_, "array.ensure.copy", currentFunction_);
        builder_->CreateCondBr(currentOwned, ownedGrowBlock, copyGrowBlock);
        builder_->SetInsertPoint(ownedGrowBlock);
        auto *realloced = builder_->CreateCall(getOrCreateRealloc(), {currentData, bytes}, "array.realloc");
        storeArrayDataValue(arrayAddress, realloced);
        storeArrayCapacityValue(arrayAddress, newCapacity);
        storeArrayOwnedValue(arrayAddress, llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), 1));
        builder_->CreateBr(doneBlock);
        builder_->SetInsertPoint(copyGrowBlock);
        auto *malloced = builder_->CreateCall(getOrCreateMalloc(), {bytes}, "array.copy");
        auto *shouldCopy = builder_->CreateICmpUGT(currentSize, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0));
        auto *copyBlock = llvm::BasicBlock::Create(context_, "array.ensure.do_copy", currentFunction_);
        auto *copyDoneBlock = llvm::BasicBlock::Create(context_, "array.ensure.copy_done", currentFunction_);
        builder_->CreateCondBr(shouldCopy, copyBlock, copyDoneBlock);
        builder_->SetInsertPoint(copyBlock);
        auto *copyBytes = builder_->CreateMul(currentSize, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elementSize));
        builder_->CreateCall(getOrCreateMemmove(), {malloced, currentData, copyBytes});
        builder_->CreateBr(copyDoneBlock);
        builder_->SetInsertPoint(copyDoneBlock);
        storeArrayDataValue(arrayAddress, malloced);
        storeArrayCapacityValue(arrayAddress, newCapacity);
        storeArrayOwnedValue(arrayAddress, llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), 1));
        builder_->CreateBr(doneBlock);
        builder_->SetInsertPoint(doneBlock);
        return emitArrayDataPointer(arrayAddress, arrayType);
    }

    llvm::Function *CodeGenerator::getOrCreateMalloc()
    {
        if (mallocFunction_ != nullptr)
            return mallocFunction_;
        auto *type = llvm::FunctionType::get(llvm::PointerType::get(context_, 0), {llvm::Type::getInt64Ty(context_)}, false);
        mallocFunction_ = llvm::cast<llvm::Function>(module_->getOrInsertFunction("malloc", type).getCallee());
        return mallocFunction_;
    }

    llvm::Function *CodeGenerator::getOrCreateFree()
    {
        if (freeFunction_ != nullptr)
            return freeFunction_;
        auto *type = llvm::FunctionType::get(llvm::Type::getVoidTy(context_), {llvm::PointerType::get(context_, 0)}, false);
        freeFunction_ = llvm::cast<llvm::Function>(module_->getOrInsertFunction("free", type).getCallee());
        return freeFunction_;
    }

    llvm::Function *CodeGenerator::getOrCreateRealloc()
    {
        if (reallocFunction_ != nullptr)
            return reallocFunction_;
        auto *type = llvm::FunctionType::get(llvm::PointerType::get(context_, 0), {llvm::PointerType::get(context_, 0), llvm::Type::getInt64Ty(context_)}, false);
        reallocFunction_ = llvm::cast<llvm::Function>(module_->getOrInsertFunction("realloc", type).getCallee());
        return reallocFunction_;
    }

    llvm::Function *CodeGenerator::getOrCreateMemmove()
    {
        if (memmoveFunction_ != nullptr)
            return memmoveFunction_;
        auto *type = llvm::FunctionType::get(llvm::PointerType::get(context_, 0), {llvm::PointerType::get(context_, 0), llvm::PointerType::get(context_, 0), llvm::Type::getInt64Ty(context_)}, false);
        memmoveFunction_ = llvm::cast<llvm::Function>(module_->getOrInsertFunction("memmove", type).getCallee());
        return memmoveFunction_;
    }


    llvm::Function *CodeGenerator::getOrCreateStrlen()
    {
        if (strlenFunction_ != nullptr)
            return strlenFunction_;
        auto *type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context_), {llvm::PointerType::get(context_, 0)}, false);
        strlenFunction_ = llvm::cast<llvm::Function>(module_->getOrInsertFunction("strlen", type).getCallee());
        return strlenFunction_;
    }

    llvm::Function *CodeGenerator::getOrCreateDashInterpUIntLen()
    {
        if (dashInterpUIntLenFunction_ != nullptr)
            return dashInterpUIntLenFunction_;

        auto *i64Type = llvm::Type::getInt64Ty(context_);
        auto *type = llvm::FunctionType::get(i64Type, {i64Type}, false);
        auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "__dash_interp_uint_len", module_.get());
        dashInterpUIntLenFunction_ = function;

        auto argIt = function->arg_begin();
        llvm::Value *value = argIt++;
        value->setName("value");

        auto *entry = llvm::BasicBlock::Create(context_, "entry", function);
        auto *loop = llvm::BasicBlock::Create(context_, "loop", function);
        auto *done = llvm::BasicBlock::Create(context_, "done", function);

        llvm::IRBuilder<> builder(entry);
        auto *zero = llvm::ConstantInt::get(i64Type, 0);
        auto *one = llvm::ConstantInt::get(i64Type, 1);
        auto *ten = llvm::ConstantInt::get(i64Type, 10);

        auto *isZero = builder.CreateICmpEQ(value, zero, "is.zero");
        builder.CreateCondBr(isZero, done, loop);

        builder.SetInsertPoint(loop);
        auto *valuePhi = builder.CreatePHI(i64Type, 2, "value.phi");
        auto *lenPhi = builder.CreatePHI(i64Type, 2, "len.phi");
        valuePhi->addIncoming(value, entry);
        lenPhi->addIncoming(zero, entry);
        auto *nextLen = builder.CreateAdd(lenPhi, one, "len.next");
        auto *nextValue = builder.CreateUDiv(valuePhi, ten, "value.next");
        auto *isDone = builder.CreateICmpEQ(nextValue, zero, "loop.done");
        builder.CreateCondBr(isDone, done, loop);
        valuePhi->addIncoming(nextValue, loop);
        lenPhi->addIncoming(nextLen, loop);

        builder.SetInsertPoint(done);
        auto *result = builder.CreatePHI(i64Type, 2, "result");
        result->addIncoming(one, entry);
        result->addIncoming(nextLen, loop);
        builder.CreateRet(result);

        return dashInterpUIntLenFunction_;
    }

    llvm::Function *CodeGenerator::getOrCreateDashInterpWriteUInt()
    {
        if (dashInterpWriteUIntFunction_ != nullptr)
            return dashInterpWriteUIntFunction_;

        auto *i8Type = llvm::Type::getInt8Ty(context_);
        auto *i64Type = llvm::Type::getInt64Ty(context_);
        auto *charPtrType = llvm::PointerType::get(context_, 0);
        auto *type = llvm::FunctionType::get(i64Type, {charPtrType, i64Type}, false);
        auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "__dash_interp_write_uint", module_.get());
        dashInterpWriteUIntFunction_ = function;

        auto argIt = function->arg_begin();
        llvm::Value *dest = argIt++;
        dest->setName("dest");
        llvm::Value *value = argIt++;
        value->setName("value");

        auto *entry = llvm::BasicBlock::Create(context_, "entry", function);
        auto *zeroBlock = llvm::BasicBlock::Create(context_, "zero", function);
        auto *nonZeroBlock = llvm::BasicBlock::Create(context_, "nonzero", function);
        auto *loop = llvm::BasicBlock::Create(context_, "loop", function);
        auto *done = llvm::BasicBlock::Create(context_, "done", function);

        llvm::IRBuilder<> builder(entry);
        auto *zero = llvm::ConstantInt::get(i64Type, 0);
        auto *one = llvm::ConstantInt::get(i64Type, 1);
        auto *ten = llvm::ConstantInt::get(i64Type, 10);
        auto *asciiZero = llvm::ConstantInt::get(i8Type, '0');

        auto *isZero = builder.CreateICmpEQ(value, zero, "is.zero");
        builder.CreateCondBr(isZero, zeroBlock, nonZeroBlock);

        builder.SetInsertPoint(zeroBlock);
        auto *zeroPtr = builder.CreateGEP(i8Type, dest, zero, "zero.ptr");
        builder.CreateStore(asciiZero, zeroPtr);
        builder.CreateRet(one);

        builder.SetInsertPoint(nonZeroBlock);
        auto *len = builder.CreateCall(getOrCreateDashInterpUIntLen(), {value}, "len");
        builder.CreateBr(loop);

        builder.SetInsertPoint(loop);
        auto *valuePhi = builder.CreatePHI(i64Type, 2, "value.phi");
        auto *indexPhi = builder.CreatePHI(i64Type, 2, "index.phi");
        valuePhi->addIncoming(value, nonZeroBlock);
        indexPhi->addIncoming(len, nonZeroBlock);
        auto *digit = builder.CreateURem(valuePhi, ten, "digit");
        auto *digitChar = builder.CreateAdd(builder.CreateTrunc(digit, i8Type), asciiZero, "digit.char");
        auto *nextIndex = builder.CreateSub(indexPhi, one, "index.next");
        auto *digitPtr = builder.CreateGEP(i8Type, dest, nextIndex, "digit.ptr");
        builder.CreateStore(digitChar, digitPtr);
        auto *nextValue = builder.CreateUDiv(valuePhi, ten, "value.next");
        auto *isDone = builder.CreateICmpEQ(nextValue, zero, "loop.done");
        builder.CreateCondBr(isDone, done, loop);
        valuePhi->addIncoming(nextValue, loop);
        indexPhi->addIncoming(nextIndex, loop);

        builder.SetInsertPoint(done);
        builder.CreateRet(len);

        return dashInterpWriteUIntFunction_;
    }

    llvm::Function *CodeGenerator::getOrCreateDashInterpWriteInt()
    {
        if (dashInterpWriteIntFunction_ != nullptr)
            return dashInterpWriteIntFunction_;

        auto *i8Type = llvm::Type::getInt8Ty(context_);
        auto *i64Type = llvm::Type::getInt64Ty(context_);
        auto *charPtrType = llvm::PointerType::get(context_, 0);
        auto *type = llvm::FunctionType::get(i64Type, {charPtrType, i64Type}, false);
        auto *function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, "__dash_interp_write_int", module_.get());
        dashInterpWriteIntFunction_ = function;

        auto argIt = function->arg_begin();
        llvm::Value *dest = argIt++;
        dest->setName("dest");
        llvm::Value *value = argIt++;
        value->setName("value");

        auto *entry = llvm::BasicBlock::Create(context_, "entry", function);
        auto *negativeBlock = llvm::BasicBlock::Create(context_, "negative", function);
        auto *done = llvm::BasicBlock::Create(context_, "done", function);

        llvm::IRBuilder<> builder(entry);
        auto *zero = llvm::ConstantInt::get(i64Type, 0);
        auto *minus = llvm::ConstantInt::get(i8Type, '-');

        auto *isNegative = builder.CreateICmpSLT(value, zero, "is.negative");
        auto *magnitude = builder.CreateSelect(isNegative,
                                               builder.CreateSub(zero, value, "abs"),
                                               value,
                                               "magnitude");
        auto *signOffset = builder.CreateZExt(isNegative, i64Type, "sign.offset");
        auto *digitDest = builder.CreateGEP(i8Type, dest, signOffset, "digit.dest");
        auto *digitCount = builder.CreateCall(getOrCreateDashInterpWriteUInt(), {digitDest, magnitude}, "digit.count");
        auto *result = builder.CreateAdd(digitCount, signOffset, "result");
        builder.CreateCondBr(isNegative, negativeBlock, done);

        builder.SetInsertPoint(negativeBlock);
        auto *signPtr = builder.CreateGEP(i8Type, dest, zero, "sign.ptr");
        builder.CreateStore(minus, signPtr);
        builder.CreateBr(done);

        builder.SetInsertPoint(done);
        builder.CreateRet(result);

        return dashInterpWriteIntFunction_;
    }

    llvm::Function *CodeGenerator::getOrCreateSnprintf()
    {
        if (snprintfFunction_ != nullptr)
            return snprintfFunction_;
        auto *type = llvm::FunctionType::get(llvm::Type::getInt32Ty(context_), {llvm::PointerType::get(context_, 0), llvm::Type::getInt64Ty(context_), llvm::PointerType::get(context_, 0)}, true);
        snprintfFunction_ = llvm::cast<llvm::Function>(module_->getOrInsertFunction("snprintf", type).getCallee());
        return snprintfFunction_;
    }

    llvm::Function *CodeGenerator::getOrCreatePow()
    {
        if (powFunction_ != nullptr)
            return powFunction_;
        auto *type = llvm::FunctionType::get(llvm::Type::getDoubleTy(context_), {llvm::Type::getDoubleTy(context_), llvm::Type::getDoubleTy(context_)}, false);
        powFunction_ = llvm::cast<llvm::Function>(module_->getOrInsertFunction("pow", type).getCallee());
        return powFunction_;
    }

    void CodeGenerator::populateManualDashAbiLinkSymbols(const std::vector<std::string> &extraLinkArgs)
    {
        manualDashAbiFunctionUniqueSymbols_.clear();
        manualDashAbiGlobalUniqueSymbols_.clear();
        manualDashAbiModuleNames_.clear();

        for (const auto &arg : extraLinkArgs)
        {
            if (arg.empty() || arg[0] == '-')
                continue;

            const auto path = std::filesystem::path(arg);
            if (!isStaticLinkInputExtension(path) && !isSharedLibraryExtension(path))
                continue;

            const auto stem = sanitizeDashAbiFragment(path.stem().string());
            if (!stem.empty())
                manualDashAbiModuleNames_.push_back(stem);

            const auto info = scanStaticLinkInput(path);
            for (const auto &symbol : info.definedSymbols)
            {
                if (const auto uq = dashAbiFunctionUnqualifiedKeyFromSymbol(symbol); uq.has_value())
                {
                    if (const auto it = manualDashAbiFunctionUniqueSymbols_.find(*uq); it == manualDashAbiFunctionUniqueSymbols_.end())
                        manualDashAbiFunctionUniqueSymbols_[*uq] = symbol;
                    else if (it->second != symbol)
                        it->second.clear();
                    continue;
                }
                if (const auto uq = dashAbiGlobalUnqualifiedKeyFromSymbol(symbol); uq.has_value())
                {
                    if (const auto it = manualDashAbiGlobalUniqueSymbols_.find(*uq); it == manualDashAbiGlobalUniqueSymbols_.end())
                        manualDashAbiGlobalUniqueSymbols_[*uq] = symbol;
                    else if (it->second != symbol)
                        it->second.clear();
                }
            }
        }
    }

    void CodeGenerator::emitLLVMToFile(const std::string &path) const
    {
        const auto outputPath = std::filesystem::path(path);
        if (!outputPath.parent_path().empty())
        {
            std::filesystem::create_directories(outputPath.parent_path());
        }
        std::error_code error;
        llvm::raw_fd_ostream out(path, error, llvm::sys::fs::OF_None);
        if (error)
        {
            core::throwDiagnostic(core::SourceLocation{}, "failed to open IR output file: " + error.message());
        }
        module_->print(out, nullptr);
    }

    void CodeGenerator::emitObjectFile(const std::string &path, bool positionIndependent)
    {
        std::string error;
        const llvm::Target *target = llvm::TargetRegistry::lookupTarget(module_->getTargetTriple(), error);
        if (target == nullptr)
        {
            core::throwDiagnostic(core::SourceLocation{}, "failed to resolve LLVM target: " + error);
        }

        llvm::TargetOptions options;
        const auto relocModel = positionIndependent ? std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_) : std::nullopt;
        auto targetMachine = std::unique_ptr<llvm::TargetMachine>(
            target->createTargetMachine(module_->getTargetTriple(), "generic", "", options, relocModel));
        module_->setDataLayout(targetMachine->createDataLayout());

        std::error_code fileError;
        llvm::raw_fd_ostream out(path, fileError, llvm::sys::fs::OF_None);
        if (fileError)
        {
            core::throwDiagnostic(core::SourceLocation{}, "failed to open object output file: " + fileError.message());
        }

        llvm::legacy::PassManager passManager;
        if (targetMachine->addPassesToEmitFile(passManager, out, nullptr, llvm::CodeGenFileType::ObjectFile))
        {
            core::throwDiagnostic(core::SourceLocation{}, "target machine cannot emit object files for this target");
        }
        passManager.run(*module_);
        out.flush();
    }
        namespace
    {
        [[nodiscard]] std::string quoteShellArg(const std::string &value)
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

        [[nodiscard]] std::vector<std::filesystem::path> defaultElfSearchDirs()
        {
            return {
                "/usr/lib", "/usr/lib64", "/lib", "/lib64",
                "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu",
                "/usr/lib/aarch64-linux-gnu", "/lib/aarch64-linux-gnu",
                "/usr/lib/riscv64-linux-gnu", "/lib/riscv64-linux-gnu"
            };
        }

        [[nodiscard]] std::filesystem::path findFirstExisting(const std::vector<std::filesystem::path> &candidates)
        {
            std::error_code ec;
            for (const auto &candidate : candidates)
            {
                if (!candidate.empty() && std::filesystem::exists(candidate, ec))
                    return candidate;
                ec.clear();
            }
            return {};
        }

        [[nodiscard]] std::filesystem::path findRuntimeObject(const std::string &name)
        {
            std::vector<std::filesystem::path> candidates;
            for (const auto &dir : defaultElfSearchDirs())
                candidates.push_back(dir / name);
            return findFirstExisting(candidates);
        }

        [[nodiscard]] std::filesystem::path findDynamicLinker(const std::string &triple)
        {
            std::vector<std::filesystem::path> candidates;
            if (triple.find("x86_64") != std::string::npos)
            {
                candidates = {"/lib64/ld-linux-x86-64.so.2", "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", "/lib/ld-linux-x86-64.so.2"};
            }
            else if (triple.find("aarch64") != std::string::npos)
            {
                candidates = {"/lib/ld-linux-aarch64.so.1", "/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"};
            }
            else if (triple.find("riscv64") != std::string::npos)
            {
                candidates = {"/lib/ld-linux-riscv64-lp64d.so.1", "/lib/riscv64-linux-gnu/ld-linux-riscv64-lp64d.so.1"};
            }
            else if (triple.find("i386") != std::string::npos || triple.find("i686") != std::string::npos)
            {
                candidates = {"/lib/ld-linux.so.2", "/lib/i386-linux-gnu/ld-linux.so.2"};
            }
            return findFirstExisting(candidates);
        }

        [[nodiscard]] std::string pkgConfigLibsOnly(const std::string &package)
        {
            const std::string command = "pkg-config --libs " + package + " 2>/dev/null";
            FILE *pipe = popen(command.c_str(), "r");
            if (pipe == nullptr)
                return {};
            std::string out;
            char buffer[256];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
                out += buffer;
            pclose(pipe);
            while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
                out.pop_back();
            return out;
        }

        [[nodiscard]] std::string normalizeLdArg(const std::string &arg)
        {
            if (arg.rfind("-Wl,", 0) != 0)
                return arg;
            std::string out;
            std::stringstream ss(arg.substr(4));
            std::string item;
            bool first = true;
            while (std::getline(ss, item, ','))
            {
                if (item.empty())
                    continue;
                if (!first)
                    out += ' ';
                out += item;
                first = false;
            }
            return out;
        }
    }

    std::string CodeGenerator::buildExtraLinkFlags(const std::vector<std::string> &linkProfiles, const std::vector<std::string> &extraLinkArgs, bool useDashRuntime, bool smartLinking) const
    {
        std::string flags;
        for (const auto &profile : linkProfiles)
        {
            if (profile == "gtk4")
            {
                const auto libs = pkgConfigLibsOnly("gtk4");
                if (libs.empty())
                    core::throwDiagnostic(core::SourceLocation{}, "pkg-config could not resolve gtk4 linker flags");
                flags += " " + libs;
                continue;
            }
            core::throwDiagnostic(core::SourceLocation{}, "unsupported -L profile during link: " + profile);
        }

        const auto dashSupportPlan = discoverDashSupportLinkPlan(usedExternCSymbols_, smartLinking);
        if (dashSupportPlan.usesSharedDashLibs)
        {
            const auto sharedDir = dashSharedSearchDir();
            if (!sharedDir.empty())
            {
                flags += " -L" + quoteShellArg(sharedDir.string());
#if !defined(_WIN32)
                flags += " -rpath " + quoteShellArg(sharedDir.string());
#endif
            }
        }
        for (const auto &input : dashSupportPlan.linkInputs)
            flags += " " + quoteShellArg(input.string());

        if (useDashRuntime)
        {
            const auto runtimeDir = dashRuntimeSearchDir();
            const auto runtimeLibs = discoverDashRuntimeLibraries();
            if (!runtimeDir.empty() && !runtimeLibs.empty())
            {
                flags += " -L" + quoteShellArg(runtimeDir);
#if !defined(_WIN32)
                flags += " -rpath " + quoteShellArg(runtimeDir);
#endif
                for (const auto &lib : runtimeLibs)
                    flags += " " + quoteShellArg(lib.string());
            }
        }

        for (const auto &arg : extraLinkArgs)
        {
            const auto normalized = normalizeLdArg(arg);
            if (normalized.empty())
                continue;
            flags += " ";
            const bool rawLinkerFlag = normalized.rfind("-l", 0) == 0 || normalized.rfind("-L", 0) == 0 || normalized.rfind("-rpath", 0) == 0 || normalized[0] == '-';
            if (rawLinkerFlag)
                flags += normalized;
            else
                flags += quoteShellArg(normalized);
        }
        return flags;
    }

    void CodeGenerator::linkExecutable(const std::string &objectPath, const std::string &outputPath, const std::vector<std::string> &linkProfiles, const std::vector<std::string> &extraLinkArgs, bool useDashRuntime, bool smartLinking) const
    {
#if defined(_WIN32) || defined(__APPLE__)
        core::throwDiagnostic(core::SourceLocation{}, "ld-based linking is currently supported only on ELF targets");
#else
        const auto crt1 = findRuntimeObject("crt1.o");
        const auto crti = findRuntimeObject("crti.o");
        const auto crtn = findRuntimeObject("crtn.o");
        const auto dynamicLinker = findDynamicLinker(module_->getTargetTriple().str());
        if (crt1.empty() || crti.empty() || crtn.empty() || dynamicLinker.empty())
            core::throwDiagnostic(core::SourceLocation{}, "failed to resolve ELF runtime objects for ld link step");

        const std::string command =
            std::string("ld -o ") + quoteShellArg(outputPath) +
            " " + quoteShellArg(crt1.string()) +
            " " + quoteShellArg(crti.string()) +
            " " + quoteShellArg(objectPath) +
            buildExtraLinkFlags(linkProfiles, extraLinkArgs, useDashRuntime, smartLinking) +
            " -dynamic-linker " + quoteShellArg(dynamicLinker.string()) +
            " -lc -lm " + quoteShellArg(crtn.string());
        if (std::system(command.c_str()) != 0)
            core::throwDiagnostic(core::SourceLocation{}, "link step failed: " + command);
#endif
    }

    void CodeGenerator::linkSharedLibrary(const std::string &objectPath, const std::string &outputPath, const std::vector<std::string> &linkProfiles, const std::vector<std::string> &extraLinkArgs, bool useDashRuntime, bool smartLinking) const
    {
#if defined(_WIN32) || defined(__APPLE__)
        core::throwDiagnostic(core::SourceLocation{}, "ld-based shared linking is currently supported only on ELF targets");
#else
        const std::string soname = std::filesystem::path(outputPath).filename().string();
        const std::string command =
            std::string("ld -shared -o ") + quoteShellArg(outputPath) +
            " " + quoteShellArg(objectPath) +
            buildExtraLinkFlags(linkProfiles, extraLinkArgs, useDashRuntime, smartLinking) +
            " -soname " + quoteShellArg(soname) +
            " -lc -lm";
        if (std::system(command.c_str()) != 0)
            core::throwDiagnostic(core::SourceLocation{}, "shared link step failed: " + command);
#endif
    }

} // namespace dash::codegen

#pragma once

#include <string>
#include <unordered_map>

#include "dash/ast/ast.hpp"

namespace dash::sema
{

    struct FunctionSymbol
    {
        std::string name;
        std::vector<ast::Parameter> parameters;
        core::TypeRef returnType{};
        bool isExtern{false};
        bool isPrivate{false};
        std::string sourceFile;
        bool deprecated{false};
        bool risky{false};
        std::string customWarning;
    };

    struct VariableSymbol
    {
        std::string name;
        core::TypeRef type{};
        bool isMutable{false};
    };

    struct ClassSymbol
    {
        std::string name;
        bool isStatic{false};
        bool isGroup{false};
        std::unordered_map<std::string, ast::FieldDecl> fields;
        std::vector<ast::FieldDecl> fieldOrder;
        std::unordered_map<std::string, const ast::MemberFunctionDecl *> methods;
    };

    struct EnumSymbol
    {
        std::string name;
        std::unordered_map<std::string, std::int64_t> items;
    };

    class Analyzer
    {
    public:
        void analyze(ast::Program &program);
        void setSharedBuild(bool shared) { isSharedBuild_ = shared; }
        void setEntryPointRequired(bool required) { requireEntryPoint_ = required; }

    private:
        void collectClasses(ast::Program &program);
        void collectEnums(ast::Program &program);
        void collectFunctions(ast::Program &program);
        void collectGlobalVariables(ast::Program &program);
        void analyzeFunction(ast::FunctionDecl &function);
        void analyzeClassFields(ast::ClassDecl &klass);
        void analyzeClassMethod(ast::ClassDecl &klass, ast::MemberFunctionDecl &method);
        void analyzeBlock(ast::BlockStmt &block, bool createScope = true);
        void analyzeStatement(ast::Stmt &stmt);
        core::TypeRef analyzeExpr(ast::Expr &expr);
        void validateMainSignature(const ast::FunctionDecl &function) const;

        [[nodiscard]] const ast::MemberFunctionDecl *findConstructor(const ClassSymbol &klass) const;
        [[nodiscard]] const FunctionSymbol &requireFunction(const std::string &name, const core::SourceLocation &location) const;
        [[nodiscard]] const ClassSymbol &requireClass(const std::string &name, const core::SourceLocation &location) const;
        [[nodiscard]] VariableSymbol &requireVariable(const std::string &name, const core::SourceLocation &location);
        [[nodiscard]] const VariableSymbol &requireVariable(const std::string &name, const core::SourceLocation &location) const;
        [[nodiscard]] core::TypeRef resolveTypeQuery(const core::TypeRef& type, const core::SourceLocation& location) const;

        void pushScope();
        void popScope();
        void declareVariable(const VariableSymbol &symbol, const core::SourceLocation &location);

        std::unordered_map<std::string, FunctionSymbol> functions_;
        std::unordered_map<std::string, FunctionSymbol> privateFunctions_;
        std::unordered_map<std::string, ClassSymbol> classes_;
        std::unordered_map<std::string, EnumSymbol> enums_;
        std::vector<std::unordered_map<std::string, VariableSymbol>> scopes_;
        core::TypeRef currentReturnType_{};
        const ClassSymbol *currentClass_{nullptr};
        bool isSharedBuild_{false};
        bool requireEntryPoint_{true};
    };

} // namespace dash::sema

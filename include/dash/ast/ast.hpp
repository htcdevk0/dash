#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dash/core/source_location.hpp"
#include "dash/core/types.hpp"

namespace dash::ast {

struct Node {
    core::SourceLocation location;
    virtual ~Node() = default;
};

struct Expr : Node {
    core::TypeRef inferredType {};
    virtual ~Expr() = default;
};

struct Stmt : Node {
    virtual ~Stmt() = default;
};

struct Decl : Node {
    virtual ~Decl() = default;
};

struct Annotation {
    std::string name;
    std::string argument;
    core::SourceLocation location;
};

struct Parameter {
    std::string name;
    core::TypeRef type {};
    bool isVariadic {false};
    bool isArrayLet {false};
    core::SourceLocation location;
};

struct IntegerLiteralExpr final : Expr {
    std::uint64_t value {0};
    bool forceUnsigned {false};
};

struct DoubleLiteralExpr final : Expr {
    double value {0.0};
};

struct BoolLiteralExpr final : Expr {
    bool value {false};
};

struct StringLiteralExpr final : Expr {
    std::string value;
};

struct InterpolatedStringExpr final : Expr {
    std::vector<std::string> textSegments;
    std::vector<std::unique_ptr<Expr>> expressions;
};

struct CharLiteralExpr final : Expr {
    std::uint8_t value {0};
};

struct NullLiteralExpr final : Expr {
};

struct ExtractDataExpr final : Expr {
    std::unique_ptr<Expr> operand;
};

struct CastExpr final : Expr {
    core::TypeRef targetType {};
    std::unique_ptr<Expr> operand;
};

struct ArrayLiteralExpr final : Expr {
    std::vector<std::unique_ptr<Expr>> elements;
};

struct VariableExpr final : Expr {
    std::string name;
};

struct ParenExpr final : Expr {
    std::unique_ptr<Expr> operand;
    bool preferVariadicCount {false};
};

struct UnaryExpr final : Expr {
    char op {'\0'};
    std::unique_ptr<Expr> operand;
};

struct BinaryExpr final : Expr {
    std::string op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

struct TernaryExpr final : Expr {
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> thenExpr;
    std::unique_ptr<Expr> elseExpr;
};

struct CallExpr final : Expr {
    std::string callee;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct IndexExpr final : Expr {
    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
};

struct SizeExpr final : Expr {
    std::unique_ptr<Expr> object;
};

struct ArrayPushExpr final : Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> value;
    std::unique_ptr<Expr> index;
};

struct ArrayInsertExpr final : Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> index;
    std::unique_ptr<Expr> value;
};

struct ArraySetExpr final : Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> index;
    std::unique_ptr<Expr> value;
};

struct ArrayRemoveExpr final : Expr {
    std::unique_ptr<Expr> array;
    std::unique_ptr<Expr> index;
};

struct VariableDeclStmt final : Stmt {
    bool isMutable {false};
    bool hasExplicitType {false};
    std::string name;
    core::TypeRef type {};
    std::unique_ptr<Expr> initializer;
};

struct DeclGroupStmt final : Stmt {
    std::vector<std::unique_ptr<Stmt>> statements;
};

struct AssignmentStmt final : Stmt {
    std::string name;
    std::unique_ptr<Expr> value;
};

struct DerefAssignmentStmt final : Stmt {
    std::unique_ptr<Expr> pointer;
    std::unique_ptr<Expr> value;
};

struct ReturnStmt final : Stmt {
    std::unique_ptr<Expr> value;
};

struct ExprStmt final : Stmt {
    std::unique_ptr<Expr> expr;
};


struct BreakStmt final : Stmt {
};

struct BlockStmt final : Stmt {
    std::vector<std::unique_ptr<Stmt>> statements;
};

struct IfStmt final : Stmt {
    std::unique_ptr<Expr> condition;
    std::unique_ptr<BlockStmt> thenBlock;
    std::unique_ptr<Stmt> elseBranch;
};

struct WhileStmt final : Stmt {
    std::unique_ptr<Expr> condition;
    std::unique_ptr<BlockStmt> body;
};

struct DoWhileStmt final : Stmt {
    std::unique_ptr<BlockStmt> body;
    std::unique_ptr<Expr> condition;
};

struct ForStmt final : Stmt {
    std::unique_ptr<Stmt> initializer;
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Stmt> increment;
    std::unique_ptr<BlockStmt> body;
};

struct SwitchStmt final : Stmt {
    std::unique_ptr<Stmt> lowered;
};

struct MatchStmt final : Stmt {
    std::unique_ptr<Stmt> lowered;
};

struct ExternDecl final : Decl {
    std::string abi;
    std::string name;
    std::vector<Parameter> parameters;
    bool isPrivate = false;
    core::TypeRef returnType {};
};

struct FunctionDecl final : Decl {

    std::string name;
    std::vector<Parameter> parameters;
    core::TypeRef returnType {};
    std::unique_ptr<BlockStmt> body;
    bool isExport {false};
    std::vector<Annotation> annotations;
};

struct GlobalVarDecl final : Decl {
    bool isMutable {false};
    bool isExport {false};
    bool isExtern {false};
    bool isPrivate {false};
    bool hasExplicitType {false};
    std::string abi;
    std::string name;
    core::TypeRef type {};
    std::unique_ptr<Expr> initializer;
};


struct EnumItem {
    std::string name;
    std::int64_t value {0};
    core::SourceLocation location;
};

struct EnumDecl final : Decl {
    std::string name;
    std::vector<EnumItem> items;
};

struct FieldDecl {
    std::string name;
    core::TypeRef type {};
    bool isPrivate {false};
    bool isMutable {false};
    bool isExtern {false};
    std::string abi;
    const Expr* initializer {nullptr};
    core::SourceLocation location;
};

struct MemberFunctionDecl {
    std::string name;
    std::vector<Parameter> parameters;
    core::TypeRef returnType {};
    std::unique_ptr<BlockStmt> body;
    bool isPrivate {false};
    bool isExtern {false};
    std::string abi;
    core::SourceLocation location;
    std::vector<Annotation> annotations;
};

struct ClassDecl final : Decl {
    std::string name;
    bool isStatic {false};
    bool isGroup {false};
    std::vector<FieldDecl> fields;
    std::vector<MemberFunctionDecl> methods;
};

struct Program final : Node {
    std::vector<std::unique_ptr<Decl>> declarations;
};

struct IsTypeExpr : Expr {
    core::TypeRef type;
    std::string variable;
};

struct VariadicIndexExpr : Expr {
    std::string name;
    std::unique_ptr<Expr> index;

    VariadicIndexExpr(std::string n, std::unique_ptr<Expr> i)
        : name(std::move(n)), index(std::move(i)) {}
};

struct VariadicSizeExpr : Expr {
    std::string name;

    explicit VariadicSizeExpr(std::string n)
        : name(std::move(n)) {}
};

struct VariadicForwardExpr : Expr {
    std::string name;

    explicit VariadicForwardExpr(std::string n)
        : name(std::move(n)) {}
};

struct MemberExpr : Expr {
    std::unique_ptr<Expr> object;
    std::string member;
};

struct MethodCallExpr : Expr {
    std::unique_ptr<Expr> object;
    std::string method;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct MemberAssignmentStmt : Stmt {
    std::unique_ptr<Expr> object;
    std::string member;
    std::unique_ptr<Expr> value;
};

} // namespace dash::ast

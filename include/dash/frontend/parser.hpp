#pragma once

#include <deque>
#include <memory>
#include <unordered_set>
#include <vector>

#include "dash/ast/ast.hpp"
#include "dash/frontend/token.hpp"

namespace dash::frontend {

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string filePath);

    [[nodiscard]] std::unique_ptr<ast::Program> parseProgram();

private:
    [[nodiscard]] bool atEnd() const noexcept;
    [[nodiscard]] const Token& current() const noexcept;
    [[nodiscard]] const Token& previous() const noexcept;
    [[nodiscard]] const Token& advance() noexcept;
    [[nodiscard]] bool check(TokenKind kind) const noexcept;
    [[nodiscard]] bool match(TokenKind kind) noexcept;
    [[nodiscard]] const Token& expect(TokenKind kind, const std::string& message);

    [[nodiscard]] std::unique_ptr<ast::Decl> parseTopLevel();
    void parseNamespaceBlock();
    void qualifyDeclaration(ast::Decl &decl);
    [[nodiscard]] std::vector<ast::Annotation> parseAnnotations();
    [[nodiscard]] std::unique_ptr<ast::Decl> parseExternDecl(bool isPrivate);
    [[nodiscard]] std::unique_ptr<ast::FunctionDecl> parseFunctionDecl(bool isExport = false, std::vector<ast::Annotation> annotations = {});
    [[nodiscard]] std::unique_ptr<ast::ClassDecl> parseClassDecl();
    [[nodiscard]] std::unique_ptr<ast::ClassDecl> parseGroupDecl();
    [[nodiscard]] std::unique_ptr<ast::EnumDecl> parseEnumDecl();
    [[nodiscard]] std::unique_ptr<ast::GlobalVarDecl> parseGlobalVarDecl(bool isMutable, bool isExport = false);
    [[nodiscard]] std::vector<ast::Parameter> parseParameterList();
    [[nodiscard]] ast::Parameter parseParameter();
    [[nodiscard]] core::TypeRef parseType();
    [[nodiscard]] std::vector<ast::FieldDecl> parseClassFields(bool isPrivate, bool isMutable);
    [[nodiscard]] ast::FieldDecl parseGroupField();
    [[nodiscard]] ast::MemberFunctionDecl parseClassMethod(bool isPrivate, std::vector<ast::Annotation> annotations = {});
    void parseClassExternMember(ast::ClassDecl& decl, bool isPrivate);

    [[nodiscard]] std::unique_ptr<ast::BlockStmt> parseBlock();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseStatement();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseIfStmt();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseWhileStmt();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseForStmt();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseDoWhileStmt();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseSwitchStmt();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseMatchStmt();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseVariableDecl(bool isMutable, bool expectSemicolon = true);
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseReturnStmt();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseBreakStmt();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseIdentifierLedStatement(bool expectSemicolon = true);
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseExpressionStatement(bool expectSemicolon = true);
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseForInitializer();
    [[nodiscard]] std::unique_ptr<ast::Stmt> parseForIncrement();

    [[nodiscard]] std::unique_ptr<ast::Expr> parseExpression();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseConditional();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseLogicalOr();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseLogicalAnd();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseEquality();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseComparison();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseAdditive();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseMultiplicative();
    [[nodiscard]] std::unique_ptr<ast::Expr> parsePower();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseUnary();
    [[nodiscard]] std::unique_ptr<ast::Expr> parsePrimary();
    [[nodiscard]] std::unique_ptr<ast::Expr> parseStringLiteralToken(const Token& token);
    [[nodiscard]] std::vector<std::unique_ptr<ast::Expr>> parseArgumentList();
    [[nodiscard]] bool isCastTypeStart() const noexcept;
    [[nodiscard]] std::unique_ptr<ast::Expr> parseBuiltinDataExpr();

    std::vector<Token> tokens_;
    std::string filePath_;
    std::size_t index_ {0};
    std::deque<std::unique_ptr<ast::Decl>> pendingDeclarations_;
    std::unordered_set<std::string> knownTypeNames_;
    std::vector<std::string> namespaceStack_;
};

} // namespace dash::frontend

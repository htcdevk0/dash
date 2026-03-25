#include "dash/frontend/parser.hpp"

#include <charconv>
#include <cstdlib>
#include <limits>
#include <system_error>
#include <utility>

#include "dash/core/diagnostic.hpp"
#include "dash/frontend/lexer.hpp"
#include "dash/frontend/token.hpp"

namespace dash::frontend
{

    namespace
    {

        [[nodiscard]] core::TypeRef typeFromToken(TokenKind kind)
        {
            switch (kind)
            {
            case TokenKind::KwVoid:
                return core::TypeRef{core::BuiltinTypeKind::Void, ""};
            case TokenKind::KwBool:
                return core::TypeRef{core::BuiltinTypeKind::Bool, ""};
            case TokenKind::KwInt:
                return core::TypeRef{core::BuiltinTypeKind::Int, ""};
            case TokenKind::KwUInt:
                return core::TypeRef{core::BuiltinTypeKind::UInt, ""};
            case TokenKind::KwDouble:
                return core::TypeRef{core::BuiltinTypeKind::Double, ""};
            case TokenKind::KwFloat:
                return core::TypeRef{core::BuiltinTypeKind::Double, ""};
            case TokenKind::KwChar:
                return core::TypeRef{core::BuiltinTypeKind::Char, ""};
            case TokenKind::KwString:
                return core::TypeRef{core::BuiltinTypeKind::String, ""};
            default:
                return core::TypeRef{core::BuiltinTypeKind::Unknown, ""};
            }
        }

        [[nodiscard]] std::uint64_t parseUnsignedIntegerLiteral(const Token &token)
        {
            std::string text = token.lexeme;
            int base = 10;

            if ((text.rfind("F0x", 0) == 0) || (text.rfind("f0x", 0) == 0))
            {
                text = text.substr(1);
            }

            if ((text.rfind("0x", 0) == 0) || (text.rfind("0X", 0) == 0))
            {
                base = 16;
                text = text.substr(2);
            }

            std::uint64_t value = 0;
            const auto *begin = text.data();
            const auto *end = text.data() + text.size();
            const auto result = std::from_chars(begin, end, value, base);
            if (result.ec != std::errc{} || result.ptr != end)
            {
                core::throwDiagnostic(token.location, "invalid integer literal: " + token.lexeme);
            }
            return value;
        }


        [[nodiscard]] std::unique_ptr<ast::Expr> cloneExprForCompoundAssignment(const ast::Expr &expr)
        {
            if (const auto *var = dynamic_cast<const ast::VariableExpr *>(&expr))
            {
                auto copy = std::make_unique<ast::VariableExpr>();
                copy->location = var->location;
                copy->inferredType = var->inferredType;
                copy->name = var->name;
                return copy;
            }
            if (const auto *member = dynamic_cast<const ast::MemberExpr *>(&expr))
            {
                auto copy = std::make_unique<ast::MemberExpr>();
                copy->location = member->location;
                copy->inferredType = member->inferredType;
                copy->object = cloneExprForCompoundAssignment(*member->object);
                copy->member = member->member;
                return copy;
            }
            if (const auto *unary = dynamic_cast<const ast::UnaryExpr *>(&expr))
            {
                auto copy = std::make_unique<ast::UnaryExpr>();
                copy->location = unary->location;
                copy->inferredType = unary->inferredType;
                copy->op = unary->op;
                copy->operand = cloneExprForCompoundAssignment(*unary->operand);
                return copy;
            }
            core::throwDiagnostic(expr.location, "invalid compound assignment target");
        }

        [[nodiscard]] bool envTargetMatches(const std::string &value)
        {
#if defined(_WIN32)
            return value == "windows";
#elif defined(__APPLE__)
            return value == "mac";
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
            return value == "bsd";
#elif defined(__linux__)
            return value == "linux";
#else
            return false;
#endif
        }

    } // namespace

    Parser::Parser(std::vector<Token> tokens, std::string filePath)
        : tokens_(std::move(tokens)), filePath_(std::move(filePath)) {}

    std::unique_ptr<ast::Expr> Parser::parseStringLiteralToken(const Token &token)
    {
        if (token.kind == TokenKind::StringLiteral)
        {
            auto node = std::make_unique<ast::StringLiteralExpr>();
            node->location = token.location;
            node->value = token.lexeme;
            return node;
        }

        auto node = std::make_unique<ast::InterpolatedStringExpr>();
        node->location = token.location;

        std::string currentText;
        const auto &text = token.lexeme;

        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '{')
            {
                currentText.push_back(text[i]);
                continue;
            }

            node->textSegments.push_back(currentText);
            currentText.clear();

            const auto exprStart = i + 1;
            std::size_t j = exprStart;
            int depth = 1;
            bool inString = false;
            bool inChar = false;
            bool escaped = false;

            for (; j < text.size(); ++j)
            {
                const char c = text[j];
                if (escaped)
                {
                    escaped = false;
                    continue;
                }
                if ((inString || inChar) && c == '\\')
                {
                    escaped = true;
                    continue;
                }
                if (inString)
                {
                    if (c == '"')
                        inString = false;
                    continue;
                }
                if (inChar)
                {
                    if (c == '\'')
                        inChar = false;
                    continue;
                }
                if (c == '"')
                {
                    inString = true;
                    continue;
                }
                if (c == '\'')
                {
                    inChar = true;
                    continue;
                }
                if (c == '{')
                {
                    ++depth;
                    continue;
                }
                if (c == '}')
                {
                    --depth;
                    if (depth == 0)
                        break;
                }
            }

            if (j >= text.size() || depth != 0)
                core::throwDiagnostic(token.location, "unterminated interpolation in string literal");

            const auto exprText = text.substr(exprStart, j - exprStart);
            Lexer lexer(filePath_, exprText);
            auto exprTokens = lexer.tokenize();
            Parser exprParser(std::move(exprTokens), filePath_);
            exprParser.knownTypeNames_ = knownTypeNames_;
            auto embeddedExpr = exprParser.parseExpression();
            if (!exprParser.atEnd())
                core::throwDiagnostic(token.location, "invalid expression inside interpolated string");
            node->expressions.push_back(std::move(embeddedExpr));

            i = j;
        }

        node->textSegments.push_back(currentText);

        if (node->expressions.empty())
        {
            auto plain = std::make_unique<ast::StringLiteralExpr>();
            plain->location = token.location;
            plain->value = token.lexeme;
            return plain;
        }

        return node;
    }

    std::unique_ptr<ast::Program> Parser::parseProgram()
    {
        auto program = std::make_unique<ast::Program>();
        program->location = core::SourceLocation{filePath_, 1, 1};

        while (!atEnd() || !pendingDeclarations_.empty())
        {
            if (!pendingDeclarations_.empty())
            {
                program->declarations.push_back(std::move(pendingDeclarations_.front()));
                pendingDeclarations_.pop_front();
                continue;
            }
            program->declarations.push_back(parseTopLevel());
        }

        return program;
    }

    bool Parser::atEnd() const noexcept
    {
        return current().kind == TokenKind::EndOfFile;
    }

    const Token &Parser::current() const noexcept
    {
        return tokens_[index_];
    }

    const Token &Parser::previous() const noexcept
    {
        return tokens_[index_ - 1];
    }

    const Token &Parser::advance() noexcept
    {
        if (!atEnd())
        {
            ++index_;
        }
        return previous();
    }

    bool Parser::check(TokenKind kind) const noexcept
    {
        if (atEnd())
        {
            return false;
        }
        return current().kind == kind;
    }

    bool Parser::match(TokenKind kind) noexcept
    {
        if (!check(kind))
        {
            return false;
        }
        (void)advance();
        return true;
    }

    const Token &Parser::expect(TokenKind kind, const std::string &message)
    {
        if (!check(kind))
        {
            core::throwDiagnostic(current().location, message + ", got " + tokenKindName(current().kind));
        }
        return advance();
    }
    std::vector<ast::Annotation> Parser::parseAnnotations()
    {
        std::vector<ast::Annotation> annotations;
        while (match(TokenKind::At))
        {
            ast::Annotation ann;
            ann.location = previous().location;
            ann.name = expect(TokenKind::Identifier, "expected annotation name after '@'").lexeme;
            if (match(TokenKind::LParen))
            {
                if (!check(TokenKind::RParen))
                {
                    ann.argument = expect(TokenKind::StringLiteral, "expected string literal annotation argument").lexeme;
                }
                (void)expect(TokenKind::RParen, "expected ')' after annotation arguments");
            }
            annotations.push_back(std::move(ann));
        }
        return annotations;
    }

    void Parser::qualifyDeclaration(ast::Decl &decl)
    {
        if (namespaceStack_.empty())
            return;

        std::string prefix;
        for (std::size_t i = 0; i < namespaceStack_.size(); ++i)
        {
            if (i != 0)
                prefix += "::";
            prefix += namespaceStack_[i];
        }

        if (auto *fn = dynamic_cast<ast::FunctionDecl *>(&decl))
        {
            fn->name = prefix + "::" + fn->name;
            return;
        }
        if (auto *ext = dynamic_cast<ast::ExternDecl *>(&decl))
        {
            ext->name = prefix + "::" + ext->name;
            return;
        }
        if (auto *global = dynamic_cast<ast::GlobalVarDecl *>(&decl))
        {
            global->name = prefix + "::" + global->name;
            return;
        }
    }

    void Parser::parseNamespaceBlock()
    {
        const auto &nameToken = expect(TokenKind::StringLiteral, "expected namespace name string");
        if (nameToken.lexeme.empty())
            core::throwDiagnostic(nameToken.location, "namespace name cannot be empty");

        namespaceStack_.push_back(nameToken.lexeme);
        (void)expect(TokenKind::LBrace, "expected '{' to start namespace body");

        while (!check(TokenKind::RBrace) && !atEnd())
        {
            auto annotations = parseAnnotations();

            if (match(TokenKind::KwNamespace))
            {
                if (!annotations.empty())
                    core::throwDiagnostic(annotations.front().location, "annotations cannot be applied to namespace blocks");
                parseNamespaceBlock();
                continue;
            }

            std::unique_ptr<ast::Decl> decl;

            if (match(TokenKind::KwPrivate))
            {
                if (!annotations.empty())
                    core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
                if (!match(TokenKind::KwExtern))
                    core::throwDiagnostic(current().location, "expected extern after private");
                decl = parseExternDecl(true);
            }
            else if (match(TokenKind::KwExtern))
            {
                if (!annotations.empty())
                    core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
                decl = parseExternDecl(false);
            }
            else if (match(TokenKind::KwFn))
            {
                decl = parseFunctionDecl(false, std::move(annotations));
            }
            else if (match(TokenKind::KwExport))
            {
                if (match(TokenKind::KwFn))
                    decl = parseFunctionDecl(true, std::move(annotations));
                else if (match(TokenKind::KwLet))
                {
                    if (!annotations.empty())
                        core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
                    decl = parseGlobalVarDecl(true, true);
                }
                else if (match(TokenKind::KwConst))
                {
                    if (!annotations.empty())
                        core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
                    decl = parseGlobalVarDecl(false, true);
                }
                else
                    core::throwDiagnostic(current().location, "expected fn, let or const after export inside namespace");
            }
            else if (match(TokenKind::KwLet))
            {
                if (!annotations.empty())
                    core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
                decl = parseGlobalVarDecl(true);
            }
            else if (match(TokenKind::KwConst))
            {
                if (!annotations.empty())
                    core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
                decl = parseGlobalVarDecl(false);
            }
            else if (check(TokenKind::KwClass) || check(TokenKind::KwGroup))
            {
                core::throwDiagnostic(current().location, "classes and groups are not allowed inside namespaces");
            }
            else if (check(TokenKind::KwEnum))
            {
                core::throwDiagnostic(current().location, "enums are not allowed inside namespaces");
            }
            else
            {
                core::throwDiagnostic(current().location, "expected namespace declaration");
            }

            if (decl != nullptr)
            {
                qualifyDeclaration(*decl);
                pendingDeclarations_.push_back(std::move(decl));
            }
        }

        (void)expect(TokenKind::RBrace, "expected '}' to close namespace body");
        namespaceStack_.pop_back();
    }

    std::unique_ptr<ast::Decl> Parser::parseTopLevel()
    {
        auto annotations = parseAnnotations();

        if (match(TokenKind::KwPrivate))
        {
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
            if (!match(TokenKind::KwExtern))
            {
                core::throwDiagnostic(current().location, "expected extern after private");
            }
            return parseExternDecl(true);
        }

        if (match(TokenKind::KwExtern))
        {
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
            return parseExternDecl(false);
        }
        if (match(TokenKind::KwNamespace))
        {
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations cannot be applied to namespace blocks");
            parseNamespaceBlock();
            if (pendingDeclarations_.empty())
                core::throwDiagnostic(previous().location, "namespace block cannot be empty");
            auto out = std::move(pendingDeclarations_.front());
            pendingDeclarations_.pop_front();
            return out;
        }
        if (match(TokenKind::KwFn))
        {
            return parseFunctionDecl(false, std::move(annotations));
        }
        if (match(TokenKind::KwExport))
        {
            if (match(TokenKind::KwFn))
                return parseFunctionDecl(true, std::move(annotations));
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
            if (match(TokenKind::KwLet))
                return parseGlobalVarDecl(true, true);
            if (match(TokenKind::KwConst))
                return parseGlobalVarDecl(false, true);
            core::throwDiagnostic(current().location, "expected fn, let or const after export");
        }
        if (match(TokenKind::KwClass))
        {
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
            return parseClassDecl();
        }
        if (match(TokenKind::KwGroup))
        {
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
            return parseGroupDecl();
        }
        if (match(TokenKind::KwEnum))
        {
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
            return parseEnumDecl();
        }
        if (match(TokenKind::KwLet))
        {
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
            return parseGlobalVarDecl(true);
        }
        if (match(TokenKind::KwConst))
        {
            if (!annotations.empty())
                core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
            return parseGlobalVarDecl(false);
        }
        core::throwDiagnostic(current().location, "expected top-level declaration");
    }

    std::unique_ptr<ast::Decl> Parser::parseExternDecl(bool isPrivate)
    {
        const auto location = previous().location;

        std::string abiName = "dash";
        if (match(TokenKind::LParen))
        {
            const auto &abi = expect(TokenKind::StringLiteral, "expected ABI string in extern declaration");
            abiName = abi.lexeme;
            (void)expect(TokenKind::RParen, "expected ')' after extern ABI");
        }

        if (match(TokenKind::Colon))
        {
            auto decl = std::make_unique<ast::GlobalVarDecl>();
            decl->location = location;
            decl->isExtern = true;
            decl->isPrivate = isPrivate;
            decl->abi = abiName;
            decl->type = parseType();

            bool sawArrayLet = false;
            if (match(TokenKind::Arrow))
            {
                (void)expect(TokenKind::KwLet, "expected let after '->' in extern variable declaration");
                if (!decl->type.isArray())
                    core::throwDiagnostic(current().location, "extern variable property '->let' requires an array type");
                sawArrayLet = true;
            }

            decl->name = expect(TokenKind::Identifier, "expected extern variable name").lexeme;

            if (!sawArrayLet && match(TokenKind::Arrow))
            {
                (void)expect(TokenKind::KwLet, "expected let after '->' in extern variable declaration");
                if (!decl->type.isArray())
                    core::throwDiagnostic(current().location, "extern variable property '->let' requires an array type");
                sawArrayLet = true;
            }

            decl->isMutable = decl->type.isArray() ? sawArrayLet : true;
            (void)expect(TokenKind::Semicolon, "expected ';' after extern variable declaration");
            return decl;
        }

        auto decl = std::make_unique<ast::ExternDecl>();
        decl->location = location;
        decl->isPrivate = isPrivate;
        decl->abi = abiName;

        const auto &name = expect(TokenKind::Identifier, "expected extern function name");
        decl->name = name.lexeme;

        (void)expect(TokenKind::LParen, "expected '(' after extern function name");
        if (!check(TokenKind::RParen))
        {
            decl->parameters = parseParameterList();
        }
        (void)expect(TokenKind::RParen, "expected ')' after parameter list");
        (void)expect(TokenKind::Colon, "expected ':' before return type");
        decl->returnType = parseType();
        (void)expect(TokenKind::Semicolon, "expected ';' after extern declaration");

        return decl;
    }

    std::unique_ptr<ast::FunctionDecl> Parser::parseFunctionDecl(bool isExport, std::vector<ast::Annotation> annotations)
    {
        auto decl = std::make_unique<ast::FunctionDecl>();
        decl->location = previous().location;
        decl->isExport = isExport;
        decl->annotations = std::move(annotations);

        const auto &name = expect(TokenKind::Identifier, "expected function name");
        decl->name = name.lexeme;
        for (const auto &ann : decl->annotations)
        {
            if (ann.name == "Namespace")
                core::throwDiagnostic(ann.location, "@Namespace has been removed; use namespace \"name\" { ... } instead");
        }
        (void)expect(TokenKind::LParen, "expected '(' after function name");
        if (!check(TokenKind::RParen))
        {
            decl->parameters = parseParameterList();
        }
        (void)expect(TokenKind::RParen, "expected ')' after parameter list");
        if (match(TokenKind::Colon))
        {
            decl->returnType = parseType();
        }
        else if (decl->name == "main")
        {
            decl->returnType = core::TypeRef{core::BuiltinTypeKind::Int, ""};
        }
        else
        {
            core::throwDiagnostic(current().location, "expected ':' before return type");
        }
        decl->body = parseBlock();
        return decl;
    }

    std::unique_ptr<ast::ClassDecl> Parser::parseClassDecl()
    {
        auto decl = std::make_unique<ast::ClassDecl>();
        decl->location = previous().location;
        decl->name = expect(TokenKind::Identifier, "expected class name").lexeme;
        knownTypeNames_.insert(decl->name);

        if (match(TokenKind::Colon))
        {
            (void)expect(TokenKind::KwStatic, "expected static after ':' in class declaration");
            decl->isStatic = true;
        }

        (void)expect(TokenKind::LBrace, "expected '{' to start class body");
        while (!check(TokenKind::RBrace) && !atEnd())
        {
            auto annotations = parseAnnotations();
            const bool isPrivate = match(TokenKind::KwPrivate);
            if (match(TokenKind::KwExtern))
            {
                if (!annotations.empty())
                    core::throwDiagnostic(previous().location, "annotations can only be applied to functions and methods");
                parseClassExternMember(*decl, isPrivate);
                continue;
            }
            if (match(TokenKind::KwFn))
            {
                decl->methods.push_back(parseClassMethod(isPrivate, std::move(annotations)));
                continue;
            }
            if (check(TokenKind::KwNamespace))
                core::throwDiagnostic(current().location, "namespaces are not allowed inside classes");
            if (!annotations.empty())
                core::throwDiagnostic(current().location, "annotations can only be applied to methods inside classes");
            if (match(TokenKind::KwLet) || match(TokenKind::KwConst))
            {
                const bool isMutable = previous().kind == TokenKind::KwLet;
                auto fields = parseClassFields(isPrivate, isMutable);
                for (auto &field : fields)
                    decl->fields.push_back(std::move(field));
                continue;
            }
            core::throwDiagnostic(current().location, "expected class field, method, or extern member");
        }
        (void)expect(TokenKind::RBrace, "expected '}' to close class body");
        return decl;
    }

    std::unique_ptr<ast::ClassDecl> Parser::parseGroupDecl()
    {
        auto decl = std::make_unique<ast::ClassDecl>();
        decl->location = previous().location;
        decl->name = expect(TokenKind::Identifier, "expected group name").lexeme;
        knownTypeNames_.insert(decl->name);
        decl->isGroup = true;

        (void)expect(TokenKind::LBrace, "expected '{' to start group body");
        while (!check(TokenKind::RBrace) && !atEnd())
        {
            decl->fields.push_back(parseGroupField());
            if (match(TokenKind::Comma))
            {
                continue;
            }
            if (!check(TokenKind::RBrace))
            {
                core::throwDiagnostic(current().location, "expected ',' or '}' after group field");
            }
        }
        (void)expect(TokenKind::RBrace, "expected '}' to close group body");
        return decl;
    }

    std::unique_ptr<ast::EnumDecl> Parser::parseEnumDecl()
    {
        auto decl = std::make_unique<ast::EnumDecl>();
        decl->location = previous().location;
        decl->name = expect(TokenKind::Identifier, "expected enum name").lexeme;
        (void)expect(TokenKind::LBrace, "expected '{' to start enum body");
        std::int64_t nextValue = 0;
        while (!check(TokenKind::RBrace) && !atEnd())
        {
            ast::EnumItem item;
            const auto &name = expect(TokenKind::Identifier, "expected enum item name");
            item.location = name.location;
            item.name = name.lexeme;
            item.value = nextValue;
            if (match(TokenKind::Assign))
            {
                bool negative = match(TokenKind::Minus);
                const auto &valueTok = expect(TokenKind::IntegerLiteral, "expected integer literal in enum value");
                item.value = static_cast<std::int64_t>(parseUnsignedIntegerLiteral(valueTok));
                if (negative) item.value = -item.value;
            }
            decl->items.push_back(item);
            nextValue = item.value + 1;
            if (!match(TokenKind::Comma))
                break;
        }
        (void)expect(TokenKind::RBrace, "expected '}' to close enum body");
        return decl;
    }

    std::unique_ptr<ast::GlobalVarDecl> Parser::parseGlobalVarDecl(bool isMutable, bool isExport)
    {
        std::vector<std::unique_ptr<ast::GlobalVarDecl>> decls;

        while (true)
        {
            const auto &name = expect(TokenKind::Identifier, "expected variable name");

            core::TypeRef type{core::BuiltinTypeKind::Unknown, ""};
            bool hasExplicitType = false;
            std::unique_ptr<ast::Expr> initializer;

            if (match(TokenKind::Colon))
            {
                hasExplicitType = true;
                type = parseType();
            }

            if (match(TokenKind::Assign))
            {
                initializer = parseExpression();
                if (dynamic_cast<ast::NullLiteralExpr *>(initializer.get()) != nullptr)
                    initializer.reset();
            }
            else if (!hasExplicitType)
            {
                core::throwDiagnostic(name.location, "variable declaration without type requires an initializer");
            }

            auto decl = std::make_unique<ast::GlobalVarDecl>();
            decl->location = name.location;
            decl->isMutable = isMutable;
            decl->isExport = isExport;
            decl->hasExplicitType = hasExplicitType;
            decl->name = name.lexeme;
            decl->type = type;
            decl->initializer = std::move(initializer);
            decls.push_back(std::move(decl));

            if (!match(TokenKind::Comma))
                break;
        }

        (void)expect(TokenKind::Semicolon, "expected ';' after variable declaration");

        auto first = std::move(decls.front());
        for (std::size_t i = 1; i < decls.size(); ++i)
            pendingDeclarations_.push_back(std::move(decls[i]));
        return first;
    }

    std::vector<ast::Parameter> Parser::parseParameterList()
    {
        std::vector<ast::Parameter> parameters;
        do
        {
            parameters.push_back(parseParameter());
        } while (match(TokenKind::Comma));
        return parameters;
    }

    ast::Parameter Parser::parseParameter()
    {
        const auto &name = (match(TokenKind::KwSelf) ? previous() : expect(TokenKind::Identifier, "expected parameter name"));

        ast::Parameter parameter;
        parameter.location = name.location;
        parameter.name = name.lexeme;

        if (check(TokenKind::LBracket))
        {
            core::throwDiagnostic(current().location, "parameter array syntax must be 'name: type[]' or 'name: type[]->let'");
        }

        (void)expect(TokenKind::Colon, "expected ':' after parameter name");

        if (match(TokenKind::Ellipsis))
        {
            parameter.isVariadic = true;
            parameter.type = core::TypeRef{core::BuiltinTypeKind::Unknown, ""};
            return parameter;
        }

        parameter.type = parseType();

        if (match(TokenKind::Arrow))
        {
            (void)expect(TokenKind::KwLet, "expected let after '->' in parameter properties");
            if (!parameter.type.isArray())
                core::throwDiagnostic(parameter.location, "parameter property '->let' requires an array parameter");
            parameter.isArrayLet = true;
        }

        return parameter;
    }

    core::TypeRef Parser::parseType()
    {
        if (match(TokenKind::Ellipsis))
        {
            return core::TypeRef{core::BuiltinTypeKind::Unknown, ""};
        }

        core::TypeRef base{};
        const TokenKind kind = current().kind;
        const auto builtin = typeFromToken(kind);
        if (builtin.kind != core::BuiltinTypeKind::Unknown)
        {
            (void)advance();
            base = builtin;
        }
        else if (check(TokenKind::Identifier) || check(TokenKind::KwSelf))
        {
            const auto name = current().lexeme;
            (void)advance();
            base.kind = core::BuiltinTypeKind::Class;
            base.name = name;
        }
        else
        {
            core::throwDiagnostic(current().location, "expected builtin type name");
        }

        if (match(TokenKind::LBracket))
        {
            core::TypeRef arrayType{};
            arrayType.kind = core::BuiltinTypeKind::Array;
            arrayType.elementKind = base.kind;
            arrayType.elementName = base.name;
            if (check(TokenKind::IntegerLiteral))
            {
                arrayType.hasArraySize = true;
                arrayType.arraySize = static_cast<std::size_t>(parseUnsignedIntegerLiteral(advance()));
            }
            (void)expect(TokenKind::RBracket, "expected ']' after array type");
            base = arrayType;
        }

        while (match(TokenKind::Star))
        {
            ++base.pointerDepth;
        }

        return base;
    }

    std::vector<ast::FieldDecl> Parser::parseClassFields(bool isPrivate, bool isMutable)
    {
        std::vector<ast::FieldDecl> fields;
        while (true)
        {
            ast::FieldDecl field;
            const auto &name = expect(TokenKind::Identifier, "expected field name");
            (void)expect(TokenKind::Colon, "expected : after field name");
            field.location = name.location;
            field.isPrivate = isPrivate;
            field.isMutable = isMutable;
            field.name = name.lexeme;
            field.type = parseType();
            if (match(TokenKind::Arrow))
                { auto __tmpInit = parseExpression(); if (dynamic_cast<ast::NullLiteralExpr *>(__tmpInit.get()) == nullptr) field.initializer = __tmpInit.release(); }
            fields.push_back(field);
            if (!match(TokenKind::Comma))
                break;
        }
        (void)expect(TokenKind::Semicolon, "expected ; after field");
        return fields;
    }

    ast::FieldDecl Parser::parseGroupField()
    {
        ast::FieldDecl field;
        const auto &name = expect(TokenKind::Identifier, "expected group field name");
        (void)expect(TokenKind::Colon, "expected ':' after group field name");
        field.location = name.location;
        field.isPrivate = false;
        field.name = name.lexeme;
        field.type = parseType();
        return field;
    }

    ast::MemberFunctionDecl Parser::parseClassMethod(bool isPrivate, std::vector<ast::Annotation> annotations)
    {
        ast::MemberFunctionDecl method;
        method.isPrivate = isPrivate;
        method.annotations = std::move(annotations);
        method.location = previous().location;
        method.name = expect(TokenKind::Identifier, "expected method name").lexeme;
        (void)expect(TokenKind::LParen, "expected ( after method name");
        if (!check(TokenKind::RParen))
        {
            method.parameters = parseParameterList();
        }
        (void)expect(TokenKind::RParen, "expected ) after parameter list");
        (void)expect(TokenKind::Colon, "expected : before return type");
        method.returnType = parseType();
        for (const auto &ann : method.annotations)
        {
            if (ann.name == "Namespace")
                core::throwDiagnostic(ann.location, "@Namespace has been removed; use namespace \"name\" { ... } instead");
        }
        method.body = parseBlock();
        return method;
    }

    void Parser::parseClassExternMember(ast::ClassDecl& decl, bool isPrivate)
    {
        const auto location = previous().location;

        std::string abiName = "dash";
        if (match(TokenKind::LParen))
        {
            const auto& abi = expect(TokenKind::StringLiteral, "expected ABI string in extern declaration");
            abiName = abi.lexeme;
            (void)expect(TokenKind::RParen, "expected ')' after extern ABI");
        }

        if (match(TokenKind::Colon))
        {
            ast::FieldDecl field;
            field.location = location;
            field.isPrivate = isPrivate;
            field.isMutable = true;
            field.isExtern = true;
            field.abi = abiName;
            field.type = parseType();

            bool sawArrayLet = false;
            if (match(TokenKind::Arrow))
            {
                (void)expect(TokenKind::KwLet, "expected let after '->' in extern field declaration");
                if (!field.type.isArray())
                    core::throwDiagnostic(current().location, "extern field property '->let' requires an array type");
                sawArrayLet = true;
            }

            field.name = expect(TokenKind::Identifier, "expected extern field name").lexeme;

            if (!sawArrayLet && match(TokenKind::Arrow))
            {
                (void)expect(TokenKind::KwLet, "expected let after '->' in extern field declaration");
                if (!field.type.isArray())
                    core::throwDiagnostic(current().location, "extern field property '->let' requires an array type");
                sawArrayLet = true;
            }

            field.isMutable = field.type.isArray() ? sawArrayLet : true;
            (void)expect(TokenKind::Semicolon, "expected ';' after extern field declaration");
            decl.fields.push_back(std::move(field));
            return;
        }

        ast::MemberFunctionDecl method;
        method.location = location;
        method.isPrivate = isPrivate;
        method.isExtern = true;
        method.abi = abiName;
        method.name = expect(TokenKind::Identifier, "expected extern method name").lexeme;
        (void)expect(TokenKind::LParen, "expected '(' after extern method name");
        if (!check(TokenKind::RParen))
            method.parameters = parseParameterList();
        (void)expect(TokenKind::RParen, "expected ')' after parameter list");
        (void)expect(TokenKind::Colon, "expected ':' before return type");
        method.returnType = parseType();
        (void)expect(TokenKind::Semicolon, "expected ';' after extern method declaration");
        decl.methods.push_back(std::move(method));
    }

    std::unique_ptr<ast::BlockStmt> Parser::parseBlock()
    {
        const auto &open = expect(TokenKind::LBrace, "expected '{' to start block");
        auto block = std::make_unique<ast::BlockStmt>();
        block->location = open.location;

        while (!check(TokenKind::RBrace) && !atEnd())
        {
            block->statements.push_back(parseStatement());
        }

        (void)expect(TokenKind::RBrace, "expected '}' to close block");
        return block;
    }

    std::unique_ptr<ast::Stmt> Parser::parseStatement()
    {
        if (match(TokenKind::KwConst))
        {
            return parseVariableDecl(false);
        }
        if (match(TokenKind::KwLet))
        {
            return parseVariableDecl(true);
        }
        if (match(TokenKind::KwReturn))
        {
            return parseReturnStmt();
        }
        if (match(TokenKind::KwBreak))
        {
            return parseBreakStmt();
        }
        if (match(TokenKind::KwIf))
        {
            return parseIfStmt();
        }
        if (match(TokenKind::KwWhile))
        {
            return parseWhileStmt();
        }
        if (match(TokenKind::KwFor))
        {
            return parseForStmt();
        }
        if (match(TokenKind::KwDo))
        {
            return parseDoWhileStmt();
        }
        if (match(TokenKind::KwSwitch))
        {
            return parseSwitchStmt();
        }
        if (match(TokenKind::KwMatch))
        {
            return parseMatchStmt();
        }
        if (check(TokenKind::LBrace))
        {
            return parseBlock();
        }
        if (check(TokenKind::Identifier) || check(TokenKind::KwSelf))
        {
            return parseIdentifierLedStatement();
        }
        return parseExpressionStatement();
    }

    std::unique_ptr<ast::Stmt> Parser::parseIfStmt()
    {
        auto wrapStatementAsBlock = [&](std::unique_ptr<ast::Stmt> bodyStmt) {
            auto block = std::make_unique<ast::BlockStmt>();
            block->location = bodyStmt->location;
            block->statements.push_back(std::move(bodyStmt));
            return block;
        };

        auto stmt = std::make_unique<ast::IfStmt>();
        stmt->location = previous().location;
        (void)expect(TokenKind::LParen, "expected '(' after if");
        stmt->condition = parseExpression();
        (void)expect(TokenKind::RParen, "expected ')' after if condition");
        if (check(TokenKind::LBrace))
            stmt->thenBlock = parseBlock();
        else
            stmt->thenBlock = wrapStatementAsBlock(parseStatement());

        if (match(TokenKind::KwElse))
        {
            if (match(TokenKind::KwIf))
            {
                stmt->elseBranch = parseIfStmt();
            }
            else if (check(TokenKind::LBrace))
            {
                stmt->elseBranch = parseBlock();
            }
            else
            {
                stmt->elseBranch = wrapStatementAsBlock(parseStatement());
            }
        }

        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseWhileStmt()
    {
        auto stmt = std::make_unique<ast::WhileStmt>();
        stmt->location = previous().location;
        (void)expect(TokenKind::LParen, "expected '(' after while");
        stmt->condition = parseExpression();
        (void)expect(TokenKind::RParen, "expected ')' after while condition");
        stmt->body = parseBlock();
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseForStmt()
    {
        auto stmt = std::make_unique<ast::ForStmt>();
        stmt->location = previous().location;
        (void)expect(TokenKind::LParen, "expected '(' after for");

        if (!check(TokenKind::Semicolon))
        {
            stmt->initializer = parseForInitializer();
        }
        (void)expect(TokenKind::Semicolon, "expected ';' after for initializer");

        if (!check(TokenKind::Semicolon))
        {
            stmt->condition = parseExpression();
        }
        (void)expect(TokenKind::Semicolon, "expected ';' after for condition");

        if (!check(TokenKind::RParen))
        {
            stmt->increment = parseForIncrement();
        }
        (void)expect(TokenKind::RParen, "expected ')' after for clauses");
        stmt->body = parseBlock();
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseDoWhileStmt()
    {
        auto stmt = std::make_unique<ast::DoWhileStmt>();
        stmt->location = previous().location;
        stmt->body = parseBlock();
        (void)expect(TokenKind::KwWhile, "expected while after do block");
        (void)expect(TokenKind::LParen, "expected '(' after while");
        stmt->condition = parseExpression();
        (void)expect(TokenKind::RParen, "expected ')' after do-while condition");
        (void)expect(TokenKind::Semicolon, "expected ';' after do-while statement");
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseSwitchStmt()
    {
        const auto loc = previous().location;
        (void)expect(TokenKind::LParen, "expected '(' after switch");
        auto valueExpr = parseExpression();
        (void)expect(TokenKind::RParen, "expected ')' after switch expression");
        (void)expect(TokenKind::LBrace, "expected '{' after switch");

        static std::size_t switchCounter = 0;
        const std::string tempName = "__switch_value_" + std::to_string(switchCounter++);

        auto root = std::make_unique<ast::BlockStmt>();
        root->location = loc;
        auto tempDecl = std::make_unique<ast::VariableDeclStmt>();
        tempDecl->location = valueExpr->location;
        tempDecl->isMutable = false;
        tempDecl->name = tempName;
        tempDecl->initializer = std::move(valueExpr);
        root->statements.push_back(std::move(tempDecl));

        std::vector<std::pair<std::unique_ptr<ast::Expr>, std::unique_ptr<ast::BlockStmt>>> cases;
        std::unique_ptr<ast::BlockStmt> defaultBlock;
        while (!check(TokenKind::RBrace) && !atEnd())
        {
            if (match(TokenKind::KwCase))
            {
                auto caseExpr = parseExpression();
                (void)expect(TokenKind::Colon, "expected ':' after case value");
                auto block = std::make_unique<ast::BlockStmt>();
                block->location = previous().location;
                while (!check(TokenKind::KwCase) && !check(TokenKind::KwDefault) && !check(TokenKind::RBrace) && !atEnd())
                    block->statements.push_back(parseStatement());
                cases.push_back({std::move(caseExpr), std::move(block)});
                continue;
            }
            if (match(TokenKind::KwDefault))
            {
                (void)expect(TokenKind::Colon, "expected ':' after default");
                defaultBlock = std::make_unique<ast::BlockStmt>();
                defaultBlock->location = previous().location;
                while (!check(TokenKind::RBrace) && !atEnd())
                    defaultBlock->statements.push_back(parseStatement());
                break;
            }
            core::throwDiagnostic(current().location, "expected case or default in switch");
        }
        (void)expect(TokenKind::RBrace, "expected '}' after switch body");

        std::unique_ptr<ast::Stmt> tail = std::move(defaultBlock);
        for (auto it = cases.rbegin(); it != cases.rend(); ++it)
        {
            auto cond = std::make_unique<ast::BinaryExpr>();
            cond->location = it->first->location;
            cond->op = "==";
            auto lhs = std::make_unique<ast::VariableExpr>();
            lhs->location = loc;
            lhs->name = tempName;
            cond->left = std::move(lhs);
            cond->right = std::move(it->first);
            auto ifStmt = std::make_unique<ast::IfStmt>();
            ifStmt->location = cond->location;
            ifStmt->condition = std::move(cond);
            ifStmt->thenBlock = std::move(it->second);
            ifStmt->elseBranch = std::move(tail);
            tail = std::move(ifStmt);
        }
        if (tail)
            root->statements.push_back(std::move(tail));

        auto stmt = std::make_unique<ast::SwitchStmt>();
        stmt->location = loc;
        stmt->lowered = std::move(root);
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseMatchStmt()
    {
        const auto loc = previous().location;
        auto valueExpr = parseExpression();
        (void)expect(TokenKind::LBrace, "expected '{' after match expression");

        static std::size_t matchCounter = 0;
        const std::string tempName = "__match_value_" + std::to_string(matchCounter);
        const std::string doneName = "__match_done_" + std::to_string(matchCounter++);

        auto root = std::make_unique<ast::BlockStmt>();
        root->location = loc;

        auto tempDecl = std::make_unique<ast::VariableDeclStmt>();
        tempDecl->location = valueExpr->location;
        tempDecl->isMutable = false;
        tempDecl->name = tempName;
        tempDecl->initializer = std::move(valueExpr);
        root->statements.push_back(std::move(tempDecl));

        auto doneDecl = std::make_unique<ast::VariableDeclStmt>();
        doneDecl->location = loc;
        doneDecl->isMutable = true;
        doneDecl->name = doneName;
        doneDecl->type = core::TypeRef{core::BuiltinTypeKind::Bool, ""};
        auto falseExpr = std::make_unique<ast::BoolLiteralExpr>();
        falseExpr->location = loc;
        falseExpr->value = false;
        doneDecl->initializer = std::move(falseExpr);
        root->statements.push_back(std::move(doneDecl));

        std::unique_ptr<ast::BlockStmt> defaultBlock;
        while (!check(TokenKind::RBrace) && !atEnd())
        {
            bool wildcard = false;
            bool isRange = false;
            std::unique_ptr<ast::Expr> pattern;
            std::unique_ptr<ast::Expr> rangeEnd;

            if (check(TokenKind::Identifier) && current().lexeme == "_")
            {
                wildcard = true;
                (void)advance();
            }
            else
            {
                pattern = parseExpression();
                if (match(TokenKind::DotDot))
                {
                    rangeEnd = parseExpression();
                    isRange = true;
                }
            }

            (void)expect(TokenKind::Arrow, "expected '->' in match arm");

            auto armBody = std::make_unique<ast::BlockStmt>();
            armBody->location = previous().location;
            if (check(TokenKind::LBrace))
            {
                armBody = parseBlock();
            }
            else if (check(TokenKind::Identifier) || check(TokenKind::KwSelf))
            {
                armBody->statements.push_back(parseIdentifierLedStatement(false));
            }
            else
            {
                armBody->statements.push_back(parseExpressionStatement(false));
            }
            (void)match(TokenKind::Comma);

            if (wildcard)
            {
                defaultBlock = std::move(armBody);
                continue;
            }

            auto notDoneCond = std::make_unique<ast::BinaryExpr>();
            notDoneCond->location = loc;
            notDoneCond->op = "==";
            auto doneVarLhs = std::make_unique<ast::VariableExpr>();
            doneVarLhs->location = loc;
            doneVarLhs->name = doneName;
            auto doneFalse = std::make_unique<ast::BoolLiteralExpr>();
            doneFalse->location = loc;
            doneFalse->value = false;
            notDoneCond->left = std::move(doneVarLhs);
            notDoneCond->right = std::move(doneFalse);

            auto outerIf = std::make_unique<ast::IfStmt>();
            outerIf->location = loc;
            outerIf->condition = std::move(notDoneCond);
            outerIf->thenBlock = std::make_unique<ast::BlockStmt>();
            outerIf->thenBlock->location = loc;

            auto tempRefA = std::make_unique<ast::VariableExpr>();
            tempRefA->location = loc;
            tempRefA->name = tempName;

            auto cond = std::make_unique<ast::BinaryExpr>();
            cond->location = pattern->location;
            cond->op = isRange ? ">=" : "==";
            cond->left = std::move(tempRefA);
            cond->right = std::move(pattern);

            if (isRange)
            {
                auto lowerIf = std::make_unique<ast::IfStmt>();
                lowerIf->location = cond->location;
                lowerIf->condition = std::move(cond);
                lowerIf->thenBlock = std::make_unique<ast::BlockStmt>();
                lowerIf->thenBlock->location = loc;

                auto upperCond = std::make_unique<ast::BinaryExpr>();
                upperCond->location = rangeEnd->location;
                upperCond->op = "<=";
                auto tempRefB = std::make_unique<ast::VariableExpr>();
                tempRefB->location = loc;
                tempRefB->name = tempName;
                upperCond->left = std::move(tempRefB);
                upperCond->right = std::move(rangeEnd);

                auto upperIf = std::make_unique<ast::IfStmt>();
                upperIf->location = upperCond->location;
                upperIf->condition = std::move(upperCond);
                upperIf->thenBlock = std::move(armBody);

                auto doneAssign = std::make_unique<ast::AssignmentStmt>();
                doneAssign->location = loc;
                doneAssign->name = doneName;
                auto doneTrue = std::make_unique<ast::BoolLiteralExpr>();
                doneTrue->location = loc;
                doneTrue->value = true;
                doneAssign->value = std::move(doneTrue);
                upperIf->thenBlock->statements.push_back(std::move(doneAssign));

                lowerIf->thenBlock->statements.push_back(std::move(upperIf));
                outerIf->thenBlock->statements.push_back(std::move(lowerIf));
            }
            else
            {
                auto valueIf = std::make_unique<ast::IfStmt>();
                valueIf->location = cond->location;
                valueIf->condition = std::move(cond);
                valueIf->thenBlock = std::move(armBody);

                auto doneAssign = std::make_unique<ast::AssignmentStmt>();
                doneAssign->location = loc;
                doneAssign->name = doneName;
                auto doneTrue = std::make_unique<ast::BoolLiteralExpr>();
                doneTrue->location = loc;
                doneTrue->value = true;
                doneAssign->value = std::move(doneTrue);
                valueIf->thenBlock->statements.push_back(std::move(doneAssign));

                outerIf->thenBlock->statements.push_back(std::move(valueIf));
            }

            root->statements.push_back(std::move(outerIf));
        }
        (void)expect(TokenKind::RBrace, "expected '}' after match body");

        if (defaultBlock != nullptr)
        {
            auto notDoneCond = std::make_unique<ast::BinaryExpr>();
            notDoneCond->location = loc;
            notDoneCond->op = "==";
            auto doneVarLhs = std::make_unique<ast::VariableExpr>();
            doneVarLhs->location = loc;
            doneVarLhs->name = doneName;
            auto doneFalse = std::make_unique<ast::BoolLiteralExpr>();
            doneFalse->location = loc;
            doneFalse->value = false;
            notDoneCond->left = std::move(doneVarLhs);
            notDoneCond->right = std::move(doneFalse);

            auto defaultIf = std::make_unique<ast::IfStmt>();
            defaultIf->location = loc;
            defaultIf->condition = std::move(notDoneCond);
            defaultIf->thenBlock = std::move(defaultBlock);
            root->statements.push_back(std::move(defaultIf));
        }

        auto stmt = std::make_unique<ast::MatchStmt>();
        stmt->location = loc;
        stmt->lowered = std::move(root);
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseVariableDecl(bool isMutable, bool expectSemicolon)
    {
        std::vector<std::unique_ptr<ast::Stmt>> decls;

        while (true)
        {
            const auto &name = expect(TokenKind::Identifier, "expected variable name");

            core::TypeRef type{core::BuiltinTypeKind::Unknown, ""};
            bool hasExplicitType = false;
            std::unique_ptr<ast::Expr> initializer;

            if (match(TokenKind::Colon))
            {
                hasExplicitType = true;
                type = parseType();
            }

            if (match(TokenKind::Assign))
            {
                initializer = parseExpression();
                if (dynamic_cast<ast::NullLiteralExpr *>(initializer.get()) != nullptr)
                    initializer.reset();
            }
            else if (!hasExplicitType)
            {
                core::throwDiagnostic(name.location, "variable declaration without type requires an initializer");
            }

            auto stmt = std::make_unique<ast::VariableDeclStmt>();
            stmt->location = name.location;
            stmt->isMutable = isMutable;
            stmt->hasExplicitType = hasExplicitType;
            stmt->name = name.lexeme;
            stmt->type = type;
            stmt->initializer = std::move(initializer);
            decls.push_back(std::move(stmt));

            if (!match(TokenKind::Comma))
                break;
        }

        if (expectSemicolon)
        {
            (void)expect(TokenKind::Semicolon, "expected ';' after variable declaration");
        }

        if (decls.size() == 1)
            return std::move(decls.front());

        auto group = std::make_unique<ast::DeclGroupStmt>();
        group->location = decls.front()->location;
        group->statements = std::move(decls);
        return group;
    }

    std::unique_ptr<ast::Stmt> Parser::parseReturnStmt()
    {
        auto stmt = std::make_unique<ast::ReturnStmt>();
        stmt->location = previous().location;

        if (!check(TokenKind::Semicolon))
        {
            stmt->value = parseExpression();
        }

        (void)expect(TokenKind::Semicolon, "expected ';' after return statement");
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseBreakStmt()
    {
        auto stmt = std::make_unique<ast::BreakStmt>();
        stmt->location = previous().location;
        (void)expect(TokenKind::Semicolon, "expected ';' after break");
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseIdentifierLedStatement(bool expectSemicolon)
    {
        const auto start = index_;
        auto expr = parseExpression();
        if (match(TokenKind::Assign) || match(TokenKind::PlusAssign) || match(TokenKind::MinusAssign))
        {
            const auto assignKind = previous().kind;
            auto rhs = parseExpression();
            if (auto *var = dynamic_cast<ast::VariableExpr *>(expr.get()))
            {
                auto stmt = std::make_unique<ast::AssignmentStmt>();
                stmt->location = var->location;
                stmt->name = var->name;
                if (assignKind == TokenKind::Assign)
                {
                    stmt->value = std::move(rhs);
                }
                else
                {
                    auto bin = std::make_unique<ast::BinaryExpr>();
                    bin->location = var->location;
                    bin->op = assignKind == TokenKind::PlusAssign ? "+" : "-";
                    bin->left = cloneExprForCompoundAssignment(*expr);
                    bin->right = std::move(rhs);
                    stmt->value = std::move(bin);
                }
                if (expectSemicolon)
                    (void)expect(TokenKind::Semicolon, "expected ';' after assignment");
                return stmt;
            }
            if (auto *member = dynamic_cast<ast::MemberExpr *>(expr.get()))
            {
                auto stmt = std::make_unique<ast::MemberAssignmentStmt>();
                stmt->location = member->location;
                stmt->object = std::move(member->object);
                stmt->member = member->member;
                if (assignKind == TokenKind::Assign)
                {
                    stmt->value = std::move(rhs);
                }
                else
                {
                    auto bin = std::make_unique<ast::BinaryExpr>();
                    bin->location = stmt->location;
                    bin->op = assignKind == TokenKind::PlusAssign ? "+" : "-";
                    auto lhs = std::make_unique<ast::MemberExpr>();
                    lhs->location = stmt->location;
                    lhs->object = cloneExprForCompoundAssignment(*stmt->object);
                    lhs->member = stmt->member;
                    bin->left = std::move(lhs);
                    bin->right = std::move(rhs);
                    stmt->value = std::move(bin);
                }
                if (expectSemicolon)
                    (void)expect(TokenKind::Semicolon, "expected ';' after assignment");
                return stmt;
            }
            core::throwDiagnostic(previous().location, "invalid assignment target");
        }
        if (auto *var = dynamic_cast<ast::VariableExpr *>(expr.get()))
        {
            if (match(TokenKind::PlusPlus) || match(TokenKind::MinusMinus))
            {
                auto stmt = std::make_unique<ast::AssignmentStmt>();
                stmt->location = var->location;
                stmt->name = var->name;
                auto one = std::make_unique<ast::IntegerLiteralExpr>();
                one->location = var->location;
                one->value = 1;
                auto lhs = std::make_unique<ast::VariableExpr>();
                lhs->location = var->location;
                lhs->name = var->name;
                auto bin = std::make_unique<ast::BinaryExpr>();
                bin->location = var->location;
                bin->op = previous().kind == TokenKind::PlusPlus ? "+" : "-";
                bin->left = std::move(lhs);
                bin->right = std::move(one);
                stmt->value = std::move(bin);
                if (expectSemicolon)
                    (void)expect(TokenKind::Semicolon, "expected ';' after increment/decrement");
                return stmt;
            }
        }
        if (expectSemicolon)
            (void)expect(TokenKind::Semicolon, "expected ';' after expression");
        auto stmt = std::make_unique<ast::ExprStmt>();
        stmt->location = tokens_[start].location;
        stmt->expr = std::move(expr);
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseExpressionStatement(bool expectSemicolon)
    {
        const auto start = index_;
        auto expr = parseExpression();
        if (match(TokenKind::Assign) || match(TokenKind::PlusAssign) || match(TokenKind::MinusAssign))
        {
            const auto assignKind = previous().kind;
            if (auto *unary = dynamic_cast<ast::UnaryExpr *>(expr.get()))
            {
                if (unary->op == '*')
                {
                    auto stmt = std::make_unique<ast::DerefAssignmentStmt>();
                    stmt->location = unary->location;
                    stmt->pointer = std::move(unary->operand);
                    auto rhs = parseExpression();
                    if (assignKind == TokenKind::Assign)
                    {
                        stmt->value = std::move(rhs);
                    }
                    else
                    {
                        auto bin = std::make_unique<ast::BinaryExpr>();
                        bin->location = stmt->location;
                        bin->op = assignKind == TokenKind::PlusAssign ? "+" : "-";
                        auto lhs = std::make_unique<ast::UnaryExpr>();
                        lhs->location = stmt->location;
                        lhs->op = '*';
                        lhs->operand = cloneExprForCompoundAssignment(*stmt->pointer);
                        bin->left = std::move(lhs);
                        bin->right = std::move(rhs);
                        stmt->value = std::move(bin);
                    }
                    if (expectSemicolon)
                    {
                        (void)expect(TokenKind::Semicolon, "expected ';' after assignment");
                    }
                    return stmt;
                }
            }
            core::throwDiagnostic(previous().location, "invalid assignment target");
        }
        auto stmt = std::make_unique<ast::ExprStmt>();
        stmt->location = tokens_[start].location;
        stmt->expr = std::move(expr);
        if (expectSemicolon)
        {
            (void)expect(TokenKind::Semicolon, "expected ';' after expression");
        }
        return stmt;
    }

    std::unique_ptr<ast::Stmt> Parser::parseForInitializer()
    {
        if (match(TokenKind::KwConst))
        {
            return parseVariableDecl(false, false);
        }
        if (match(TokenKind::KwLet))
        {
            return parseVariableDecl(true, false);
        }
        if (check(TokenKind::Identifier) || check(TokenKind::KwSelf))
        {
            return parseIdentifierLedStatement(false);
        }
        return parseExpressionStatement(false);
    }

    std::unique_ptr<ast::Stmt> Parser::parseForIncrement()
    {
        if (check(TokenKind::Identifier) || check(TokenKind::KwSelf))
        {
            return parseIdentifierLedStatement(false);
        }
        return parseExpressionStatement(false);
    }

    std::unique_ptr<ast::Expr> Parser::parseExpression()
    {
        return parseConditional();
    }

    std::unique_ptr<ast::Expr> Parser::parseConditional()
    {
        auto expr = parseLogicalOr();

        if (match(TokenKind::Question))
        {
            auto ternary = std::make_unique<ast::TernaryExpr>();
            ternary->location = expr->location;
            ternary->condition = std::move(expr);
            ternary->thenExpr = parseExpression();
            (void)expect(TokenKind::Colon, "expected ':' in ternary expression");
            ternary->elseExpr = parseConditional();
            return ternary;
        }

        return expr;
    }

    std::unique_ptr<ast::Expr> Parser::parseLogicalOr()
    {
        auto expr = parseLogicalAnd();

        while (match(TokenKind::PipePipe))
        {
            const auto op = previous();
            auto right = parseEquality();
            auto binary = std::make_unique<ast::BinaryExpr>();
            binary->location = op.location;
            binary->op = op.lexeme;
            binary->left = std::move(expr);
            binary->right = std::move(right);
            expr = std::move(binary);
        }

        return expr;
    }


    std::unique_ptr<ast::Expr> Parser::parseLogicalAnd()
    {
        auto expr = parseEquality();

        while (match(TokenKind::AmpAmp))
        {
            const auto op = previous();
            auto right = parseEquality();
            auto binary = std::make_unique<ast::BinaryExpr>();
            binary->location = op.location;
            binary->op = op.lexeme;
            binary->left = std::move(expr);
            binary->right = std::move(right);
            expr = std::move(binary);
        }

        return expr;
    }

    std::unique_ptr<ast::Expr> Parser::parseEquality()
    {
        auto expr = parseComparison();

        while (match(TokenKind::EqualEqual) || match(TokenKind::BangEqual))
        {
            const auto op = previous();
            auto right = parseComparison();
            auto binary = std::make_unique<ast::BinaryExpr>();
            binary->location = op.location;
            binary->op = op.lexeme;
            binary->left = std::move(expr);
            binary->right = std::move(right);
            expr = std::move(binary);
        }

        return expr;
    }

    std::unique_ptr<ast::Expr> Parser::parseComparison()
    {
        auto expr = parseAdditive();

        while (match(TokenKind::Less) || match(TokenKind::LessEqual) || match(TokenKind::Greater) || match(TokenKind::GreaterEqual))
        {
            const auto op = previous();
            auto right = parseAdditive();
            auto binary = std::make_unique<ast::BinaryExpr>();
            binary->location = op.location;
            binary->op = op.lexeme;
            binary->left = std::move(expr);
            binary->right = std::move(right);
            expr = std::move(binary);
        }

        return expr;
    }

    std::unique_ptr<ast::Expr> Parser::parseAdditive()
    {
        auto expr = parseMultiplicative();

        while (match(TokenKind::Plus) || match(TokenKind::Minus))
        {
            const auto op = previous();
            auto right = parseMultiplicative();
            auto binary = std::make_unique<ast::BinaryExpr>();
            binary->location = op.location;
            binary->op = op.lexeme;
            binary->left = std::move(expr);
            binary->right = std::move(right);
            expr = std::move(binary);
        }

        return expr;
    }

    std::unique_ptr<ast::Expr> Parser::parseMultiplicative()
    {
        auto expr = parsePower();

        while (match(TokenKind::Star) || match(TokenKind::Slash) || match(TokenKind::Percent) || match(TokenKind::ShiftLeft) || match(TokenKind::ShiftRight))
        {
            const auto op = previous();
            auto right = parsePower();
            auto binary = std::make_unique<ast::BinaryExpr>();
            binary->location = op.location;
            binary->op = op.lexeme;
            binary->left = std::move(expr);
            binary->right = std::move(right);
            expr = std::move(binary);
        }

        return expr;
    }

    std::unique_ptr<ast::Expr> Parser::parsePower()
    {
        auto expr = parseUnary();

        if (match(TokenKind::Caret))
        {
            const auto op = previous();
            auto right = parsePower();
            auto binary = std::make_unique<ast::BinaryExpr>();
            binary->location = op.location;
            binary->op = op.lexeme;
            binary->left = std::move(expr);
            binary->right = std::move(right);
            expr = std::move(binary);
        }

        return expr;
    }

    std::unique_ptr<ast::Expr> Parser::parseUnary()
    {
        if (match(TokenKind::Minus) || match(TokenKind::Ampersand) || match(TokenKind::Bang) || match(TokenKind::Star))
        {
            const auto op = previous();
            auto expr = std::make_unique<ast::UnaryExpr>();
            expr->location = op.location;
            expr->op = op.lexeme[0];
            expr->operand = parseUnary();
            return expr;
        }
        return parsePrimary();
    }

    std::unique_ptr<ast::Expr> Parser::parsePrimary()
    {
        std::unique_ptr<ast::Expr> expr;

        if (match(TokenKind::IntegerLiteral))
        {
            auto node = std::make_unique<ast::IntegerLiteralExpr>();
            node->location = previous().location;
            node->value = parseUnsignedIntegerLiteral(previous());
            node->forceUnsigned = previous().lexeme.rfind("0x", 0) == 0 || previous().lexeme.rfind("0X", 0) == 0 || previous().lexeme.rfind("F0x", 0) == 0 || previous().lexeme.rfind("f0x", 0) == 0;
            expr = std::move(node);
        }
        else if (match(TokenKind::FloatingLiteral))
        {
            auto node = std::make_unique<ast::DoubleLiteralExpr>();
            node->location = previous().location;
            std::string text = previous().lexeme;
            if (!text.empty() && (text.back() == 'f' || text.back() == 'F'))
            {
                text.pop_back();
            }
            node->value = std::strtod(text.c_str(), nullptr);
            expr = std::move(node);
        }
        else if (match(TokenKind::StringLiteral) || match(TokenKind::InterpolatedStringLiteral))
        {
            expr = parseStringLiteralToken(previous());
        }
        else if (match(TokenKind::CharLiteral))
        {
            auto node = std::make_unique<ast::CharLiteralExpr>();
            node->location = previous().location;
            node->value = previous().lexeme.empty() ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(previous().lexeme[0]);
            expr = std::move(node);
        }
        else if (match(TokenKind::KwTrue) || match(TokenKind::KwFalse))
        {
            auto node = std::make_unique<ast::BoolLiteralExpr>();
            node->location = previous().location;
            node->value = previous().kind == TokenKind::KwTrue;
            expr = std::move(node);
        }
        else if (match(TokenKind::KwNull))
        {
            auto node = std::make_unique<ast::NullLiteralExpr>();
            node->location = previous().location;
            expr = std::move(node);
        }
        else if (match(TokenKind::KwIs))
        {
            const auto loc = previous().location;
            (void)expect(TokenKind::Less, "expected '<' after is");
            auto type = parseType();
            (void)expect(TokenKind::Comma, "expected ',' in is expression");
            const auto &name = (match(TokenKind::KwSelf) ? previous() : expect(TokenKind::Identifier, "expected variable name"));
            (void)expect(TokenKind::Greater, "expected '>' after is expression");
            auto node = std::make_unique<ast::IsTypeExpr>();
            node->location = loc;
            node->type = type;
            node->variable = name.lexeme;
            expr = std::move(node);
        }
        else if (check(TokenKind::Identifier) && current().lexeme == "env" && (index_ + 1) < tokens_.size() && tokens_[index_ + 1].kind == TokenKind::Less)
        {
            const auto loc = current().location;
            (void)advance();
            (void)expect(TokenKind::Less, "expected '<' after env");
            const auto &domain = expect(TokenKind::StringLiteral, "expected env domain string");
            (void)expect(TokenKind::Comma, "expected ',' in env expression");
            const auto &value = expect(TokenKind::StringLiteral, "expected env value string");
            (void)expect(TokenKind::Greater, "expected '>' after env expression");

            auto node = std::make_unique<ast::IntegerLiteralExpr>();
            node->location = loc;
            node->forceUnsigned = false;
            if (domain.lexeme == "target")
                node->value = envTargetMatches(value.lexeme) ? 1u : 0u;
            else
                core::throwDiagnostic(domain.location, "unknown env domain: " + domain.lexeme);
            expr = std::move(node);
        }
        else if (check(TokenKind::Identifier) && current().lexeme == "exdt" && (index_ + 1) < tokens_.size() && tokens_[index_ + 1].kind == TokenKind::Less)
        {
            const auto loc = current().location;
            (void)advance();
            (void)expect(TokenKind::Less, "expected '<' after exdt");
            auto node = std::make_unique<ast::ExtractDataExpr>();
            node->location = loc;
            node->operand = parseUnary();
            (void)expect(TokenKind::Greater, "expected '>' after exdt expression");
            expr = std::move(node);
        }
        else if (isCastTypeStart())
        {
            const auto savedIndex = index_;
            const auto savedToken = current();
            auto targetType = parseType();
            if (match(TokenKind::Less))
            {
                auto node = std::make_unique<ast::CastExpr>();
                node->location = savedToken.location;
                node->targetType = targetType;
                node->operand = parseUnary();
                (void)expect(TokenKind::Greater, "expected '>' after cast expression");
                expr = std::move(node);
            }
            else
            {
                index_ = savedIndex;
                auto node = std::make_unique<ast::VariableExpr>();
                node->location = advance().location;
                node->name = previous().lexeme;
                expr = std::move(node);
            }
        }
        else if (match(TokenKind::Identifier) || match(TokenKind::KwSelf))
        {
            auto node = std::make_unique<ast::VariableExpr>();
            node->location = previous().location;
            node->name = previous().lexeme;
            expr = std::move(node);
        }
        else if (match(TokenKind::LParen))
        {
            if ((check(TokenKind::Identifier) || check(TokenKind::KwSelf))
                && (index_ + 1) < tokens_.size()
                && tokens_[index_ + 1].kind == TokenKind::RParen)
            {
                auto var = std::make_unique<ast::VariableExpr>();
                var->location = current().location;
                var->name = current().lexeme;
                (void)advance();
                (void)expect(TokenKind::RParen, "expected ')' after parenthesized expression");
                auto paren = std::make_unique<ast::ParenExpr>();
                paren->location = var->location;
                paren->operand = std::move(var);
                paren->preferVariadicCount = true;
                expr = std::move(paren);
            }
            else
            {
                expr = parseExpression();
                (void)expect(TokenKind::RParen, "expected ')' after parenthesized expression");
            }
        }
        else if (match(TokenKind::LBrace))
        {
            auto array = std::make_unique<ast::ArrayLiteralExpr>();
            array->location = previous().location;
            if (!check(TokenKind::RBrace))
            {
                do
                {
                    array->elements.push_back(parseExpression());
                } while (match(TokenKind::Comma));
            }
            (void)expect(TokenKind::RBrace, "expected '}' after array literal");
            expr = std::move(array);
        }
        else
        {
            core::throwDiagnostic(current().location, "expected expression");
        }

        while (true)
        {
            if (check(TokenKind::Colon) && (index_ + 1) < tokens_.size() && tokens_[index_ + 1].kind == TokenKind::Colon)
            {
                (void)advance();
                (void)expect(TokenKind::Colon, "expected second ':'");
                const auto &field = expect(TokenKind::Identifier, "expected identifier after '::'");
                if (field.lexeme == "size")
                {
                    auto sizeExpr = std::make_unique<ast::SizeExpr>();
                    sizeExpr->location = field.location;
                    sizeExpr->object = std::move(expr);
                    expr = std::move(sizeExpr);
                    continue;
                }
                if (field.lexeme == "push")
                {
                    (void)expect(TokenKind::LParen, "expected '(' after ::push");
                    auto pushExpr = std::make_unique<ast::ArrayPushExpr>();
                    pushExpr->location = field.location;
                    pushExpr->array = std::move(expr);
                    pushExpr->value = parseExpression();
                    if (match(TokenKind::Comma))
                    {
                        pushExpr->index = parseExpression();
                    }
                    (void)expect(TokenKind::RParen, "expected ')' after ::push arguments");
                    expr = std::move(pushExpr);
                    continue;
                }
                if (field.lexeme == "insert")
                {
                    (void)expect(TokenKind::LParen, "expected '(' after ::insert");
                    auto insertExpr = std::make_unique<ast::ArrayInsertExpr>();
                    insertExpr->location = field.location;
                    insertExpr->array = std::move(expr);
                    insertExpr->index = parseExpression();
                    (void)expect(TokenKind::Comma, "expected ',' after ::insert index");
                    insertExpr->value = parseExpression();
                    (void)expect(TokenKind::RParen, "expected ')' after ::insert arguments");
                    expr = std::move(insertExpr);
                    continue;
                }
                if (field.lexeme == "set")
                {
                    (void)expect(TokenKind::LParen, "expected '(' after ::set");
                    auto setExpr = std::make_unique<ast::ArraySetExpr>();
                    setExpr->location = field.location;
                    setExpr->array = std::move(expr);
                    setExpr->index = parseExpression();
                    (void)expect(TokenKind::Comma, "expected ',' after ::set index");
                    setExpr->value = parseExpression();
                    (void)expect(TokenKind::RParen, "expected ')' after ::set arguments");
                    expr = std::move(setExpr);
                    continue;
                }
                if (field.lexeme == "rem")
                {
                    (void)expect(TokenKind::LParen, "expected '(' after ::rem");
                    auto remExpr = std::make_unique<ast::ArrayRemoveExpr>();
                    remExpr->location = field.location;
                    remExpr->array = std::move(expr);
                    remExpr->index = parseExpression();
                    (void)expect(TokenKind::RParen, "expected ')' after ::rem arguments");
                    expr = std::move(remExpr);
                    continue;
                }
                if (auto *var = dynamic_cast<ast::VariableExpr *>(expr.get()))
                {
                    const std::string qualified = var->name + "::" + field.lexeme;
                    if (match(TokenKind::LParen))
                    {
                        auto call = std::make_unique<ast::CallExpr>();
                        call->location = field.location;
                        call->callee = qualified;
                        if (!check(TokenKind::RParen))
                            call->arguments = parseArgumentList();
                        (void)expect(TokenKind::RParen, "expected ')' after call arguments");
                        expr = std::move(call);
                        continue;
                    }
                    auto namespaced = std::make_unique<ast::VariableExpr>();
                    namespaced->location = field.location;
                    namespaced->name = qualified;
                    expr = std::move(namespaced);
                    continue;
                }
                core::throwDiagnostic(field.location, "unknown '::' property or method");
            }
            if (match(TokenKind::LBracket))
            {
                auto indexExpr = parseExpression();
                (void)expect(TokenKind::RBracket, "expected ']' after index expression");
                auto indexed = std::make_unique<ast::IndexExpr>();
                indexed->location = expr->location;
                indexed->object = std::move(expr);
                indexed->index = std::move(indexExpr);
                expr = std::move(indexed);
                continue;
            }
            if (match(TokenKind::LParen))
            {
                auto *var = dynamic_cast<ast::VariableExpr *>(expr.get());
                if (var == nullptr)
                    core::throwDiagnostic(previous().location, "invalid call target");
                auto call = std::make_unique<ast::CallExpr>();
                call->location = var->location;
                call->callee = var->name;
                if (!check(TokenKind::RParen))
                    call->arguments = parseArgumentList();
                (void)expect(TokenKind::RParen, "expected ')' after call arguments");
                expr = std::move(call);
                continue;
            }
            if (match(TokenKind::Dot))
            {
                const auto &member = expect(TokenKind::Identifier, "expected member name after '.'");
                if (match(TokenKind::LParen))
                {
                    auto call = std::make_unique<ast::MethodCallExpr>();
                    call->location = member.location;
                    call->object = std::move(expr);
                    call->method = member.lexeme;
                    if (!check(TokenKind::RParen))
                        call->arguments = parseArgumentList();
                    (void)expect(TokenKind::RParen, "expected ')' after call arguments");
                    expr = std::move(call);
                }
                else
                {
                    auto access = std::make_unique<ast::MemberExpr>();
                    access->location = member.location;
                    access->object = std::move(expr);
                    access->member = member.lexeme;
                    expr = std::move(access);
                }
                continue;
            }
            break;
        }

        return expr;
    }

    bool Parser::isCastTypeStart() const noexcept
    {
        const auto kind = current().kind;
        const auto builtin = typeFromToken(kind);
        if (builtin.kind != core::BuiltinTypeKind::Unknown)
            return true;
        return check(TokenKind::Identifier) && knownTypeNames_.contains(current().lexeme);
    }

    std::vector<std::unique_ptr<ast::Expr>> Parser::parseArgumentList()
    {
        std::vector<std::unique_ptr<ast::Expr>> args;
        do
        {
            if (check(TokenKind::LParen)
                && (index_ + 2) < tokens_.size()
                && (tokens_[index_ + 1].kind == TokenKind::Identifier || tokens_[index_ + 1].kind == TokenKind::KwSelf)
                && tokens_[index_ + 2].kind == TokenKind::RParen)
            {
                const auto open = advance();
                const auto nameTok = advance();
                (void)expect(TokenKind::RParen, "expected ')' after variadic forward");
                auto forward = std::make_unique<ast::VariadicForwardExpr>(nameTok.lexeme);
                forward->location = open.location;
                args.push_back(std::move(forward));
            }
            else if ((check(TokenKind::Identifier) || check(TokenKind::KwSelf))
                     && (index_ + 1) < tokens_.size()
                     && tokens_[index_ + 1].kind == TokenKind::Ellipsis)
            {
                const auto nameTok = advance();
                (void)expect(TokenKind::Ellipsis, "expected '...' after variadic forward name");
                auto forward = std::make_unique<ast::VariadicForwardExpr>(nameTok.lexeme);
                forward->location = nameTok.location;
                args.push_back(std::move(forward));
            }
            else
            {
                args.push_back(parseExpression());
            }
        } while (match(TokenKind::Comma));
        return args;
    }

} // namespace dash::frontend

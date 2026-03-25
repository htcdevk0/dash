#include "dash/frontend/lexer.hpp"

#include <cctype>
#include <unordered_map>

#include "dash/core/diagnostic.hpp"

namespace dash::frontend
{

    namespace
    {

        [[nodiscard]] TokenKind keywordKind(const std::string &text)
        {
            static const std::unordered_map<std::string, TokenKind> keywords{
                {"extern", TokenKind::KwExtern},
                {"fn", TokenKind::KwFn},
                {"return", TokenKind::KwReturn},
                {"const", TokenKind::KwConst},
                {"let", TokenKind::KwLet},
                {"if", TokenKind::KwIf},
                {"else", TokenKind::KwElse},
                {"while", TokenKind::KwWhile},
                {"for", TokenKind::KwFor},
                {"do", TokenKind::KwDo},
                {"void", TokenKind::KwVoid},
                {"bool", TokenKind::KwBool},
                {"int", TokenKind::KwInt},
                {"uint", TokenKind::KwUInt},
                {"double", TokenKind::KwDouble},
                {"float", TokenKind::KwFloat},
                {"char", TokenKind::KwChar},
                {"string", TokenKind::KwString},
                {"true", TokenKind::KwTrue},
                {"false", TokenKind::KwFalse},
                {"private", TokenKind::KwPrivate},
                {"is", TokenKind::KwIs},
                {"class", TokenKind::KwClass},
                {"group", TokenKind::KwGroup},
                {"static", TokenKind::KwStatic},
                {"self", TokenKind::KwSelf},
                {"switch", TokenKind::KwSwitch},
                {"case", TokenKind::KwCase},
                {"default", TokenKind::KwDefault},
                {"match", TokenKind::KwMatch},
                {"enum", TokenKind::KwEnum},
                {"break", TokenKind::KwBreak},
                {"export", TokenKind::KwExport},
                {"null", TokenKind::KwNull},
                {"namespace", TokenKind::KwNamespace}};

            if (const auto it = keywords.find(text); it != keywords.end())
            {
                return it->second;
            }
            return TokenKind::Identifier;
        }

    } // namespace

    std::string tokenKindName(TokenKind kind)
    {
        switch (kind)
        {
        case TokenKind::EndOfFile:
            return "end of file";
        case TokenKind::Identifier:
            return "identifier";
        case TokenKind::IntegerLiteral:
            return "integer literal";
        case TokenKind::FloatingLiteral:
            return "floating literal";
        case TokenKind::StringLiteral:
            return "string literal";
        case TokenKind::InterpolatedStringLiteral:
            return "interpolated string literal";
        case TokenKind::CharLiteral:
            return "char literal";
        case TokenKind::KwExtern:
            return "extern";
        case TokenKind::KwFn:
            return "fn";
        case TokenKind::KwReturn:
            return "return";
        case TokenKind::KwConst:
            return "const";
        case TokenKind::KwLet:
            return "let";
        case TokenKind::KwIf:
            return "if";
        case TokenKind::KwElse:
            return "else";
        case TokenKind::KwWhile:
            return "while";
        case TokenKind::KwFor:
            return "for";
        case TokenKind::KwDo:
            return "do";
        case TokenKind::KwVoid:
            return "void";
        case TokenKind::KwBool:
            return "bool";
        case TokenKind::KwInt:
            return "int";
        case TokenKind::KwUInt:
            return "uint";
        case TokenKind::KwDouble:
            return "double";
        case TokenKind::KwFloat:
            return "float";
        case TokenKind::KwChar:
            return "char";
        case TokenKind::KwString:
            return "string";
        case TokenKind::KwTrue:
            return "true";
        case TokenKind::KwFalse:
            return "false";
        case TokenKind::KwPrivate:
            return "private";
        case TokenKind::KwIs:
            return "is";
        case TokenKind::KwClass:
            return "class";
        case TokenKind::KwGroup:
            return "group";
        case TokenKind::KwStatic:
            return "static";
        case TokenKind::KwSelf:
            return "self";
        case TokenKind::KwSwitch:
            return "switch";
        case TokenKind::KwCase:
            return "case";
        case TokenKind::KwDefault:
            return "default";
        case TokenKind::KwMatch:
            return "match";
        case TokenKind::KwEnum:
            return "enum";
        case TokenKind::KwBreak:
            return "break";
        case TokenKind::KwExport:
            return "export";
        case TokenKind::KwNull:
            return "null";
        case TokenKind::KwNamespace:
            return "namespace";
        case TokenKind::At:
            return "@";
        case TokenKind::LParen:
            return "(";
        case TokenKind::RParen:
            return ")";
        case TokenKind::LBrace:
            return "{";
        case TokenKind::RBrace:
            return "}";
        case TokenKind::LBracket:
            return "[";
        case TokenKind::RBracket:
            return "]";
        case TokenKind::Colon:
            return ":";
        case TokenKind::Semicolon:
            return ";";
        case TokenKind::Comma:
            return ",";
        case TokenKind::Question:
            return "?";
        case TokenKind::Dot:
            return ".";
        case TokenKind::DotDot:
            return "..";
        case TokenKind::Assign:
            return "=";
        case TokenKind::EqualEqual:
            return "==";
        case TokenKind::Bang:
            return "!";
        case TokenKind::BangEqual:
            return "!=";
        case TokenKind::Less:
            return "<";
        case TokenKind::LessEqual:
            return "<=";
        case TokenKind::Greater:
            return ">";
        case TokenKind::GreaterEqual:
            return ">=";
        case TokenKind::Plus:
            return "+";
        case TokenKind::PlusAssign:
            return "+=";
        case TokenKind::Minus:
            return "-";
        case TokenKind::MinusAssign:
            return "-=";
        case TokenKind::Arrow:
            return "->";
        case TokenKind::Star:
            return "*";
        case TokenKind::Slash:
            return "/";
        case TokenKind::Percent:
            return "%";
        case TokenKind::Caret:
            return "^";
        case TokenKind::ShiftLeft:
            return "<<";
        case TokenKind::ShiftRight:
            return ">>";
        case TokenKind::Ampersand:
            return "&";
        case TokenKind::AmpAmp:
            return "&&";
        case TokenKind::PipePipe:
            return "||";
        case TokenKind::Ellipsis:
            return "...";
        case TokenKind::PlusPlus:
            return "++";
        case TokenKind::MinusMinus:
            return "--";
        case TokenKind::Hash:
            return "#";
        }
        return "<unknown token>";
    }

    Lexer::Lexer(std::string filePath, std::string source)
        : filePath_(std::move(filePath)), source_(std::move(source)) {}

    std::vector<Token> Lexer::tokenize()
    {
        std::vector<Token> tokens;

        while (!atEnd())
        {
            skipWhitespaceAndComments();
            if (atEnd())
            {
                break;
            }

            const auto location = core::SourceLocation{filePath_, line_, column_};
            const char c = peek();

            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || ((c == 'F' || c == 'f') && peek(1) == '0' && (peek(2) == 'x' || peek(2) == 'X')))
            {
                if ((c == 'F' || c == 'f') && peek(1) == '0' && (peek(2) == 'x' || peek(2) == 'X'))
                {
                    tokens.push_back(lexNumber());
                }
                else
                {
                    tokens.push_back(lexIdentifierOrKeyword());
                }
                continue;
            }

            if (std::isdigit(static_cast<unsigned char>(c)))
            {
                tokens.push_back(lexNumber());
                continue;
            }

            if (c == '$' && peek(1) == '"')
            {
                tokens.push_back(lexString(true));
                continue;
            }

            if (c == '"')
            {
                tokens.push_back(lexString(false));
                continue;
            }

            if (c == '\'')
            {
                tokens.push_back(lexChar());
                continue;
            }

            switch (advance())
            {
            case '@':
                tokens.push_back(makeToken(TokenKind::At, "@", location));
                break;
            case '#':
                tokens.push_back(makeToken(TokenKind::Hash, "#", location));
                break;
            case '(':
                tokens.push_back(makeToken(TokenKind::LParen, "(", location));
                break;
            case ')':
                tokens.push_back(makeToken(TokenKind::RParen, ")", location));
                break;
            case '{':
                tokens.push_back(makeToken(TokenKind::LBrace, "{", location));
                break;
            case '}':
                tokens.push_back(makeToken(TokenKind::RBrace, "}", location));
                break;
            case '[':
                tokens.push_back(makeToken(TokenKind::LBracket, "[", location));
                break;
            case ']':
                tokens.push_back(makeToken(TokenKind::RBracket, "]", location));
                break;
            case ':':
                tokens.push_back(makeToken(TokenKind::Colon, ":", location));
                break;
            case ';':
                tokens.push_back(makeToken(TokenKind::Semicolon, ";", location));
                break;
            case ',':
                tokens.push_back(makeToken(TokenKind::Comma, ",", location));
                break;
            case '?':
                tokens.push_back(makeToken(TokenKind::Question, "?", location));
                break;
            case '.':
                if (match('.'))
                {
                    if (match('.'))
                    {
                        tokens.push_back(makeToken(TokenKind::Ellipsis, "...", location));
                    }
                    else
                    {
                        tokens.push_back(makeToken(TokenKind::DotDot, "..", location));
                    }
                }
                else
                {
                    tokens.push_back(makeToken(TokenKind::Dot, ".", location));
                }
                break;
            case '=':
                if (match('='))
                {
                    tokens.push_back(makeToken(TokenKind::EqualEqual, "==", location));
                }
                else
                {
                    tokens.push_back(makeToken(TokenKind::Assign, "=", location));
                }
                break;
            case '!':
                if (match('='))
                {
                    tokens.push_back(makeToken(TokenKind::BangEqual, "!=", location));
                }
                else
                {
                    tokens.push_back(makeToken(TokenKind::Bang, "!", location));
                }
                break;
            case '<':
                if (match('<'))
                {
                    tokens.push_back(makeToken(TokenKind::ShiftLeft, "<<", location));
                }
                else if (match('='))
                {
                    tokens.push_back(makeToken(TokenKind::LessEqual, "<=", location));
                }
                else
                {
                    tokens.push_back(makeToken(TokenKind::Less, "<", location));
                }
                break;
            case '>':
                if (match('>'))
                {
                    tokens.push_back(makeToken(TokenKind::ShiftRight, ">>", location));
                }
                else if (match('='))
                {
                    tokens.push_back(makeToken(TokenKind::GreaterEqual, ">=", location));
                }
                else
                {
                    tokens.push_back(makeToken(TokenKind::Greater, ">", location));
                }
                break;
            case '+':
                if (match('+'))
                {
                    tokens.push_back(makeToken(TokenKind::PlusPlus, "++", location));
                }
                else if (match('='))
                {
                    tokens.push_back(makeToken(TokenKind::PlusAssign, "+=", location));
                }
                else
                {
                    tokens.push_back(makeToken(TokenKind::Plus, "+", location));
                }
                break;

            case '-':
                if (match('-'))
                {
                    tokens.push_back(makeToken(TokenKind::MinusMinus, "--", location));
                }
                else if (match('='))
                {
                    tokens.push_back(makeToken(TokenKind::MinusAssign, "-=", location));
                }
                else if (match('>'))
                {
                    tokens.push_back(makeToken(TokenKind::Arrow, "->", location));
                }
                else
                {
                    tokens.push_back(makeToken(TokenKind::Minus, "-", location));
                }
                break;
            case '*':
                tokens.push_back(makeToken(TokenKind::Star, "*", location));
                break;
            case '&':
                if (match('&'))
                {
                    tokens.push_back(makeToken(TokenKind::AmpAmp, "&&", location));
                }
                else
                {
                    tokens.push_back(makeToken(TokenKind::Ampersand, "&", location));
                }
                break;
            case '|':
                if (match('|'))
                {
                    tokens.push_back(makeToken(TokenKind::PipePipe, "||", location));
                }
                else
                {
                    core::throwDiagnostic(location, "unexpected character '|'");
                }
                break;
            case '/':
                tokens.push_back(makeToken(TokenKind::Slash, "/", location));
                break;
            case '%':
                tokens.push_back(makeToken(TokenKind::Percent, "%", location));
                break;
            case '^':
                tokens.push_back(makeToken(TokenKind::Caret, "^", location));
                break;
            default:
                core::throwDiagnostic(location, std::string("unexpected character '") + c + "'");
            }
        }

        tokens.push_back(Token{TokenKind::EndOfFile, "", core::SourceLocation{filePath_, line_, column_}});
        return tokens;
    }

    bool Lexer::atEnd() const noexcept
    {
        return index_ >= source_.size();
    }

    char Lexer::peek(std::size_t offset) const noexcept
    {
        const auto position = index_ + offset;
        if (position >= source_.size())
        {
            return '\0';
        }
        return source_[position];
    }

    char Lexer::advance() noexcept
    {
        if (atEnd())
        {
            return '\0';
        }
        const char c = source_[index_++];
        if (c == '\n')
        {
            ++line_;
            column_ = 1;
        }
        else
        {
            ++column_;
        }
        return c;
    }

    bool Lexer::match(char expected) noexcept
    {
        if (atEnd() || source_[index_] != expected)
        {
            return false;
        }
        (void)advance();
        return true;
    }

    void Lexer::skipWhitespaceAndComments()
    {
        while (!atEnd())
        {
            const char c = peek();
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                (void)advance();
                continue;
            }

            if (c == '/' && peek(1) == '/')
            {
                while (!atEnd() && peek() != '\n')
                {
                    (void)advance();
                }
                continue;
            }

            if (c == '/' && peek(1) == '*')
            {
                const auto start = core::SourceLocation{filePath_, line_, column_};
                (void)advance();
                (void)advance();
                bool closed = false;
                while (!atEnd())
                {
                    if (peek() == '*' && peek(1) == '/')
                    {
                        (void)advance();
                        (void)advance();
                        closed = true;
                        break;
                    }
                    (void)advance();
                }
                if (!closed)
                {
                    core::throwDiagnostic(start, "unterminated block comment");
                }
                continue;
            }

            break;
        }
    }

    Token Lexer::makeToken(TokenKind kind, std::string lexeme, core::SourceLocation location) const
    {
        return Token{kind, std::move(lexeme), std::move(location)};
    }

    Token Lexer::lexIdentifierOrKeyword()
    {
        const auto location = core::SourceLocation{filePath_, line_, column_};
        std::string text;

        while (!atEnd())
        {
            const char c = peek();
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            {
                break;
            }
            text.push_back(advance());
        }

        const auto kind = keywordKind(text);
        return makeToken(kind, std::move(text), location);
    }

    Token Lexer::lexNumber()
    {
        const auto location = core::SourceLocation{filePath_, line_, column_};
        std::string text;

        if ((peek() == 'F' || peek() == 'f') && peek(1) == '0' && (peek(2) == 'x' || peek(2) == 'X'))
        {
            text.push_back(advance());
        }

        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X'))
        {
            text.push_back(advance());
            text.push_back(advance());
            while (std::isxdigit(static_cast<unsigned char>(peek())))
            {
                text.push_back(advance());
            }
            return makeToken(TokenKind::IntegerLiteral, std::move(text), location);
        }

        bool sawDot = false;
        while (!atEnd())
        {
            const char c = peek();
            if (std::isdigit(static_cast<unsigned char>(c)))
            {
                text.push_back(advance());
                continue;
            }
            if (c == '.' && !sawDot)
            {
                if (peek(1) == '.')
                {
                    break;
                }
                sawDot = true;
                text.push_back(advance());
                continue;
            }
            break;
        }

        if (sawDot && (peek() == 'f' || peek() == 'F'))
        {
            text.push_back(advance());
        }

        return makeToken(sawDot ? TokenKind::FloatingLiteral : TokenKind::IntegerLiteral, std::move(text), location);
    }

    Token Lexer::lexChar()
    {
        const auto location = core::SourceLocation{filePath_, line_, column_};
        std::string text;
        (void)advance();

        if (atEnd())
        {
            core::throwDiagnostic(location, "unterminated char literal");
        }

        if (peek() == '\'')
        {
            (void)advance();
            return makeToken(TokenKind::CharLiteral, std::string(1, '\0'), location);
        }

        char value = 0;
        const char c = advance();
        if (c == '\\')
        {
            if (atEnd())
                core::throwDiagnostic(location, "unterminated char escape");
            const char esc = advance();
            switch (esc)
            {
            case 'n':
                value = '\n';
                break;
            case 'r':
                value = '\r';
                break;
            case 't':
                value = '\t';
                break;
            case '0':
                value = '\0';
                break;
            case '\'':
                value = '\'';
                break;
            case '\\':
                value = '\\';
                break;
            default:
                value = esc;
                break;
            }
        }
        else
        {
            value = c;
        }

        if (!match('\''))
        {
            core::throwDiagnostic(location, "unterminated char literal");
        }

        text.push_back(value);
        return makeToken(TokenKind::CharLiteral, std::move(text), location);
    }

    Token Lexer::lexString(bool isInterpolated)
    {
        const auto location = core::SourceLocation{filePath_, line_, column_};
        std::string text;
        if (isInterpolated)
        {
            (void)advance();
        }
        (void)advance();

        while (!atEnd())
        {
            const char c = advance();
            if (c == '"')
            {
                return makeToken(isInterpolated ? TokenKind::InterpolatedStringLiteral : TokenKind::StringLiteral, std::move(text), location);
            }
            if (c == '\\' && !atEnd())
            {
                text.push_back(c);
                text.push_back(advance());
                continue;
            }
            text.push_back(c);
        }

        core::throwDiagnostic(location, "unterminated string literal");
    }

} // namespace dash::frontend

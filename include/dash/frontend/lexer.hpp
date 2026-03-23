#pragma once

#include <string>
#include <vector>

#include "dash/frontend/token.hpp"

namespace dash::frontend {

class Lexer {
public:
    Lexer(std::string filePath, std::string source);

    [[nodiscard]] std::vector<Token> tokenize();

private:
    [[nodiscard]] bool atEnd() const noexcept;
    [[nodiscard]] char peek(std::size_t offset = 0) const noexcept;
    [[nodiscard]] char advance() noexcept;
    [[nodiscard]] bool match(char expected) noexcept;

    void skipWhitespaceAndComments();
    [[nodiscard]] Token makeToken(TokenKind kind, std::string lexeme, core::SourceLocation location) const;
    [[nodiscard]] Token lexIdentifierOrKeyword();
    [[nodiscard]] Token lexNumber();
    [[nodiscard]] Token lexString(bool isInterpolated = false);
    [[nodiscard]] Token lexChar();

    std::string filePath_;
    std::string source_;
    std::size_t index_ {0};
    std::size_t line_ {1};
    std::size_t column_ {1};
};

} // namespace dash::frontend

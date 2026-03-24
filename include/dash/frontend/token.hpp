#pragma once

#include <string>

#include "dash/core/source_location.hpp"

namespace dash::frontend {

enum class TokenKind {
    EndOfFile,

    Identifier,
    IntegerLiteral,
    FloatingLiteral,
    StringLiteral,
    InterpolatedStringLiteral,
    CharLiteral,

    KwExtern,
    KwFn,
    KwReturn,
    KwLet,
    KwConst,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwDo,
    KwVoid,
    KwBool,
    KwInt,
    KwUInt,
    KwDouble,
    KwFloat,
    KwChar,
    KwString,
    KwTrue,
    KwFalse,
    KwPrivate,
    KwIs,
    KwClass,
    KwGroup,
    KwStatic,
    KwSelf,
    KwSwitch,
    KwCase,
    KwDefault,
    KwMatch,
    KwEnum,
    KwBreak,
    KwExport,
    KwNull,

    At,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Colon,
    Semicolon,
    Comma,
    Question,
    Dot,
    DotDot,
    Assign,
    EqualEqual,
    Bang,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Plus,
    PlusAssign,
    Minus,
    MinusAssign,
    Arrow,
    Star,
    Slash,
    Percent,
    Caret,
    ShiftLeft,
    ShiftRight,
    Ampersand,
    AmpAmp,
    PipePipe,
    Ellipsis,
    PlusPlus,
    MinusMinus
};

struct Token {
    TokenKind kind {TokenKind::EndOfFile};
    std::string lexeme;
    core::SourceLocation location;
};

[[nodiscard]] std::string tokenKindName(TokenKind kind);

} // namespace dash::frontend

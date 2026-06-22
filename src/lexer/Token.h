#pragma once
#include <string>
#include <cstdint>

namespace toyc {

enum class Tok {
    // literals / identifiers
    Ident,
    Number,
    // keywords
    Kw_int, Kw_void, Kw_const,
    Kw_if, Kw_else, Kw_while,
    Kw_break, Kw_continue, Kw_return,
    // punctuation
    LParen, RParen, LBrace, RBrace,
    Semi, Comma, Assign,
    // operators
    Plus, Minus, Star, Slash, Percent,
    Not,                       // !
    Lt, Gt, Le, Ge, Eq, Ne,    // < > <= >= == !=
    AndAnd, OrOr,              // && ||
    // end of file
    Eof,
};

struct Token {
    Tok kind;
    std::string text;   // identifier name / raw number text
    int64_t value = 0;  // parsed numeric value (for Number)
    int line = 0;
    int col = 0;
};

const char* tokName(Tok k);

} // namespace toyc

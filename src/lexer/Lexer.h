#pragma once
#include "lexer/Token.h"
#include <string>
#include <vector>

namespace toyc {

// Hand-written lexer for ToyC. Tokenizes the whole input up front.
class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}

    // Tokenize the entire source. Throws std::runtime_error on a lexical error.
    std::vector<Token> tokenize();

private:
    std::string src_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;

    char peek(size_t off = 0) const;
    char advance();
    bool eof() const { return pos_ >= src_.size(); }
    void skipTrivia();
    [[noreturn]] void error(const std::string& msg) const;
};

} // namespace toyc

#include "lexer/Lexer.h"
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace toyc {

const char* tokName(Tok k) {
    switch (k) {
        case Tok::Ident: return "identifier";
        case Tok::Number: return "number";
        case Tok::Kw_int: return "int";
        case Tok::Kw_void: return "void";
        case Tok::Kw_const: return "const";
        case Tok::Kw_if: return "if";
        case Tok::Kw_else: return "else";
        case Tok::Kw_while: return "while";
        case Tok::Kw_break: return "break";
        case Tok::Kw_continue: return "continue";
        case Tok::Kw_return: return "return";
        case Tok::LParen: return "(";
        case Tok::RParen: return ")";
        case Tok::LBrace: return "{";
        case Tok::RBrace: return "}";
        case Tok::Semi: return ";";
        case Tok::Comma: return ",";
        case Tok::Assign: return "=";
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::Slash: return "/";
        case Tok::Percent: return "%";
        case Tok::Not: return "!";
        case Tok::Lt: return "<";
        case Tok::Gt: return ">";
        case Tok::Le: return "<=";
        case Tok::Ge: return ">=";
        case Tok::Eq: return "==";
        case Tok::Ne: return "!=";
        case Tok::AndAnd: return "&&";
        case Tok::OrOr: return "||";
        case Tok::Eof: return "<eof>";
    }
    return "?";
}

char Lexer::peek(size_t off) const {
    size_t p = pos_ + off;
    return p < src_.size() ? src_[p] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { line_++; col_ = 1; } else { col_++; }
    return c;
}

void Lexer::error(const std::string& msg) const {
    throw std::runtime_error("lexical error at " + std::to_string(line_) + ":" +
                             std::to_string(col_) + ": " + msg);
}

void Lexer::skipTrivia() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (!eof() && peek() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            advance(); advance();
            bool closed = false;
            while (!eof()) {
                if (peek() == '*' && peek(1) == '/') { advance(); advance(); closed = true; break; }
                advance();
            }
            if (!closed) error("unterminated block comment");
        } else {
            break;
        }
    }
}

static const std::unordered_map<std::string, Tok>& keywords() {
    static const std::unordered_map<std::string, Tok> kw = {
        {"int", Tok::Kw_int}, {"void", Tok::Kw_void}, {"const", Tok::Kw_const},
        {"if", Tok::Kw_if}, {"else", Tok::Kw_else}, {"while", Tok::Kw_while},
        {"break", Tok::Kw_break}, {"continue", Tok::Kw_continue}, {"return", Tok::Kw_return},
    };
    return kw;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    for (;;) {
        skipTrivia();
        int ln = line_, cl = col_;
        if (eof()) { out.push_back({Tok::Eof, "", 0, ln, cl}); break; }

        char c = peek();

        // identifier / keyword
        if (std::isalpha((unsigned char)c) || c == '_') {
            std::string id;
            while (std::isalnum((unsigned char)peek()) || peek() == '_') id += advance();
            auto it = keywords().find(id);
            Token t;
            t.kind = (it != keywords().end()) ? it->second : Tok::Ident;
            t.text = id; t.line = ln; t.col = cl;
            out.push_back(std::move(t));
            continue;
        }

        // number (decimal). The grammar's NUMBER is unsigned here; a leading '-'
        // is handled as a unary operator by the parser.
        if (std::isdigit((unsigned char)c)) {
            std::string num;
            // Leading-zero rule: "0" alone, otherwise [1-9][0-9]*. We accept a
            // run of digits and rely on the input being well-formed per spec.
            while (std::isdigit((unsigned char)peek())) num += advance();
            Token t;
            t.kind = Tok::Number; t.text = num; t.line = ln; t.col = cl;
            // Parse into 64-bit then truncate to 32-bit two's complement so that
            // 2147483648 used under unary minus yields INT_MIN correctly.
            try {
                t.value = (int32_t)(uint32_t)std::stoull(num);
            } catch (...) {
                t.value = (int32_t)(uint32_t)std::stoull(num.substr(num.size() >= 10 ? num.size() - 10 : 0));
            }
            out.push_back(std::move(t));
            continue;
        }

        advance(); // consume first char of the operator/punct
        auto push = [&](Tok k, const char* s) { out.push_back({k, s, 0, ln, cl}); };
        switch (c) {
            case '(': push(Tok::LParen, "("); break;
            case ')': push(Tok::RParen, ")"); break;
            case '{': push(Tok::LBrace, "{"); break;
            case '}': push(Tok::RBrace, "}"); break;
            case ';': push(Tok::Semi, ";"); break;
            case ',': push(Tok::Comma, ","); break;
            case '+': push(Tok::Plus, "+"); break;
            case '-': push(Tok::Minus, "-"); break;
            case '*': push(Tok::Star, "*"); break;
            case '/': push(Tok::Slash, "/"); break;
            case '%': push(Tok::Percent, "%"); break;
            case '=':
                if (peek() == '=') { advance(); push(Tok::Eq, "=="); }
                else push(Tok::Assign, "=");
                break;
            case '!':
                if (peek() == '=') { advance(); push(Tok::Ne, "!="); }
                else push(Tok::Not, "!");
                break;
            case '<':
                if (peek() == '=') { advance(); push(Tok::Le, "<="); }
                else push(Tok::Lt, "<");
                break;
            case '>':
                if (peek() == '=') { advance(); push(Tok::Ge, ">="); }
                else push(Tok::Gt, ">");
                break;
            case '&':
                if (peek() == '&') { advance(); push(Tok::AndAnd, "&&"); }
                else error("unexpected '&' (bitwise operators are not supported)");
                break;
            case '|':
                if (peek() == '|') { advance(); push(Tok::OrOr, "||"); }
                else error("unexpected '|' (bitwise operators are not supported)");
                break;
            default:
                error(std::string("unexpected character '") + c + "'");
        }
    }
    return out;
}

} // namespace toyc

#pragma once
#include "ast/AST.h"
#include "lexer/Token.h"
#include <vector>

namespace toyc {

// Recursive-descent / precedence-layered parser for ToyC.
class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    // Parse a whole compilation unit. Throws std::runtime_error on syntax error.
    Module parseModule();

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    const Token& cur() const { return toks_[pos_]; }
    const Token& peek(size_t off = 1) const;
    bool check(Tok k) const { return cur().kind == k; }
    bool accept(Tok k);
    const Token& expect(Tok k, const char* what);
    Token advance();
    [[noreturn]] void error(const std::string& msg) const;

    // top level
    void parseTopLevel(Module& m);
    std::unique_ptr<FuncDef> parseFuncDef(bool returnsVoid);
    std::unique_ptr<GlobalVar> parseGlobalVar(bool isConst);

    // statements
    StmtPtr parseStmt();
    std::unique_ptr<BlockStmt> parseBlock();
    StmtPtr parseDeclStmt();

    // expressions (precedence climbing layers)
    ExprPtr parseExpr();
    ExprPtr parseLOr();
    ExprPtr parseLAnd();
    ExprPtr parseRel();
    ExprPtr parseAdd();
    ExprPtr parseMul();
    ExprPtr parseUnary();
    ExprPtr parsePrimary();
};

} // namespace toyc

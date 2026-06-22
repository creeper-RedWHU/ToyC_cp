#include "parser/Parser.h"
#include <stdexcept>

namespace toyc {

const Token& Parser::peek(size_t off) const {
    size_t p = pos_ + off;
    return p < toks_.size() ? toks_[p] : toks_.back(); // back() is Eof
}

bool Parser::accept(Tok k) {
    if (cur().kind == k) { pos_++; return true; }
    return false;
}

Token Parser::advance() { return toks_[pos_++]; }

const Token& Parser::expect(Tok k, const char* what) {
    if (cur().kind != k)
        error(std::string("expected ") + what + " but found '" + tokName(cur().kind) + "'");
    return toks_[pos_++];
}

void Parser::error(const std::string& msg) const {
    throw std::runtime_error("syntax error at " + std::to_string(cur().line) + ":" +
                             std::to_string(cur().col) + ": " + msg);
}

// ============================ top level ======================================
Module Parser::parseModule() {
    Module m;
    while (!check(Tok::Eof)) parseTopLevel(m);
    return m;
}

void Parser::parseTopLevel(Module& m) {
    int line = cur().line;
    if (check(Tok::Kw_const)) {
        TopLevel t; t.kind = TopLevel::Kind::Global;
        t.global = parseGlobalVar(true);
        t.global->line = line;
        m.items.push_back(std::move(t));
        return;
    }
    if (check(Tok::Kw_void)) {
        advance();
        TopLevel t; t.kind = TopLevel::Kind::Func;
        t.func = parseFuncDef(true);
        t.func->line = line;
        m.items.push_back(std::move(t));
        return;
    }
    if (check(Tok::Kw_int)) {
        advance();
        // 'int' ID  -> decide function vs global variable by the next token.
        if (!check(Tok::Ident)) error("expected identifier after 'int'");
        if (peek(1).kind == Tok::LParen) {
            TopLevel t; t.kind = TopLevel::Kind::Func;
            t.func = parseFuncDef(false);
            t.func->line = line;
            m.items.push_back(std::move(t));
        } else {
            TopLevel t; t.kind = TopLevel::Kind::Global;
            t.global = parseGlobalVar(false);
            t.global->line = line;
            m.items.push_back(std::move(t));
        }
        return;
    }
    error("expected a global declaration or function definition");
}

// FuncDef: '(' int|void already consumed; cursor at ID.
std::unique_ptr<FuncDef> Parser::parseFuncDef(bool returnsVoid) {
    auto fn = std::make_unique<FuncDef>();
    fn->returnsVoid = returnsVoid;
    fn->name = expect(Tok::Ident, "function name").text;
    expect(Tok::LParen, "'('");
    if (!check(Tok::RParen)) {
        do {
            expect(Tok::Kw_int, "'int' parameter type");
            Param p;
            p.name = expect(Tok::Ident, "parameter name").text;
            fn->params.push_back(std::move(p));
        } while (accept(Tok::Comma));
    }
    expect(Tok::RParen, "')'");
    fn->body = parseBlock();
    return fn;
}

// 'const'/'int' keyword still at cursor.
std::unique_ptr<GlobalVar> Parser::parseGlobalVar(bool isConst) {
    if (isConst) { expect(Tok::Kw_const, "'const'"); expect(Tok::Kw_int, "'int'"); }
    else { expect(Tok::Kw_int, "'int'"); }
    auto g = std::make_unique<GlobalVar>();
    g->isConst = isConst;
    g->name = expect(Tok::Ident, "variable name").text;
    expect(Tok::Assign, "'=' (declarations must be initialized)");
    g->init = parseExpr();
    expect(Tok::Semi, "';'");
    return g;
}

// ============================ statements =====================================
std::unique_ptr<BlockStmt> Parser::parseBlock() {
    expect(Tok::LBrace, "'{'");
    auto b = std::make_unique<BlockStmt>();
    b->line = cur().line;
    while (!check(Tok::RBrace) && !check(Tok::Eof))
        b->stmts.push_back(parseStmt());
    expect(Tok::RBrace, "'}'");
    return b;
}

StmtPtr Parser::parseDeclStmt() {
    bool isConst = accept(Tok::Kw_const);
    expect(Tok::Kw_int, "'int'");
    std::string name = expect(Tok::Ident, "variable name").text;
    expect(Tok::Assign, "'=' (declarations must be initialized)");
    ExprPtr init = parseExpr();
    expect(Tok::Semi, "';'");
    return std::make_unique<VarDeclStmt>(isConst, std::move(name), std::move(init));
}

StmtPtr Parser::parseStmt() {
    int line = cur().line;
    switch (cur().kind) {
        case Tok::LBrace: { auto s = parseBlock(); s->line = line; return s; }
        case Tok::Semi:   { advance(); auto s = std::make_unique<EmptyStmt>(); s->line = line; return s; }
        case Tok::Kw_const:
        case Tok::Kw_int: { auto s = parseDeclStmt(); s->line = line; return s; }
        case Tok::Kw_if: {
            advance();
            auto s = std::make_unique<IfStmt>();
            s->line = line;
            expect(Tok::LParen, "'('");
            s->cond = parseExpr();
            expect(Tok::RParen, "')'");
            s->then_ = parseStmt();
            if (accept(Tok::Kw_else)) s->else_ = parseStmt();
            return s;
        }
        case Tok::Kw_while: {
            advance();
            auto s = std::make_unique<WhileStmt>();
            s->line = line;
            expect(Tok::LParen, "'('");
            s->cond = parseExpr();
            expect(Tok::RParen, "')'");
            s->body = parseStmt();
            return s;
        }
        case Tok::Kw_break:    { advance(); expect(Tok::Semi, "';'"); auto s = std::make_unique<BreakStmt>(); s->line = line; return s; }
        case Tok::Kw_continue: { advance(); expect(Tok::Semi, "';'"); auto s = std::make_unique<ContinueStmt>(); s->line = line; return s; }
        case Tok::Kw_return: {
            advance();
            auto s = std::make_unique<ReturnStmt>();
            s->line = line;
            if (!check(Tok::Semi)) s->value = parseExpr();
            expect(Tok::Semi, "';'");
            return s;
        }
        default: break;
    }

    // Assignment ( ID '=' Expr ';' ) vs expression statement.
    if (check(Tok::Ident) && peek(1).kind == Tok::Assign) {
        std::string name = advance().text;  // ID
        advance();                          // '='
        ExprPtr val = parseExpr();
        expect(Tok::Semi, "';'");
        auto s = std::make_unique<AssignStmt>(std::move(name), std::move(val));
        s->line = line;
        return s;
    }

    ExprPtr e = parseExpr();
    expect(Tok::Semi, "';'");
    auto s = std::make_unique<ExprStmt>(std::move(e));
    s->line = line;
    return s;
}

// ============================ expressions ====================================
ExprPtr Parser::parseExpr() { return parseLOr(); }

ExprPtr Parser::parseLOr() {
    ExprPtr lhs = parseLAnd();
    while (check(Tok::OrOr)) {
        int line = cur().line; advance();
        ExprPtr rhs = parseLAnd();
        auto e = std::make_unique<LogicalExpr>(true, std::move(lhs), std::move(rhs));
        e->line = line;
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseLAnd() {
    ExprPtr lhs = parseRel();
    while (check(Tok::AndAnd)) {
        int line = cur().line; advance();
        ExprPtr rhs = parseRel();
        auto e = std::make_unique<LogicalExpr>(false, std::move(lhs), std::move(rhs));
        e->line = line;
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseRel() {
    ExprPtr lhs = parseAdd();
    for (;;) {
        BinOp op;
        switch (cur().kind) {
            case Tok::Lt: op = BinOp::Lt; break;
            case Tok::Gt: op = BinOp::Gt; break;
            case Tok::Le: op = BinOp::Le; break;
            case Tok::Ge: op = BinOp::Ge; break;
            case Tok::Eq: op = BinOp::Eq; break;
            case Tok::Ne: op = BinOp::Ne; break;
            default: return lhs;
        }
        int line = cur().line; advance();
        ExprPtr rhs = parseAdd();
        auto e = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
        e->line = line;
        lhs = std::move(e);
    }
}

ExprPtr Parser::parseAdd() {
    ExprPtr lhs = parseMul();
    for (;;) {
        BinOp op;
        if (check(Tok::Plus)) op = BinOp::Add;
        else if (check(Tok::Minus)) op = BinOp::Sub;
        else return lhs;
        int line = cur().line; advance();
        ExprPtr rhs = parseMul();
        auto e = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
        e->line = line;
        lhs = std::move(e);
    }
}

ExprPtr Parser::parseMul() {
    ExprPtr lhs = parseUnary();
    for (;;) {
        BinOp op;
        if (check(Tok::Star)) op = BinOp::Mul;
        else if (check(Tok::Slash)) op = BinOp::Div;
        else if (check(Tok::Percent)) op = BinOp::Mod;
        else return lhs;
        int line = cur().line; advance();
        ExprPtr rhs = parseUnary();
        auto e = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
        e->line = line;
        lhs = std::move(e);
    }
}

ExprPtr Parser::parseUnary() {
    UnOp op;
    bool isUnary = true;
    switch (cur().kind) {
        case Tok::Plus:  op = UnOp::Plus; break;
        case Tok::Minus: op = UnOp::Neg;  break;
        case Tok::Not:   op = UnOp::Not;  break;
        default: isUnary = false; break;
    }
    if (isUnary) {
        int line = cur().line; advance();
        ExprPtr operand = parseUnary();
        auto e = std::make_unique<UnaryExpr>(op, std::move(operand));
        e->line = line;
        return e;
    }
    return parsePrimary();
}

ExprPtr Parser::parsePrimary() {
    int line = cur().line;
    switch (cur().kind) {
        case Tok::Number: {
            int32_t v = (int32_t)advance().value;
            auto e = std::make_unique<NumberExpr>(v);
            e->line = line;
            return e;
        }
        case Tok::LParen: {
            advance();
            ExprPtr e = parseExpr();
            expect(Tok::RParen, "')'");
            return e;
        }
        case Tok::Ident: {
            std::string name = advance().text;
            if (accept(Tok::LParen)) {           // function call
                auto call = std::make_unique<CallExpr>(name);
                call->line = line;
                if (!check(Tok::RParen)) {
                    do { call->args.push_back(parseExpr()); } while (accept(Tok::Comma));
                }
                expect(Tok::RParen, "')'");
                return call;
            }
            auto e = std::make_unique<VarExpr>(name);  // variable reference
            e->line = line;
            return e;
        }
        default:
            error(std::string("expected an expression but found '") + tokName(cur().kind) + "'");
    }
}

} // namespace toyc

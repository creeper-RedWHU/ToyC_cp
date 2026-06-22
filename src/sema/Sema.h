#pragma once
#include "ast/AST.h"
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace toyc {

// Semantic analysis: scope-based name resolution, symbol creation, constant
// folding/evaluation, and basic well-formedness checks.
//
// Inputs are guaranteed by the spec to be valid ToyC, so this pass is
// conservative: it never rejects a program it cannot prove invalid. Missing
// returns are tolerated here and handled with safe defaults in codegen.
class Sema {
public:
    // Analyze and annotate the module in place. `globalCount` receives the
    // number of global variables (each with a distinct sym->globalId).
    void analyze(Module& m, int& globalCount);

private:
    std::vector<std::unique_ptr<Symbol>> arena_;
    std::vector<std::unordered_map<std::string, Symbol*>> scopes_;
    std::unordered_map<std::string, FuncDef*> funcs_;
    int globalCount_ = 0;
    int loopDepth_ = 0;

    Symbol* newSymbol();
    Symbol* declare(const std::string& name, Symbol::Kind kind);
    Symbol* lookup(const std::string& name);
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }

    void analyzeFunc(FuncDef& fn);
    void analyzeStmt(Stmt* s);
    void analyzeExpr(Expr* e);   // resolves names and folds constants

    [[noreturn]] void error(int line, const std::string& msg) const;
};

// Evaluate a binary/unary op with 32-bit two's-complement (RISC-V) semantics.
std::optional<int32_t> evalBin(BinOp op, int32_t a, int32_t b, bool& divByZero);
int32_t evalUn(UnOp op, int32_t a);

} // namespace toyc

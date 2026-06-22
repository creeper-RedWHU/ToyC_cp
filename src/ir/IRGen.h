#pragma once
#include "ast/AST.h"
#include "ir/IR.h"
#include <vector>

namespace toyc {

// Lowers the annotated AST into the three-address IR.
//
// Variable model: every local variable and parameter is a virtual register
// (ToyC has no address-of, so locals never need memory). Constants are inlined.
// Globals live in memory and are accessed via LoadGlobal/StoreGlobal.
class IRGen {
public:
    IRModule generate(Module& m, int globalCount);

private:
    std::vector<BasicBlock> blocks_;
    int curBB_ = -1;
    bool sealed_ = false;
    int nextVreg_ = 0;
    bool curReturnsVoid_ = false;

    struct Loop { int continueBB; int breakBB; };
    std::vector<Loop> loops_;

    int newVreg() { return nextVreg_++; }
    int newBlock();
    void startBlock(int id) { curBB_ = id; sealed_ = false; }
    void ensureOpen();
    void emit(Inst i);
    void termJmp(int target);
    void termBr(Val cond, int t, int f);
    void termRet(bool hasVal, Val v);

    void genFunc(FuncDef& fn, IRFunc& out);
    void genStmt(Stmt* s);
    Val genExpr(Expr* e);
    void genCond(Expr* e, int tBB, int fBB);
};

} // namespace toyc

#include "ir/IRGen.h"

namespace toyc {

void computeCFG(IRFunc& fn) {
    for (auto& bb : fn.blocks) bb.preds.clear();
    for (auto& bb : fn.blocks) {
        int s[2], n; succsOf(bb, s, n);
        for (int i = 0; i < n; i++)
            if (s[i] >= 0) fn.blocks[s[i]].preds.push_back(bb.id);
    }
}

static Op mapBin(BinOp op) {
    switch (op) {
        case BinOp::Add: return Op::Add; case BinOp::Sub: return Op::Sub;
        case BinOp::Mul: return Op::Mul; case BinOp::Div: return Op::Div;
        case BinOp::Mod: return Op::Mod; case BinOp::Lt: return Op::Lt;
        case BinOp::Gt: return Op::Gt;  case BinOp::Le: return Op::Le;
        case BinOp::Ge: return Op::Ge;  case BinOp::Eq: return Op::Eq;
        case BinOp::Ne: return Op::Ne;
    }
    return Op::Add;
}

int IRGen::newBlock() {
    int id = (int)blocks_.size();
    BasicBlock bb; bb.id = id;
    blocks_.push_back(std::move(bb));
    return id;
}

void IRGen::ensureOpen() {
    if (sealed_) startBlock(newBlock());   // begin a fresh (unreachable) block
}

void IRGen::emit(Inst i) {
    ensureOpen();
    blocks_[curBB_].insts.push_back(std::move(i));
}

void IRGen::termJmp(int target) {
    ensureOpen();
    auto& bb = blocks_[curBB_];
    bb.term = Term::Jmp; bb.succ0 = target;
    sealed_ = true;
}

void IRGen::termBr(Val cond, int t, int f) {
    ensureOpen();
    auto& bb = blocks_[curBB_];
    bb.term = Term::Br; bb.cond = cond; bb.succ0 = t; bb.succ1 = f;
    sealed_ = true;
}

void IRGen::termRet(bool hasVal, Val v) {
    ensureOpen();
    auto& bb = blocks_[curBB_];
    bb.term = Term::Ret; bb.retHasVal = hasVal; bb.retVal = v;
    sealed_ = true;
}

IRModule IRGen::generate(Module& m, int globalCount) {
    IRModule mod;
    mod.globals.resize(globalCount);
    for (auto& item : m.items) {
        if (item.kind == TopLevel::Kind::Global && !item.global->isConst) {
            int id = item.global->sym->globalId;
            mod.globals[id] = { item.global->name, item.global->initValue };
        }
    }
    for (auto& item : m.items) {
        if (item.kind == TopLevel::Kind::Func) {
            IRFunc f;
            genFunc(*item.func, f);
            mod.funcs.push_back(std::move(f));
        }
    }
    return mod;
}

void IRGen::genFunc(FuncDef& fn, IRFunc& out) {
    blocks_.clear();
    loops_.clear();
    nextVreg_ = 0;
    curReturnsVoid_ = fn.returnsVoid;

    out.name = fn.name;
    out.returnsVoid = fn.returnsVoid;
    out.numParams = (int)fn.params.size();

    for (auto& p : fn.params) {
        int v = newVreg();
        p.sym->irId = v;
        out.paramRegs.push_back(v);
    }

    int entry = newBlock();
    startBlock(entry);
    genStmt(fn.body.get());

    if (!sealed_) {
        if (curReturnsVoid_) termRet(false, Val::I(0));
        else termRet(true, Val::I(0));   // safe default for missing return
    }

    out.numVregs = nextVreg_;
    out.blocks = std::move(blocks_);
    computeCFG(out);
}

void IRGen::genStmt(Stmt* s) {
    switch (s->kind) {
        case Stmt::Kind::Block:
            for (auto& st : static_cast<BlockStmt*>(s)->stmts) genStmt(st.get());
            break;
        case Stmt::Kind::Empty:
            break;
        case Stmt::Kind::ExprStmt:
            genExpr(static_cast<ExprStmt*>(s)->expr.get());
            break;
        case Stmt::Kind::VarDecl: {
            auto* d = static_cast<VarDeclStmt*>(s);
            if (d->isConst) break;             // inlined constant
            int v = newVreg();
            d->sym->irId = v;
            Val init = genExpr(d->init.get());
            Inst i; i.op = Op::Mv; i.dst = v; i.a = init;
            emit(std::move(i));
            break;
        }
        case Stmt::Kind::Assign: {
            auto* a = static_cast<AssignStmt*>(s);
            Val v = genExpr(a->value.get());
            if (a->sym->kind == Symbol::Kind::GlobalVar) {
                Inst i; i.op = Op::StoreGlobal; i.gid = a->sym->globalId; i.a = v;
                emit(std::move(i));
            } else {
                Inst i; i.op = Op::Mv; i.dst = a->sym->irId; i.a = v;
                emit(std::move(i));
            }
            break;
        }
        case Stmt::Kind::If: {
            auto* i = static_cast<IfStmt*>(s);
            if (i->cond->isConst) {            // statically known condition
                if (i->cond->constValue != 0) genStmt(i->then_.get());
                else if (i->else_) genStmt(i->else_.get());
                break;
            }
            bool hasElse = (bool)i->else_;
            int tBB = newBlock();
            int jBB = newBlock();
            int eBB = hasElse ? newBlock() : jBB;
            genCond(i->cond.get(), tBB, eBB);
            startBlock(tBB); genStmt(i->then_.get()); termJmp(jBB);
            if (hasElse) { startBlock(eBB); genStmt(i->else_.get()); termJmp(jBB); }
            startBlock(jBB);
            break;
        }
        case Stmt::Kind::While: {
            // Rotated layout: the body block is created before the condition
            // block so that, in emission order [body, cond, after], the body
            // falls through to the condition and the condition branches back to
            // the body -- one branch per iteration instead of two.
            auto* w = static_cast<WhileStmt*>(s);
            int bBB = newBlock();
            int cBB = newBlock();
            int aBB = newBlock();
            termJmp(cBB);                         // initial guard
            loops_.push_back({cBB, aBB});         // continue -> cond, break -> after
            startBlock(bBB); genStmt(w->body.get()); termJmp(cBB);
            loops_.pop_back();
            startBlock(cBB); genCond(w->cond.get(), bBB, aBB);
            startBlock(aBB);
            break;
        }
        case Stmt::Kind::Break:
            termJmp(loops_.back().breakBB);
            break;
        case Stmt::Kind::Continue:
            termJmp(loops_.back().continueBB);
            break;
        case Stmt::Kind::Return: {
            auto* r = static_cast<ReturnStmt*>(s);
            if (r->value) { Val v = genExpr(r->value.get()); termRet(true, v); }
            else termRet(false, Val::I(0));
            break;
        }
    }
}

Val IRGen::genExpr(Expr* e) {
    if (e->isConst) return Val::I(e->constValue);

    switch (e->kind) {
        case Expr::Kind::Var: {
            auto* v = static_cast<VarExpr*>(e);
            if (v->sym->kind == Symbol::Kind::GlobalVar) {
                int t = newVreg();
                Inst i; i.op = Op::LoadGlobal; i.dst = t; i.gid = v->sym->globalId;
                emit(std::move(i));
                return Val::R(t);
            }
            return Val::R(v->sym->irId);   // local / param
        }
        case Expr::Kind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            Val la = genExpr(b->lhs.get());
            Val lb = genExpr(b->rhs.get());
            int t = newVreg();
            Inst i; i.op = mapBin(b->op); i.dst = t; i.a = la; i.b = lb;
            emit(std::move(i));
            return Val::R(t);
        }
        case Expr::Kind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            if (u->op == UnOp::Plus) return genExpr(u->operand.get());
            Val a = genExpr(u->operand.get());
            int t = newVreg();
            Inst i; i.op = (u->op == UnOp::Neg) ? Op::Neg : Op::Not; i.dst = t; i.a = a;
            emit(std::move(i));
            return Val::R(t);
        }
        case Expr::Kind::Logical: {
            int res = newVreg();
            int tBB = newBlock(), fBB = newBlock(), endBB = newBlock();
            genCond(e, tBB, fBB);
            startBlock(tBB); { Inst i; i.op = Op::Mv; i.dst = res; i.a = Val::I(1); emit(std::move(i)); } termJmp(endBB);
            startBlock(fBB); { Inst i; i.op = Op::Mv; i.dst = res; i.a = Val::I(0); emit(std::move(i)); } termJmp(endBB);
            startBlock(endBB);
            return Val::R(res);
        }
        case Expr::Kind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            Inst i; i.op = Op::Call; i.callee = c->target;
            for (auto& a : c->args) i.args.push_back(genExpr(a.get()));
            Val ret;
            if (c->target->returnsVoid) { i.dst = -1; ret = Val::I(0); }
            else { int t = newVreg(); i.dst = t; ret = Val::R(t); }
            emit(std::move(i));
            return ret;
        }
        default:
            return Val::I(0);   // Number handled by isConst above
    }
}

void IRGen::genCond(Expr* e, int tBB, int fBB) {
    if (e->isConst) { termJmp(e->constValue != 0 ? tBB : fBB); return; }

    if (e->kind == Expr::Kind::Logical) {
        auto* l = static_cast<LogicalExpr*>(e);
        int mid = newBlock();
        if (l->isOr) { genCond(l->lhs.get(), tBB, mid); startBlock(mid); genCond(l->rhs.get(), tBB, fBB); }
        else { genCond(l->lhs.get(), mid, fBB); startBlock(mid); genCond(l->rhs.get(), tBB, fBB); }
        return;
    }
    if (e->kind == Expr::Kind::Unary && static_cast<UnaryExpr*>(e)->op == UnOp::Not) {
        genCond(static_cast<UnaryExpr*>(e)->operand.get(), fBB, tBB);
        return;
    }
    Val v = genExpr(e);
    termBr(v, tBB, fBB);
}

} // namespace toyc

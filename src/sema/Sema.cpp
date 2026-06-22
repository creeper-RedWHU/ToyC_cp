#include "sema/Sema.h"
#include <climits>
#include <stdexcept>

namespace toyc {

void Sema::error(int line, const std::string& msg) const {
    throw std::runtime_error("semantic error at line " + std::to_string(line) + ": " + msg);
}

Symbol* Sema::newSymbol() {
    arena_.push_back(std::make_unique<Symbol>());
    return arena_.back().get();
}

Symbol* Sema::declare(const std::string& name, Symbol::Kind kind) {
    Symbol* s = newSymbol();
    s->name = name;
    s->kind = kind;
    scopes_.back()[name] = s;
    return s;
}

Symbol* Sema::lookup(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second;
    }
    return nullptr;
}

// ----- arithmetic with RISC-V/2's-complement semantics ----------------------
std::optional<int32_t> evalBin(BinOp op, int32_t a, int32_t b, bool& divByZero) {
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    switch (op) {
        case BinOp::Add: return (int32_t)(ua + ub);
        case BinOp::Sub: return (int32_t)(ua - ub);
        case BinOp::Mul: return (int32_t)(ua * ub);
        case BinOp::Div:
            if (b == 0) { divByZero = true; return std::nullopt; }
            if (a == INT_MIN && b == -1) return INT_MIN;           // RISC-V overflow rule
            return (int32_t)(a / b);
        case BinOp::Mod:
            if (b == 0) { divByZero = true; return std::nullopt; }
            if (a == INT_MIN && b == -1) return 0;                 // RISC-V overflow rule
            return (int32_t)(a % b);
        case BinOp::Lt: return a < b;
        case BinOp::Gt: return a > b;
        case BinOp::Le: return a <= b;
        case BinOp::Ge: return a >= b;
        case BinOp::Eq: return a == b;
        case BinOp::Ne: return a != b;
    }
    return std::nullopt;
}

int32_t evalUn(UnOp op, int32_t a) {
    switch (op) {
        case UnOp::Plus: return a;
        case UnOp::Neg:  return (int32_t)(0u - (uint32_t)a);
        case UnOp::Not:  return a == 0 ? 1 : 0;
    }
    return a;
}

// ============================================================================
void Sema::analyze(Module& m, int& globalCount) {
    pushScope();   // global scope

    bool hasMain = false;
    for (auto& item : m.items) {
        if (item.kind == TopLevel::Kind::Global) {
            GlobalVar& g = *item.global;
            // Initializer of a global must be a compile-time constant.
            analyzeExpr(g.init.get());
            if (!g.init->isConst)
                error(g.line, "global initializer of '" + g.name + "' is not a constant expression");
            g.initValue = g.init->constValue;

            Symbol* s;
            if (g.isConst) {
                s = declare(g.name, Symbol::Kind::Const);
                s->isConst = true;
                s->constValue = g.initValue;
            } else {
                s = declare(g.name, Symbol::Kind::GlobalVar);
                s->globalId = globalCount_++;
            }
            g.sym = s;
        } else {
            FuncDef& fn = *item.func;
            if (funcs_.count(fn.name))
                error(fn.line, "redefinition of function '" + fn.name + "'");
            funcs_[fn.name] = &fn;   // register before body for self-recursion
            if (fn.name == "main") {
                hasMain = true;
                if (fn.returnsVoid || !fn.params.empty())
                    error(fn.line, "'main' must be 'int main()'");
            }
        }
    }
    if (!hasMain) error(0, "program is missing an entry function 'int main()'");

    // Analyze function bodies in source order.
    for (auto& item : m.items)
        if (item.kind == TopLevel::Kind::Func)
            analyzeFunc(*item.func);

    popScope();
    globalCount = globalCount_;
}

void Sema::analyzeFunc(FuncDef& fn) {
    pushScope();
    for (auto& p : fn.params) {
        Symbol* s = declare(p.name, Symbol::Kind::Param);
        p.sym = s;
    }
    analyzeStmt(fn.body.get());
    popScope();
}

void Sema::analyzeStmt(Stmt* s) {
    switch (s->kind) {
        case Stmt::Kind::Block: {
            auto* b = static_cast<BlockStmt*>(s);
            pushScope();
            for (auto& st : b->stmts) analyzeStmt(st.get());
            popScope();
            break;
        }
        case Stmt::Kind::Empty: break;
        case Stmt::Kind::ExprStmt:
            analyzeExpr(static_cast<ExprStmt*>(s)->expr.get());
            break;
        case Stmt::Kind::Assign: {
            auto* a = static_cast<AssignStmt*>(s);
            Symbol* sym = lookup(a->name);
            if (!sym) error(a->line, "use of undeclared identifier '" + a->name + "'");
            if (sym->isConst) error(a->line, "cannot assign to constant '" + a->name + "'");
            a->sym = sym;
            analyzeExpr(a->value.get());
            break;
        }
        case Stmt::Kind::VarDecl: {
            auto* d = static_cast<VarDeclStmt*>(s);
            analyzeExpr(d->init.get());
            if (d->isConst) {
                if (!d->init->isConst)
                    error(d->line, "initializer of constant '" + d->name + "' is not a constant expression");
                Symbol* sym = declare(d->name, Symbol::Kind::Const);
                sym->isConst = true;
                sym->constValue = d->init->constValue;
                d->sym = sym;
            } else {
                Symbol* sym = declare(d->name, Symbol::Kind::LocalVar);
                d->sym = sym;
            }
            break;
        }
        case Stmt::Kind::If: {
            auto* i = static_cast<IfStmt*>(s);
            analyzeExpr(i->cond.get());
            analyzeStmt(i->then_.get());
            if (i->else_) analyzeStmt(i->else_.get());
            break;
        }
        case Stmt::Kind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            analyzeExpr(w->cond.get());
            loopDepth_++;
            analyzeStmt(w->body.get());
            loopDepth_--;
            break;
        }
        case Stmt::Kind::Break:
            if (loopDepth_ == 0) error(s->line, "'break' outside of a loop");
            break;
        case Stmt::Kind::Continue:
            if (loopDepth_ == 0) error(s->line, "'continue' outside of a loop");
            break;
        case Stmt::Kind::Return:
            if (auto* r = static_cast<ReturnStmt*>(s); r->value)
                analyzeExpr(r->value.get());
            break;
    }
}

void Sema::analyzeExpr(Expr* e) {
    switch (e->kind) {
        case Expr::Kind::Number:
            break;  // already const
        case Expr::Kind::Var: {
            auto* v = static_cast<VarExpr*>(e);
            Symbol* sym = lookup(v->name);
            if (!sym) error(v->line, "use of undeclared identifier '" + v->name + "'");
            v->sym = sym;
            if (sym->isConst) { v->isConst = true; v->constValue = sym->constValue; }
            break;
        }
        case Expr::Kind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            analyzeExpr(b->lhs.get());
            analyzeExpr(b->rhs.get());
            if (b->lhs->isConst && b->rhs->isConst) {
                bool dz = false;
                auto r = evalBin(b->op, b->lhs->constValue, b->rhs->constValue, dz);
                if (dz) error(b->line, "division by zero in constant expression");
                if (r) { b->isConst = true; b->constValue = *r; }
            }
            break;
        }
        case Expr::Kind::Logical: {
            auto* l = static_cast<LogicalExpr*>(e);
            analyzeExpr(l->lhs.get());
            analyzeExpr(l->rhs.get());
            if (l->lhs->isConst) {
                int32_t lv = l->lhs->constValue;
                if (l->isOr && lv != 0) { l->isConst = true; l->constValue = 1; }
                else if (!l->isOr && lv == 0) { l->isConst = true; l->constValue = 0; }
                else if (l->rhs->isConst) { l->isConst = true; l->constValue = (l->rhs->constValue != 0) ? 1 : 0; }
            }
            break;
        }
        case Expr::Kind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            analyzeExpr(u->operand.get());
            if (u->operand->isConst) { u->isConst = true; u->constValue = evalUn(u->op, u->operand->constValue); }
            break;
        }
        case Expr::Kind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            auto it = funcs_.find(c->callee);
            if (it == funcs_.end())
                error(c->line, "call to undeclared function '" + c->callee + "'");
            c->target = it->second;
            if (c->args.size() != c->target->params.size())
                error(c->line, "function '" + c->callee + "' expects " +
                      std::to_string(c->target->params.size()) + " argument(s)");
            for (auto& a : c->args) analyzeExpr(a.get());
            break;
        }
    }
}

} // namespace toyc

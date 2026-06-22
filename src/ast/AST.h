#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace toyc {

// ----- operators -------------------------------------------------------------
enum class BinOp { Add, Sub, Mul, Div, Mod, Lt, Gt, Le, Ge, Eq, Ne };
enum class UnOp  { Plus, Neg, Not };

// ----- symbols (filled in by Sema) ------------------------------------------
struct FuncDef;
struct Symbol {
    enum class Kind { GlobalVar, LocalVar, Param, Const };
    Kind kind = Kind::LocalVar;
    std::string name;
    bool isConst = false;
    int32_t constValue = 0;   // valid when isConst
    int globalId = -1;        // index among global variables (GlobalVar)
    int irId = -1;            // virtual-register / slot id assigned during IR gen
};

// ============================ Expressions ====================================
struct Expr {
    enum class Kind { Number, Var, Binary, Logical, Unary, Call };
    Kind kind;
    int line = 0;
    // Set by Sema when the whole expression folds to a compile-time constant.
    bool isConst = false;
    int32_t constValue = 0;
    explicit Expr(Kind k) : kind(k) {}
    virtual ~Expr() = default;
};
using ExprPtr = std::unique_ptr<Expr>;

struct NumberExpr : Expr {
    int32_t value;
    explicit NumberExpr(int32_t v) : Expr(Kind::Number), value(v) { isConst = true; constValue = v; }
};

struct VarExpr : Expr {
    std::string name;
    Symbol* sym = nullptr;   // resolved by Sema
    explicit VarExpr(std::string n) : Expr(Kind::Var), name(std::move(n)) {}
};

struct BinaryExpr : Expr {
    BinOp op;
    ExprPtr lhs, rhs;
    BinaryExpr(BinOp o, ExprPtr l, ExprPtr r)
        : Expr(Kind::Binary), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

// Short-circuit && / ||  (kept separate from BinaryExpr).
struct LogicalExpr : Expr {
    bool isOr;
    ExprPtr lhs, rhs;
    LogicalExpr(bool orr, ExprPtr l, ExprPtr r)
        : Expr(Kind::Logical), isOr(orr), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct UnaryExpr : Expr {
    UnOp op;
    ExprPtr operand;
    UnaryExpr(UnOp o, ExprPtr e) : Expr(Kind::Unary), op(o), operand(std::move(e)) {}
};

struct CallExpr : Expr {
    std::string callee;
    std::vector<ExprPtr> args;
    FuncDef* target = nullptr;   // resolved by Sema
    explicit CallExpr(std::string c) : Expr(Kind::Call), callee(std::move(c)) {}
};

// ============================ Statements =====================================
struct Stmt {
    enum class Kind { Block, Empty, ExprStmt, Assign, VarDecl, If, While, Break, Continue, Return };
    Kind kind;
    int line = 0;
    explicit Stmt(Kind k) : kind(k) {}
    virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;

struct BlockStmt : Stmt {
    std::vector<StmtPtr> stmts;
    BlockStmt() : Stmt(Kind::Block) {}
};

struct EmptyStmt : Stmt { EmptyStmt() : Stmt(Kind::Empty) {} };

struct ExprStmt : Stmt {
    ExprPtr expr;
    explicit ExprStmt(ExprPtr e) : Stmt(Kind::ExprStmt), expr(std::move(e)) {}
};

struct AssignStmt : Stmt {
    std::string name;
    Symbol* sym = nullptr;   // resolved by Sema
    ExprPtr value;
    AssignStmt(std::string n, ExprPtr v) : Stmt(Kind::Assign), name(std::move(n)), value(std::move(v)) {}
};

struct VarDeclStmt : Stmt {
    bool isConst;
    std::string name;
    ExprPtr init;
    Symbol* sym = nullptr;   // created by Sema
    VarDeclStmt(bool c, std::string n, ExprPtr i)
        : Stmt(Kind::VarDecl), isConst(c), name(std::move(n)), init(std::move(i)) {}
};

struct IfStmt : Stmt {
    ExprPtr cond;
    StmtPtr then_, else_;     // else_ may be null
    IfStmt() : Stmt(Kind::If) {}
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    StmtPtr body;
    WhileStmt() : Stmt(Kind::While) {}
};

struct BreakStmt : Stmt { BreakStmt() : Stmt(Kind::Break) {} };
struct ContinueStmt : Stmt { ContinueStmt() : Stmt(Kind::Continue) {} };

struct ReturnStmt : Stmt {
    ExprPtr value;            // may be null (void return)
    ReturnStmt() : Stmt(Kind::Return) {}
};

// ============================ Top level ======================================
struct Param {
    std::string name;
    Symbol* sym = nullptr;
};

struct FuncDef {
    bool returnsVoid = false;
    std::string name;
    std::vector<Param> params;
    std::unique_ptr<BlockStmt> body;
    int line = 0;
};

struct GlobalVar {
    bool isConst = false;
    std::string name;
    ExprPtr init;
    Symbol* sym = nullptr;
    int32_t initValue = 0;    // computed by Sema (all global inits are constant)
    int line = 0;
};

// A top-level item preserves source order (needed for "use after declaration").
struct TopLevel {
    enum class Kind { Global, Func } kind;
    std::unique_ptr<GlobalVar> global;
    std::unique_ptr<FuncDef> func;
};

struct Module {
    std::vector<TopLevel> items;
};

} // namespace toyc

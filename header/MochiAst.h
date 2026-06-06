#include "MochiInclude.h"
#include "MochiLexer.h"

#ifndef MOCHIAST
#define MOCHIAST


namespace Mochi
{
    // ============================================================
// 2. AST 节点定义
// ============================================================
    struct Expr;
    struct Stmt;

    using ExprPtr = std::shared_ptr<Expr>;
    using StmtPtr = std::shared_ptr<Stmt>;

    // --- 表达式 ---
    struct Assign;
    struct Binary;
    struct Call;
    struct Get;
    struct Set;
    struct Super;
    struct This;
    struct Literal;
    struct Variable;
    struct Grouping;
    struct Unary;
    struct FunctionExpr;
    struct ArrayLiteral;
    struct DictLiteral;
    struct IndexGet;
    struct IndexSet;


    struct Expr { virtual ~Expr() = default; };

    struct Assign : Expr {
        std::string name;
        ExprPtr value;
        Assign(std::string n, ExprPtr v) : name(std::move(n)), value(std::move(v)) {}
    };

    struct Binary : Expr {
        ExprPtr left;
        Token op;
        ExprPtr right;
        Binary(ExprPtr l, Token o, ExprPtr r) : left(std::move(l)), op(std::move(o)), right(std::move(r)) {}
    };

    struct Call : Expr {
        ExprPtr callee;
        Token paren;
        std::vector<ExprPtr> arguments;
        Call(ExprPtr c, Token p, std::vector<ExprPtr> a)
            : callee(std::move(c)), paren(std::move(p)), arguments(std::move(a)) {
        }
    };

    struct Get : Expr {
        ExprPtr object;
        std::string name;
        Get(ExprPtr o, std::string n) : object(std::move(o)), name(std::move(n)) {}
    };

    struct Set : Expr {
        ExprPtr object;
        std::string name;
        ExprPtr value;
        Set(ExprPtr o, std::string n, ExprPtr v)
            : object(std::move(o)), name(std::move(n)), value(std::move(v)) {
        }
    };

    struct Super : Expr {
        std::string method;
        Super(std::string m) : method(std::move(m)) {}
    };

    struct This : Expr {};

    struct Literal : Expr {
        std::any value;
        explicit Literal(std::any v) : value(std::move(v)) {}
    };

    struct Variable : Expr {
        std::string name;
        explicit Variable(std::string n) : name(std::move(n)) {}
    };

    struct Grouping : Expr {
        ExprPtr expression;
        explicit Grouping(ExprPtr e) : expression(std::move(e)) {}
    };

    struct Unary : Expr {
        Token op;
        ExprPtr right;
        Unary(Token o, ExprPtr r) : op(std::move(o)), right(std::move(r)) {}
    };

    struct FunctionExpr : Expr {
        std::vector<std::string> params;
        std::vector<StmtPtr> body;
        FunctionExpr(std::vector<std::string> p, std::vector<StmtPtr> b)
            : params(std::move(p)), body(std::move(b)) {
        }
    };

    struct ArrayLiteral : Expr {
        std::vector<ExprPtr> elements;
        explicit ArrayLiteral(std::vector<ExprPtr> e) : elements(std::move(e)) {}
    };

    struct DictLiteral : Expr {
        std::vector<std::pair<std::string, ExprPtr>> entries;
        explicit DictLiteral(std::vector<std::pair<std::string, ExprPtr>> e) : entries(std::move(e)) {}
    };

    struct IndexGet : Expr {
        ExprPtr object;
        ExprPtr index;
        IndexGet(ExprPtr o, ExprPtr i) : object(std::move(o)), index(std::move(i)) {}
    };

    struct IndexSet : Expr {
        ExprPtr object;
        ExprPtr index;
        ExprPtr value;
        IndexSet(ExprPtr o, ExprPtr i, ExprPtr v)
            : object(std::move(o)), index(std::move(i)), value(std::move(v)) {
        }
    };

    // --- 语句 ---
    struct ExpressionStmt;
    struct PrintStmt;
    struct VarStmt;
    struct BlockStmt;
    struct IfStmt;
    struct WhileStmt;
    struct ForStmt;
    struct ReturnStmt;
    struct ClassStmt;
    struct FunctionStmt;
    struct ForInStmt;
    struct TryStmt;
    struct ThrowStmt;

    struct Stmt { virtual ~Stmt() = default; };

    // 空;处理
    struct EmptyStmt : Stmt {};

    struct BreakStmt : Stmt {};
    struct ContinueStmt : Stmt {};

    struct ExpressionStmt : Stmt {
        ExprPtr expression;
        explicit ExpressionStmt(ExprPtr e) : expression(std::move(e)) {}
    };

    struct PrintStmt : Stmt {
        ExprPtr expression;
        explicit PrintStmt(ExprPtr e) : expression(std::move(e)) {}
    };

    struct VarStmt : Stmt {
        std::string name;
        ExprPtr initializer;
        VarStmt(std::string n, ExprPtr i) : name(std::move(n)), initializer(std::move(i)) {}
    };

    struct BlockStmt : Stmt {
        std::vector<StmtPtr> statements;
        explicit BlockStmt(std::vector<StmtPtr> s) : statements(std::move(s)) {}
    };

    struct IfStmt : Stmt {
        ExprPtr condition;
        StmtPtr thenBranch;
        StmtPtr elseBranch;
        IfStmt(ExprPtr c, StmtPtr t, StmtPtr e)
            : condition(std::move(c)), thenBranch(std::move(t)), elseBranch(std::move(e)) {
        }
    };

    struct WhileStmt : Stmt {
        ExprPtr condition;
        StmtPtr body;
        WhileStmt(ExprPtr c, StmtPtr b) : condition(std::move(c)), body(std::move(b)) {}
    };

    struct ForStmt : Stmt {
        StmtPtr initializer;
        ExprPtr condition;
        ExprPtr increment;
        StmtPtr body;
        ForStmt(StmtPtr i, ExprPtr c, ExprPtr inc, StmtPtr b)
            : initializer(std::move(i)), condition(std::move(c)), increment(std::move(inc)), body(std::move(b)) {
        }
    };

    struct ReturnStmt : Stmt {
        Token keyword;
        ExprPtr value;
        ReturnStmt(Token k, ExprPtr v) : keyword(std::move(k)), value(std::move(v)) {}
    };

    struct FunctionStmt : Stmt {
        std::string name;
        std::vector<std::string> params;
        std::vector<StmtPtr> body;
        FunctionStmt(std::string n, std::vector<std::string> p, std::vector<StmtPtr> b)
            : name(std::move(n)), params(std::move(p)), body(std::move(b)) {
        }
    };

    struct ClassStmt : Stmt {
        std::string name;
        std::string superClass;
        std::vector<FunctionStmt> methods;
        ClassStmt(std::string n, std::string s, std::vector<FunctionStmt> m)
            : name(std::move(n)), superClass(std::move(s)), methods(std::move(m)) {
        }
    };

    struct ForInStmt : Stmt {
        std::string varName;
        ExprPtr iterable;
        StmtPtr body;
        ForInStmt(std::string vn, ExprPtr it, StmtPtr b)
            : varName(std::move(vn)), iterable(std::move(it)), body(std::move(b)) {
        }
    };

    struct TryStmt : Stmt {
        StmtPtr tryBody;
        std::string catchVar;
        StmtPtr catchBody;
        TryStmt(StmtPtr t, std::string cv, StmtPtr cb)
            : tryBody(std::move(t)), catchVar(std::move(cv)), catchBody(std::move(cb)) {
        }
    };

    struct ThrowStmt : Stmt {
        ExprPtr value;
        explicit ThrowStmt(ExprPtr v) : value(std::move(v)) {}
    };

}

#endif // !MOCHIAST
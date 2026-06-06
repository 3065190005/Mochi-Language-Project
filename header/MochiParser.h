#include "MochiInclude.h"
#include "MochiLexer.h"
#include "MochiAst.h"

#ifndef MOCHIPARSER
#define MOCHIPARSER

namespace Mochi
{
    class Parser {
        std::vector<Token> tokens;
        size_t current = 0;
    public:
        explicit Parser(std::vector<Token> t) : tokens(std::move(t)) {}
        std::vector<StmtPtr> parse();

    private:
        bool isAtEnd() const { return peek().type == TokenType::END_OF_FILE; }
        Token peek() const { return tokens[current]; }
        Token previous() const { return tokens[current - 1]; }
        Token advance() { if (!isAtEnd()) current++; return previous(); }
        bool check(TokenType type) const { return isAtEnd() ? false : peek().type == type; }
        bool match(std::initializer_list<TokenType> types);
        Token consume(TokenType type, const std::string& message);

        StmtPtr declaration();

        StmtPtr classDeclaration();

        StmtPtr function(const std::string& kind);

        StmtPtr varDeclaration();

        StmtPtr statement();

        StmtPtr ifStatement();

        StmtPtr whileStatement();

        StmtPtr forStatement();

        StmtPtr returnStatement();

        StmtPtr tryStatement();

        StmtPtr throwStatement();

        StmtPtr expressionStatement();
        std::vector<StmtPtr> block();

        ExprPtr expression() { return assignment(); }

        ExprPtr assignment();

        ExprPtr orExpr();

        ExprPtr andExpr();

        ExprPtr equality();

        ExprPtr comparison();

        ExprPtr addition();

        ExprPtr multiplication();

        ExprPtr unary();

        ExprPtr call();

        ExprPtr finishCall(ExprPtr callee);

        ExprPtr primary();
    };

}

#endif
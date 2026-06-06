#include "MochiParser.h"

namespace Mochi {
        std::vector<StmtPtr> Parser::parse() {
            std::vector<StmtPtr> statements;
            while (!isAtEnd()) {
                statements.push_back(declaration());
            }
            return statements;
        }
    
        bool Parser::match(std::initializer_list<TokenType> types) {
            for (auto type : types) {
                if (check(type)) { advance(); return true; }
            }
            return false;
        }
        Token Parser::consume(TokenType type, const std::string& message) {
            if (check(type)) return advance();
            throw std::runtime_error(message + " at line " + std::to_string(peek().line));
        }

        StmtPtr Parser::declaration() {
            if (match({ TokenType::CLASS })) return classDeclaration();
            if (match({ TokenType::FN })) return function("function");
            if (match({ TokenType::VAR })) return varDeclaration();
            return statement();
        }

        StmtPtr Parser::classDeclaration() {
            std::string name = consume(TokenType::IDENTIFIER, "Expect class name.").lexeme;
            std::string superClass;
            if (match({ TokenType::COLON })) {
                superClass = consume(TokenType::IDENTIFIER, "Expect superclass name.").lexeme;
            }
            consume(TokenType::LEFT_BRACE, "Expect '{' before class body.");
            std::vector<FunctionStmt> methods;
            while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
                /* 修改 */
                auto method_ptr = function("method");
                auto method_cast = dynamic_cast<FunctionStmt*>(method_ptr.get());
                methods.push_back(*method_cast);
            }
            consume(TokenType::RIGHT_BRACE, "Expect '}' after class body.");
            return std::make_unique<ClassStmt>(name, superClass, std::move(methods));
        }

        StmtPtr Parser::function(const std::string& kind) {
            std::string name = consume(TokenType::IDENTIFIER, "Expect " + kind + " name.").lexeme;
            consume(TokenType::LEFT_PAREN, "Expect '(' after " + kind + " name.");
            std::vector<std::string> parameters;
            if (!check(TokenType::RIGHT_PAREN)) {
                do {
                    parameters.push_back(consume(TokenType::IDENTIFIER, "Expect parameter name.").lexeme);
                } while (match({ TokenType::COMMA }));
            }
            consume(TokenType::RIGHT_PAREN, "Expect ')' after parameters.");
            consume(TokenType::LEFT_BRACE, "Expect '{' before " + kind + " body.");
            std::vector<StmtPtr> body = block();
            return std::make_unique<FunctionStmt>(name, parameters, std::move(body));
        }

        StmtPtr Parser::varDeclaration() {
            std::string name = consume(TokenType::IDENTIFIER, "Expect variable name.").lexeme;
            ExprPtr initializer = nullptr;
            if (match({ TokenType::ASSIGN })) initializer = expression();
            consume(TokenType::SEMICOLON, "Expect ';' after variable declaration.");
            return std::make_unique<VarStmt>(name, std::move(initializer));
        }

        StmtPtr Parser::statement() {
            if (match({ TokenType::SEMICOLON })) {
                return std::make_unique<EmptyStmt>();
            }
            if (match({ TokenType::BREAK })) {
                consume(TokenType::SEMICOLON, "Expect ';' after 'break'.");
                return std::make_unique<BreakStmt>();
            }
            if (match({ TokenType::CONTINUE })) {
                consume(TokenType::SEMICOLON, "Expect ';' after 'continue'.");
                return std::make_unique<ContinueStmt>();
            }
            if (match({ TokenType::IF })) return ifStatement();
            if (match({ TokenType::WHILE })) return whileStatement();
            if (match({ TokenType::FOR })) return forStatement();
            if (match({ TokenType::RETURN })) return returnStatement();
            if (match({ TokenType::TRY })) return tryStatement();
            if (match({ TokenType::THROW })) return throwStatement();
            if (match({ TokenType::LEFT_BRACE })) return std::make_unique<BlockStmt>(block());
            return expressionStatement();
        }

        StmtPtr Parser::ifStatement() {
            consume(TokenType::LEFT_PAREN, "Expect '(' after 'if'.");
            ExprPtr condition = expression();
            consume(TokenType::RIGHT_PAREN, "Expect ')' after if condition.");
            StmtPtr thenBranch = statement();
            StmtPtr elseBranch = nullptr;
            if (match({ TokenType::ELSE })) elseBranch = statement();
            return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
        }

        StmtPtr Parser::whileStatement() {
            consume(TokenType::LEFT_PAREN, "Expect '(' after 'while'.");
            ExprPtr condition = expression();
            consume(TokenType::RIGHT_PAREN, "Expect ')' after condition.");
            StmtPtr body = statement();
            return std::make_unique<WhileStmt>(std::move(condition), std::move(body));
        }

        StmtPtr Parser::forStatement() {
            consume(TokenType::LEFT_PAREN, "Expect '(' after 'for'.");
            // 判断是 for-in 还是传统 for
            bool isForIn = false;
            size_t saved = current;
            if (check(TokenType::VAR)) {
                advance();
                if (check(TokenType::IDENTIFIER)) {
                    advance();
                    isForIn = check(TokenType::IN);
                }
            }
            current = saved;
            if (isForIn) {
                consume(TokenType::VAR, "");
                std::string varName = consume(TokenType::IDENTIFIER, "Expect variable name.").lexeme;
                consume(TokenType::IN, "Expect 'in' after variable.");
                ExprPtr iterable = expression();
                consume(TokenType::RIGHT_PAREN, "Expect ')' after for-in expression.");
                StmtPtr body = statement();
                return std::make_unique<ForInStmt>(varName, std::move(iterable), std::move(body));
            }
            // 传统 for
            StmtPtr initializer;
            if (match({ TokenType::SEMICOLON })) {
                initializer = nullptr;
            }
            else if (match({ TokenType::VAR })) {
                initializer = varDeclaration();
            }
            else {
                initializer = expressionStatement();
            }
            ExprPtr condition = nullptr;
            if (!check(TokenType::SEMICOLON)) condition = expression();
            consume(TokenType::SEMICOLON, "Expect ';' after loop condition.");
            ExprPtr increment = nullptr;
            if (!check(TokenType::RIGHT_PAREN)) increment = expression();
            consume(TokenType::RIGHT_PAREN, "Expect ')' after for clauses.");
            StmtPtr body = statement();
            return std::make_unique<ForStmt>(std::move(initializer), std::move(condition),
                std::move(increment), std::move(body));
        }

        StmtPtr Parser::returnStatement() {
            Token keyword = previous();
            ExprPtr value = nullptr;
            if (!check(TokenType::SEMICOLON)) value = expression();
            consume(TokenType::SEMICOLON, "Expect ';' after return value.");
            return std::make_unique<ReturnStmt>(keyword, std::move(value));
        }

        StmtPtr Parser::tryStatement() {
            consume(TokenType::LEFT_BRACE, "Expect '{' after 'try'.");
            StmtPtr tryBody = std::make_unique<BlockStmt>(block());
            consume(TokenType::CATCH, "Expect 'catch' after try block.");
            consume(TokenType::LEFT_PAREN, "Expect '(' after 'catch'.");
            std::string catchVar = consume(TokenType::IDENTIFIER, "Expect catch variable name.").lexeme;
            consume(TokenType::RIGHT_PAREN, "Expect ')' after catch variable.");
            consume(TokenType::LEFT_BRACE, "Expect '{' after catch clause.");
            StmtPtr catchBody = std::make_unique<BlockStmt>(block());
            return std::make_unique<TryStmt>(std::move(tryBody), catchVar, std::move(catchBody));
        }

        StmtPtr Parser::throwStatement() {
            ExprPtr value = expression();
            consume(TokenType::SEMICOLON, "Expect ';' after throw value.");
            return std::make_unique<ThrowStmt>(std::move(value));
        }

        StmtPtr Parser::expressionStatement() {
            ExprPtr expr = expression();
            consume(TokenType::SEMICOLON, "Expect ';' after expression.");
            return std::make_unique<ExpressionStmt>(std::move(expr));
        }

        std::vector<StmtPtr> Parser::block() {
            std::vector<StmtPtr> statements;
            while (!check(TokenType::RIGHT_BRACE) && !isAtEnd()) {
                statements.push_back(declaration());
            }
            consume(TokenType::RIGHT_BRACE, "Expect '}' after block.");
            return statements;
        }

        ExprPtr Parser::assignment() {
            ExprPtr expr = orExpr();
            if (match({ TokenType::ASSIGN })) {
                Token equals = previous();
                ExprPtr value = assignment();
                if (auto* var = dynamic_cast<Variable*>(expr.get())) {
                    return std::make_unique<Assign>(var->name, std::move(value));
                }
                else if (auto* get = dynamic_cast<Get*>(expr.get())) {
                    return std::make_unique<Set>(std::move(get->object), get->name, std::move(value));
                }
                else if (auto* idx = dynamic_cast<IndexGet*>(expr.get())) {
                    return std::make_unique<IndexSet>(std::move(idx->object), std::move(idx->index), std::move(value));
                }
                throw std::runtime_error("Invalid assignment target.");
            }
            return expr;
        }

        ExprPtr Parser::orExpr() {
            ExprPtr expr = andExpr();
            while (match({ TokenType::OR })) {
                Token op = previous();
                ExprPtr right = andExpr();
                expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
            }
            return expr;
        }

        ExprPtr Parser::andExpr() {
            ExprPtr expr = equality();
            while (match({ TokenType::AND })) {
                Token op = previous();
                ExprPtr right = equality();
                expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
            }
            return expr;
        }

        ExprPtr Parser::equality() {
            ExprPtr expr = comparison();
            while (match({ TokenType::EQUAL_EQUAL, TokenType::BANG_EQUAL })) {
                Token op = previous();
                ExprPtr right = comparison();
                expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
            }
            return expr;
        }

        ExprPtr Parser::comparison() {
            ExprPtr expr = addition();
            while (match({ TokenType::GREATER, TokenType::GREATER_EQUAL,
                          TokenType::LESS, TokenType::LESS_EQUAL })) {
                Token op = previous();
                ExprPtr right = addition();
                expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
            }
            return expr;
        }

        ExprPtr Parser::addition() {
            ExprPtr expr = multiplication();
            while (match({ TokenType::PLUS, TokenType::MINUS })) {
                Token op = previous();
                ExprPtr right = multiplication();
                expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
            }
            return expr;
        }

        ExprPtr Parser::multiplication() {
            ExprPtr expr = unary();
            while (match({ TokenType::STAR, TokenType::SLASH, TokenType::MOD })) {
                Token op = previous();
                ExprPtr right = unary();
                expr = std::make_unique<Binary>(std::move(expr), op, std::move(right));
            }
            return expr;
        }

        ExprPtr Parser::unary() {
            if (match({ TokenType::BANG, TokenType::MINUS })) {
                Token op = previous();
                ExprPtr right = unary();
                return std::make_unique<Unary>(op, std::move(right));
            }
            return call();
        }

        ExprPtr Parser::call() {
            ExprPtr expr = primary();
            while (true) {
                if (match({ TokenType::LEFT_PAREN })) {
                    expr = finishCall(std::move(expr));
                }
                else if (match({ TokenType::DOT })) {
                    std::string name = consume(TokenType::IDENTIFIER, "Expect property name after '.'.").lexeme;
                    expr = std::make_unique<Get>(std::move(expr), name);
                }
                else if (match({ TokenType::LEFT_BRACKET })) {
                    ExprPtr index = expression();
                    consume(TokenType::RIGHT_BRACKET, "Expect ']' after index.");
                    expr = std::make_unique<IndexGet>(std::move(expr), std::move(index));
                }
                else {
                    break;
                }
            }
            return expr;
        }

        ExprPtr Parser::finishCall(ExprPtr callee) {
            std::vector<ExprPtr> arguments;
            if (!check(TokenType::RIGHT_PAREN)) {
                do {
                    arguments.push_back(expression());
                } while (match({ TokenType::COMMA }));
            }
            Token paren = consume(TokenType::RIGHT_PAREN, "Expect ')' after arguments.");
            return std::make_unique<Call>(std::move(callee), paren, std::move(arguments));
        }

        ExprPtr Parser::primary() {
            if (match({ TokenType::TRUE })) return std::make_unique<Literal>(true);
            if (match({ TokenType::FALSE })) return std::make_unique<Literal>(false);
            if (match({ TokenType::NIL })) return std::make_unique<Literal>(std::any{});
            if (match({ TokenType::NUMBER })) return std::make_unique<Literal>(previous().literal);
            if (match({ TokenType::STRING })) return std::make_unique<Literal>(previous().literal);
            if (match({ TokenType::SUPER })) {
                consume(TokenType::DOT, "Expect '.' after 'super'.");
                std::string method = consume(TokenType::IDENTIFIER, "Expect superclass method name.").lexeme;
                return std::make_unique<Super>(method);
            }
            if (match({ TokenType::THIS })) return std::make_unique<This>();
            if (match({ TokenType::IDENTIFIER })) return std::make_unique<Variable>(previous().lexeme);
            if (match({ TokenType::FN })) {
                consume(TokenType::LEFT_PAREN, "Expect '(' after 'fn'.");
                std::vector<std::string> params;
                if (!check(TokenType::RIGHT_PAREN)) {
                    do {
                        params.push_back(consume(TokenType::IDENTIFIER, "Expect parameter name.").lexeme);
                    } while (match({ TokenType::COMMA }));
                }
                consume(TokenType::RIGHT_PAREN, "Expect ')' after parameters.");
                consume(TokenType::LEFT_BRACE, "Expect '{' before function body.");
                std::vector<StmtPtr> body = block();
                return std::make_unique<FunctionExpr>(params, std::move(body));
            }
            if (match({ TokenType::LEFT_BRACKET })) {
                std::vector<ExprPtr> elements;
                if (!check(TokenType::RIGHT_BRACKET)) {
                    do {
                        elements.push_back(expression());
                    } while (match({ TokenType::COMMA }));
                }
                consume(TokenType::RIGHT_BRACKET, "Expect ']' after array literal.");
                return std::make_unique<ArrayLiteral>(std::move(elements));
            }
            if (match({ TokenType::LEFT_BRACE })) {
                std::vector<std::pair<std::string, ExprPtr>> entries;
                if (!check(TokenType::RIGHT_BRACE)) {
                    do {
                        std::string key;
                        if (match({ TokenType::STRING })) {
                            key = std::any_cast<std::string>(previous().literal);
                        }
                        else {
                            key = consume(TokenType::IDENTIFIER, "Expect dictionary key.").lexeme;
                        }
                        consume(TokenType::COLON, "Expect ':' after dictionary key.");
                        ExprPtr value = expression();
                        entries.emplace_back(key, std::move(value));
                    } while (match({ TokenType::COMMA }));
                }
                consume(TokenType::RIGHT_BRACE, "Expect '}' after dictionary literal.");
                return std::make_unique<DictLiteral>(std::move(entries));
            }
            if (match({ TokenType::LEFT_PAREN })) {
                ExprPtr expr = expression();
                consume(TokenType::RIGHT_PAREN, "Expect ')' after expression.");
                return std::make_unique<Grouping>(std::move(expr));
            }
            throw std::runtime_error("Expect expression at line " + std::to_string(peek().line));
        }
}
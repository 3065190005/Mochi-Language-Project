#include "MochiInclude.h"
#pragma once
#ifndef MOCHILEXER
#define MOCHILEXER


namespace Mochi
{
    // ============================================================
    //  词法分析器 (Lexer)
    // ============================================================
    enum class TokenType {
        // 关键字
        CLASS, FN, VAR, IF, ELSE, WHILE, FOR, RETURN,
        SUPER, THIS, TRUE, FALSE, NIL, AND, OR,
        IN, TRY, CATCH, THROW, BREAK, CONTINUE, MOD,
        // 字面量
        IDENTIFIER, NUMBER, STRING,
        // 运算符和分隔符
        PLUS, MINUS, STAR, SLASH, BANG, EQUAL, EQUAL_EQUAL,
        BANG_EQUAL, GREATER, GREATER_EQUAL, LESS, LESS_EQUAL,
        LEFT_PAREN, RIGHT_PAREN, LEFT_BRACE, RIGHT_BRACE,
        LEFT_BRACKET, RIGHT_BRACKET,
        COMMA, DOT, COLON, SEMICOLON, ASSIGN,
        END_OF_FILE
    };

    struct Token {
        TokenType type;
        std::string lexeme;
        std::any literal;
        int line;
        Token(TokenType t, std::string l, std::any lit, int ln)
            : type(t), lexeme(std::move(l)), literal(std::move(lit)), line(ln) {
        }
    };

    class Lexer {
        std::string source;
        size_t start = 0, current = 0;
        int line = 1;
        static const std::map<std::string, TokenType> keywords;

    public:
        explicit Lexer(std::string src) : source(std::move(src)) {}

        std::vector<Token> scanTokens();

    private:
        bool isAtEnd() const { return current >= source.size(); }

        void scanToken(std::vector<Token>& tokens);

        char advance() { return source[current++]; }
        char peek() const { return isAtEnd() ? '\0' : source[current]; }
        char peekNext() const { return (current + 1 >= source.size()) ? '\0' : source[current + 1]; }

        bool match(char expected);

        void addToken(std::vector<Token>& tokens, TokenType type, std::any literal = {});

        void stringLiteral(std::vector<Token>& tokens);

        void numberLiteral(std::vector<Token>& tokens);

        void identifier(std::vector<Token>& tokens);

        static bool isDigit(char c) { return c >= '0' && c <= '9'; }
        static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
        static bool isAlphaNumeric(char c) { return isAlpha(c) || isDigit(c); }
    };

}

#endif // !MOCHILEXER
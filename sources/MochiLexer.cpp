#include "MochiLexer.h"

namespace Mochi
{
    // ============================================================
   //  词法分析器 (Lexer)
   // ============================================================
   

    const std::map<std::string, TokenType> Lexer::keywords = {
        {"class", TokenType::CLASS}, {"fn", TokenType::FN}, {"loc", TokenType::VAR},
        {"if", TokenType::IF}, {"else", TokenType::ELSE}, {"while", TokenType::WHILE},
        {"for", TokenType::FOR}, {"return", TokenType::RETURN}, {"super", TokenType::SUPER},
        {"this", TokenType::THIS}, {"true", TokenType::TRUE}, {"false", TokenType::FALSE},
        {"nil", TokenType::NIL}, {"and", TokenType::AND}, {"or", TokenType::OR},
        {"in", TokenType::IN}, {"try", TokenType::TRY}, {"catch", TokenType::CATCH},
        {"throw", TokenType::THROW},{"break", TokenType::BREAK},{"continue", TokenType::CONTINUE}
    };

    std::vector<Token> Lexer::scanTokens() {
        std::vector<Token> tokens;
        while (!isAtEnd()) {
            start = current;
            scanToken(tokens);
        }
        tokens.emplace_back(TokenType::END_OF_FILE, "", std::any{}, line);
        return tokens;
    }

    void Lexer::scanToken(std::vector<Token>& tokens) {
        char c = advance();
        switch (c) {
        case '(': addToken(tokens, TokenType::LEFT_PAREN); break;
        case ')': addToken(tokens, TokenType::RIGHT_PAREN); break;
        case '{': addToken(tokens, TokenType::LEFT_BRACE); break;
        case '}': addToken(tokens, TokenType::RIGHT_BRACE); break;
        case '[': addToken(tokens, TokenType::LEFT_BRACKET); break;
        case ']': addToken(tokens, TokenType::RIGHT_BRACKET); break;
        case ',': addToken(tokens, TokenType::COMMA); break;
        case '.': addToken(tokens, TokenType::DOT); break;
        case ':': addToken(tokens, TokenType::COLON); break;
        case ';': addToken(tokens, TokenType::SEMICOLON); break;
        case '+': addToken(tokens, TokenType::PLUS); break;
        case '-': addToken(tokens, TokenType::MINUS); break;
        case '%': addToken(tokens, TokenType::MOD); break;
        case '*': addToken(tokens, TokenType::STAR); break;
        case '/':
            if (match('/')) {
                // 单行注释，跳过直到行尾
                while (peek() != '\n' && !isAtEnd()) advance();
            }
            else if (match('*')) {
                // 多行注释，跳过直到 "*/"
                while (!isAtEnd()) {
                    if (peek() == '*' && peekNext() == '/') {
                        advance(); // 消费 '*'
                        advance(); // 消费 '/'
                        break;
                    }
                    if (peek() == '\n') line++;
                    advance();
                }
                if (isAtEnd()) {
                    throw std::runtime_error("Unterminated block comment at line " + std::to_string(line));
                }
            }
            else {
                addToken(tokens, TokenType::SLASH);
            }
            break;
        case '!':
            addToken(tokens, match('=') ? TokenType::BANG_EQUAL : TokenType::BANG);
            break;
        case '=':
            addToken(tokens, match('=') ? TokenType::EQUAL_EQUAL : TokenType::ASSIGN);
            break;
        case '>':
            addToken(tokens, match('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
            break;
        case '<':
            addToken(tokens, match('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
            break;
        case '"': stringLiteral(tokens); break;
        case ' ': case '\r': case '\t': break;
        case '\n': line++; break;
        default:
            if (isDigit(c)) {
                numberLiteral(tokens);
            }
            else if (isAlpha(c)) {
                identifier(tokens);
            }
            else {
                throw std::runtime_error("Unexpected character at line " + std::to_string(line));
            }
        }
    }

    bool Lexer::match(char expected) {
        if (isAtEnd() || source[current] != expected) return false;
        current++;
        return true;
    }

    void Lexer::addToken(std::vector<Token>& tokens, TokenType type, std::any literal) {
        std::string text = source.substr(start, current - start);
        tokens.emplace_back(type, text, std::move(literal), line);
    }

    void Lexer::stringLiteral(std::vector<Token>& tokens) {
        std::string value;
        while (peek() != '"' && !isAtEnd()) {
            if (peek() == '\n') line++;
            if (peek() == '\\') {
                advance(); // 消费反斜杠
                switch (peek()) {
                case 'n': value += '\n'; break;
                case 't': value += '\t'; break;
                case 'r': value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"': value += '"'; break;
                case '0': value += '\0'; break;
                default:
                    // 未识别的转义，保留原样（或报错）
                    value += '\\';
                    value += peek();
                    break;
                }
                advance();
            }
            else {
                value += advance();
            }
        }
        if (isAtEnd()) throw std::runtime_error("Unterminated string at line " + std::to_string(line));
        advance(); // 消费闭合引号
        addToken(tokens, TokenType::STRING, std::string(value));
    }

    void Lexer::numberLiteral(std::vector<Token>& tokens) {
        while (isDigit(peek())) advance();
        if (peek() == '.' && isDigit(peekNext())) {
            advance();
            while (isDigit(peek())) advance();
        }
        double value = std::stod(source.substr(start, current - start));
        addToken(tokens, TokenType::NUMBER, value);
    }

    void Lexer::identifier(std::vector<Token>& tokens) {
        while (isAlphaNumeric(peek())) advance();
        std::string text = source.substr(start, current - start);
        auto it = keywords.find(text);
        TokenType type = (it != keywords.end()) ? it->second : TokenType::IDENTIFIER;
        addToken(tokens, type);
    }
}
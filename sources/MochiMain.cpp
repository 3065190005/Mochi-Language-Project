#include "MochiMain.h"
#include "MochiInterpreter.h"

namespace Mochi
{
    // ============================================================
    // 运行入口
    // ============================================================

    void run(const std::string& source) {

        try {
            Lexer lexer(source);
            auto tokens = lexer.scanTokens();
            Parser parser(std::move(tokens));
            auto statements = parser.parse();
            auto interpreter = std::make_shared<Interpreter>();
            interpreter->interpret(statements);
        }

        catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
        catch (...) {
            std::cerr << "Unknown error occurred." << std::endl;
        }
    }

    void runFile(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Could not open file: " << path << std::endl;
            return;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();
        run(source);
    }

    //// ============================================================
    //// REPL 与主入口
    //// ============================================================

    void runPrompt() {
        std::cout << "Mochi v3.4 REPL (type 'exit' to quit)\n";
        auto interpreter = std::make_shared<Interpreter>();
        std::string line, buffer;
        int depth = 0; // 简单支持多行

        while (true) {
            std::cout << (depth > 0 ? ". " : "> ");
            if (!std::getline(std::cin, line)) break;
            if (line == "exit" || line == "quit") break;
            buffer += line + "\n";
            for (char c : line) {
                if (c == '{') depth++;
                else if (c == '}') depth--;
            }
            if (depth <= 0) {
                try {
                    Lexer lexer(buffer);
                    auto tokens = lexer.scanTokens();
                    Parser parser(std::move(tokens));
                    auto statements = parser.parse();
                    interpreter->interpret(statements);
                }
                catch (const std::exception& e) {
                    std::cerr << "Error: " << e.what() << std::endl;
                }
                buffer.clear();
                depth = 0;
            }
        }
    }
}

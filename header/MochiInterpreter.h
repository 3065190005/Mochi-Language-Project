#include "MochiInclude.h"
#include "MochiAst.h"
#include "MochiEnv.h"
#include "MochiRuntime.h"

#ifndef MOCHIINTERPRETER
#define MOCHIINTERPRETER

namespace Mochi
{
    // 向前声明
    class FFILibrary;

    // ============================================================
    //  解释器 (Interpreter) - 声明
    // ============================================================

    class MOCHI_API Interpreter : public std::enable_shared_from_this<Interpreter> {
        friend class Parser;
        void initBuiltins(); // 把原来构造函数里的内置注册提取到这个函数
    public:
        Interpreter();

        ~Interpreter();

        Interpreter(std::shared_ptr<Environment> g);

        void interpret(const std::vector<StmtPtr>& statements);

        std::shared_ptr<MochiFunction> bindFunction(std::shared_ptr<MochiFunction> func,
            std::shared_ptr<MochiInstance> instance);
        Value callFunction(std::shared_ptr<MochiFunction> func, const std::vector<Value>& arguments);

        void setGlobals(std::shared_ptr<Environment> g);

        std::shared_ptr<Environment> getGlobals() const;

        Value callDestroy(std::shared_ptr<MochiFunction> method, MochiInstance* instanceRaw);


        std::shared_ptr<FFILibrary> getFFI() const;

        // 标准库
        void registerBuiltinModules();

    private:
        Value evaluate(const Expr& expr);
        void execute(const Stmt& stmt);
        Value evaluateBinary(const Binary& expr);
        Value evaluateUnary(const Unary& expr);
        Value evaluateCall(const Call& expr);
        Value evaluateGet(const Get& expr);
        Value evaluateSet(const Set& expr);
        void executeBlock(const std::vector<StmtPtr>& statements, std::shared_ptr<Environment> env);
        bool isEqual(const Value& a, const Value& b);
        Value importModule(const std::string& path);

    private:
        // 其他私有方法...
        std::map<std::string, std::function<Value()>> builtinModules;
        std::shared_ptr<Environment> globals;
        std::shared_ptr<Environment> environment;
        std::map<std::string, Value> modules;
        std::shared_ptr<FFILibrary> ffiLib;
        std::vector<void*> loadedHandles;
        // ...
    };

    static Value jsonEncode(Value v);

    static Value jsonDecode(const std::string& json);

    // 在 Interpreter 类定义之后，所有成员函数实现之前添加
    static Value deepClone(Value v, Interpreter* interp);
}

#endif
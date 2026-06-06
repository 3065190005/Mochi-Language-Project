#include "MochiInclude.h"
#include "MochiAst.h"

#ifndef MOCHIRUNTIME
#define MOCHIRUNTIME

namespace Mochi
{
    // ============================================================
// 4. 运行时对象 
// ============================================================
    using Value = std::any;

    struct MochiFunction;
    struct MochiClass;
    struct MochiInstance;
    struct MochiArray;
    struct MochiDict;
    class Environment;

    struct MochiFunction {
        std::string name;
        std::vector<std::string> params;
        std::vector<StmtPtr> body;
        std::shared_ptr<Environment> closure; // 前向声明，后面定义
        MochiFunction(std::string n, std::vector<std::string> p, std::vector<StmtPtr> b,
            std::shared_ptr<Environment> c)
            : name(std::move(n)), params(std::move(p)), body(std::move(b)), closure(std::move(c)) {
        }
    };

    struct NativeFunction {
        std::string name;
        std::function<Value(class Interpreter*, const std::vector<Value>&)> func;
    };

    struct MochiClass {
        std::string name;
        std::shared_ptr<MochiClass> superClass;
        std::map<std::string, std::shared_ptr<MochiFunction>> methods;
        MochiClass(std::string n, std::shared_ptr<MochiClass> s,
            std::map<std::string, std::shared_ptr<MochiFunction>> m)
            : name(std::move(n)), superClass(std::move(s)), methods(std::move(m)) {
        }
        std::shared_ptr<MochiFunction> findMethod(const std::string& name) const {
            auto it = methods.find(name);
            if (it != methods.end()) return it->second;
            if (superClass) return superClass->findMethod(name);
            return nullptr;
        }
    };

    struct MochiInstance {
        std::shared_ptr<MochiClass> klass;
        std::map<std::string, Value> fields;
        std::shared_ptr<Environment> globals; // 新增

        MochiInstance(std::shared_ptr<MochiClass> k, std::shared_ptr<Environment> g)
            : klass(std::move(k)), globals(std::move(g)) {
        }
    };

    struct MochiArray {
        std::vector<Value> elements;
    };

    struct MochiDict {
        std::map<std::string, Value> entries;
    };
}

#endif
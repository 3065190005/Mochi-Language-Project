#include "MochiInclude.h"
#include "MochiUtils.h"
#include "MochiInterpreter.h"
#include "MochiAst.h"
#include "MochiEnv.h"

#ifndef MOCHIFFILIB
#define MOCHIFFILIB


namespace Mochi {

    // ============================================================
    // 11. FFI (FFILibrary) - 声明
    // ============================================================
    class FFILibrary {
        std::map<std::string, std::function<Value(Interpreter*, const std::vector<Value>&)>> nativeFuncs;
        std::map<std::string, std::shared_ptr<MochiFunction>> exportedFuncs;
        Interpreter* interpreterRaw; // 裸指针，生命周期由解释器保证
    public:
        explicit FFILibrary(Interpreter* interp) : interpreterRaw(interp) {}

        void add(const std::string& name,
            std::function<Value(Interpreter*, const std::vector<Value>&)> func);

        // 调用内置函数
        Value call(const std::string& name, const std::vector<Value>& args);

        void exportFunc(const std::string& name, std::shared_ptr<MochiFunction> func);

        // 调用导出函数
        Value callMochi(const std::string& name, const std::vector<Value>& args);

        // 调用 Mochi 实例的方法
        Value callInstanceMethod(const Value& instance, const std::string& methodName,
            const std::vector<Value>& args);

        // 根据类名创建实例（需提前在脚本中定义该类）
        Value createInstance(const std::string& className, const std::vector<Value>& args);

    public:
        class FFIHelper {
            FFILibrary* lib;
        public:
            explicit FFIHelper(FFILibrary* lib) : lib(lib) {}

            // 纯静态判断（无需上下文）
            static bool isNumber(const Value& v);
            static bool isString(const Value& v);
            static bool isBool(const Value& v);
            static bool isNil(const Value& v);
            static bool isArray(const Value& v);
            static bool isDict(const Value& v);
            static bool isInstance(const Value& v);

            // 类型转换
            static double toNumber(const Value& v);
            static std::string toString(const Value& v);
            static bool toBool(const Value& v);

            // 数组操作（无需解释器）
            static std::shared_ptr<MochiArray> toArray(const Value& v);
            static Value arrayGet(const Value& arr, int idx);
            static void arrayPush(const Value& arr, Value val);
            static Value arraySize(const Value& arr);
            static Value makeArray(const std::vector<Value>& elems);

            // 字典操作（无需解释器）
            static std::shared_ptr<MochiDict> toDict(const Value& v);
            static Value dictGet(const Value& dict, const std::string& key);
            static void dictSet(const Value& dict, const std::string& key, Value val);
            static Value dictSize(const Value& dict);
            static Value makeDict(const std::map<std::string, Value>& entries);

            // 实例字段操作（无需解释器）
            static std::shared_ptr<MochiInstance> toInstance(const Value& v);
            static Value instanceGetField(const Value& inst, const std::string& name);
            static void instanceSetField(const Value& inst, const std::string& name, Value val);

            // 需要解释器的方法（通过 lib 访问）
            Value callInstanceMethod(const Value& instance, const std::string& methodName,
                const std::vector<Value>& args);

            Value createInstance(const std::string& className, const std::vector<Value>& args);
        };

        // 提供 helper 的访问接口
        FFIHelper getHelper();
    };
}

#endif // !MOCHIFFILIB
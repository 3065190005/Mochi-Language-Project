#include "MochiFFILib.h"
#include "MochiRuntime.h"

namespace Mochi {

    // ============================================================
    // FFI (FFILibrary)
    // ============================================================

    void FFILibrary::add(const std::string& name,
        std::function<Value(Interpreter*, const std::vector<Value>&)> func) {
        nativeFuncs[name] = std::move(func);
    }

    // 调用内置函数
    Value FFILibrary::call(const std::string& name, const std::vector<Value>& args) {
        auto it = nativeFuncs.find(name);
        if (it == nativeFuncs.end())
            throw std::runtime_error("FFI function not found: " + name);
        return it->second(interpreterRaw, args);
    }

    void FFILibrary::exportFunc(const std::string& name, std::shared_ptr<MochiFunction> func) {
        exportedFuncs[name] = std::move(func);
    }

    // 调用导出函数

    Value FFILibrary::callMochi(const std::string& name, const std::vector<Value>& args) {
        auto it = exportedFuncs.find(name);
        if (it == exportedFuncs.end())
            throw std::runtime_error("No exported Mochi function: " + name);
        return interpreterRaw->callFunction(it->second, args);
    }

    // 调用 Mochi 实例的方法
    Value FFILibrary::callInstanceMethod(const Value& instance, const std::string& methodName,
        const std::vector<Value>& args) {
        auto obj = std::any_cast<std::shared_ptr<MochiInstance>>(instance);
        auto method = obj->klass->findMethod(methodName);
        if (!method) throw std::runtime_error("Method not found: " + methodName);
        // 绑定 this 并调用
        auto bound = interpreterRaw->bindFunction(method, obj);
        return interpreterRaw->callFunction(bound, args);
    }

    // 根据类名创建实例（需提前在脚本中定义该类）
    Value FFILibrary::createInstance(const std::string& className, const std::vector<Value>& args) {
        // 从全局环境获取类
        auto globalEnv = interpreterRaw->getGlobals();
        Value classVal = globalEnv->get(className); // 假设类在全局
        if (classVal.type() != typeid(std::shared_ptr<MochiClass>))
            throw std::runtime_error("Class not found: " + className);
        auto klass = std::any_cast<std::shared_ptr<MochiClass>>(classVal);
        // 使用 Interpreter 的 callFunction 机制（与脚本 new 类一致）
        // 直接调用 evaluateCall 逻辑，但此处简化：手动创建实例并调 init
        auto instance = std::make_shared<MochiInstance>(klass, globalEnv);
        auto init = klass->findMethod("init");
        if (init) {
            auto bound = interpreterRaw->bindFunction(init, instance);
            interpreterRaw->callFunction(bound, args);
        }
        return Value(instance);
    }

    // 提供 helper 的访问接口
    FFILibrary::FFIHelper FFILibrary::getHelper()
    {
        return FFILibrary::FFIHelper(this);
    }

    // 纯静态判断（无需上下文）

    bool FFILibrary::FFIHelper::isNumber(const Value& v)
    {
        return false;
    }

     bool FFILibrary::FFIHelper::isString(const Value& v) { return v.type() == typeid(std::string); }
     bool FFILibrary::FFIHelper::isBool(const Value& v) { return v.type() == typeid(bool); }
     bool FFILibrary::FFIHelper::isNil(const Value& v) { return !v.has_value(); }
     bool FFILibrary::FFIHelper::isArray(const Value& v) { return v.type() == typeid(std::shared_ptr<MochiArray>); }
     bool FFILibrary::FFIHelper::isDict(const Value& v) { return v.type() == typeid(std::shared_ptr<MochiDict>); }
     bool FFILibrary::FFIHelper::isInstance(const Value& v) { return v.type() == typeid(std::shared_ptr<MochiInstance>); }

    // 类型转换
     double FFILibrary::FFIHelper::toNumber(const Value& v) { return std::any_cast<double>(v); }
     std::string FFILibrary::FFIHelper::toString(const Value& v) { return std::any_cast<std::string>(v); }
     bool FFILibrary::FFIHelper::toBool(const Value& v) { return std::any_cast<bool>(v); }

    // 数组操作（无需解释器）
     std::shared_ptr<MochiArray> FFILibrary::FFIHelper::toArray(const Value& v) {
        return std::any_cast<std::shared_ptr<MochiArray>>(v);
    }
     Value FFILibrary::FFIHelper::arrayGet(const Value& arr, int idx) {
        auto a = toArray(arr);
        if (idx < 0 || idx >= (int)a->elements.size())
            throw std::runtime_error("Array index out of bounds.");
        return a->elements[idx];
    }
     void FFILibrary::FFIHelper::arrayPush(const Value& arr, Value val) {
        toArray(arr)->elements.push_back(std::move(val));
    }
     Value FFILibrary::FFIHelper::arraySize(const Value& arr) {
        return Value(static_cast<double>(toArray(arr)->elements.size()));
    }

     Value FFILibrary::FFIHelper::makeArray(const std::vector<Value>& elems) {
        auto a = std::make_shared<MochiArray>();
        a->elements = elems;
        return Value(a);
    }

    // 字典操作（无需解释器）
     std::shared_ptr<MochiDict> FFILibrary::FFIHelper::toDict(const Value& v) {
        return std::any_cast<std::shared_ptr<MochiDict>>(v);
    }
     Value FFILibrary::FFIHelper::dictGet(const Value& dict, const std::string& key) {
        auto d = toDict(dict);
        auto it = d->entries.find(key);
        if (it == d->entries.end()) return Value{};
        return it->second;
    }
     void FFILibrary::FFIHelper::dictSet(const Value& dict, const std::string& key, Value val) {
        toDict(dict)->entries[key] = std::move(val);
    }
     Value FFILibrary::FFIHelper::dictSize(const Value& dict) {
        return Value(static_cast<double>(toDict(dict)->entries.size()));
    }
     Value FFILibrary::FFIHelper::makeDict(const std::map<std::string, Value>& entries) {
        auto d = std::make_shared<MochiDict>();
        d->entries = entries;
        return Value(d);
    }

    // 实例字段操作（无需解释器）
     std::shared_ptr<MochiInstance> FFILibrary::FFIHelper::toInstance(const Value& v) {
        return std::any_cast<std::shared_ptr<MochiInstance>>(v);
    }
     Value FFILibrary::FFIHelper::instanceGetField(const Value& inst, const std::string& name) {
        auto obj = toInstance(inst);
        auto it = obj->fields.find(name);
        if (it != obj->fields.end()) return it->second;
        auto method = obj->klass->findMethod(name);
        if (method) return Value(method); // 未绑定
        return Value{};
    }
     void FFILibrary::FFIHelper::instanceSetField(const Value& inst, const std::string& name, Value val) {
        toInstance(inst)->fields[name] = std::move(val);
    }

    // 需要解释器的方法（通过 lib 访问）
    Value FFILibrary::FFIHelper::callInstanceMethod(const Value& instance, const std::string& methodName,
        const std::vector<Value>& args) {
        auto obj = std::any_cast<std::shared_ptr<MochiInstance>>(instance);
        auto method = obj->klass->findMethod(methodName);
        if (!method) throw std::runtime_error("Method not found: " + methodName);
        auto bound = lib->interpreterRaw->bindFunction(method, obj);
        return lib->interpreterRaw->callFunction(bound, args);
    }

    Value FFILibrary::FFIHelper::createInstance(const std::string& className, const std::vector<Value>& args) {
        auto globalEnv = lib->interpreterRaw->getGlobals();
        Value classVal = globalEnv->get(className);
        if (!isInstance(classVal)) { // 实际上类不是实例，我们需要检查 MochiClass
            // 这里可以增加一个 isClass 辅助
        }
        // ... 简化实现，可复用 lib 的 createInstance 方法
        return lib->createInstance(className, args); // 调用 FFILibrary 已经实现好的
    }


}
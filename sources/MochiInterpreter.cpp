#include "MochiInterpreter.h"
#include "MochiEnv.h"
#include "MochiRuntime.h"
#include "MochiUtils.h"
#include "MochiGui.h"
#include "MochiErr.h"
#include "MochiActor.h"
#include "MochiFFILib.h"
#include "MochiParser.h"

// 标准库头文件
#include "dialog_wrapper.h"
#include "socket_wrapper.h"

#pragma once 

namespace Mochi
{

    Interpreter::Interpreter()
        {
            globals = std::make_shared<Environment>();
            environment = globals;
            initBuiltins();
            registerBuiltinModules();
        }

    Interpreter:: ~Interpreter() {
            // 进程退出时由操作系统自动回收动态库资源，不手动卸载
            // 手动卸载可能因 Actor 线程仍在运行或静态析构顺序导致崩溃
            loadedHandles.clear();
        }

    Interpreter::Interpreter(std::shared_ptr<Environment> g) : globals(std::move(g)), environment(globals) {}

        void Interpreter::setGlobals(std::shared_ptr<Environment> g) {
            globals = g;
            environment = g;
        }

        std::shared_ptr<Environment> Interpreter::getGlobals() const { return globals; }

        Value Interpreter::callDestroy(std::shared_ptr<MochiFunction> method, MochiInstance* instanceRaw) {
            // 创建一个不拥有实例的临时 shared_ptr，用于绑定 this
            auto nonOwning = std::shared_ptr<MochiInstance>(instanceRaw, [](MochiInstance*) {});
            auto bound = bindFunction(method, nonOwning);
            return callFunction(bound, {});
        }


        std::shared_ptr<FFILibrary> Interpreter::getFFI() const { return ffiLib; }


    static Value jsonEncode(Value v) {
        std::ostringstream oss;
        std::function<void(const Value&)> encode = [&](const Value& val) {
            if (!val.has_value()) {
                oss << "null";
            }
            else if (val.type() == typeid(bool)) {
                oss << (std::any_cast<bool>(val) ? "true" : "false");
            }
            else if (val.type() == typeid(double)) {
                double d = std::any_cast<double>(val);
                if (std::isnan(d) || std::isinf(d))
                    throw std::runtime_error("JSON encode: cannot encode NaN or Infinity.");
                oss << d;
            }
            else if (val.type() == typeid(std::string)) {
                std::string s = std::any_cast<std::string>(val);
                oss << '"';
                for (char c : s) {
                    switch (c) {
                    case '"': oss << "\\\""; break;
                    case '\\': oss << "\\\\"; break;
                    case '\b': oss << "\\b"; break;
                    case '\f': oss << "\\f"; break;
                    case '\n': oss << "\\n"; break;
                    case '\r': oss << "\\r"; break;
                    case '\t': oss << "\\t"; break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                << static_cast<int>(c);
                        }
                        else {
                            oss << c;
                        }
                    }
                }
                oss << '"';
            }
            else if (val.type() == typeid(std::shared_ptr<MochiArray>)) {
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(val);
                oss << '[';
                for (size_t i = 0; i < arr->elements.size(); ++i) {
                    if (i > 0) oss << ',';
                    encode(arr->elements[i]);
                }
                oss << ']';
            }
            else if (val.type() == typeid(std::shared_ptr<MochiDict>)) {
                auto dict = std::any_cast<std::shared_ptr<MochiDict>>(val);
                oss << '{';
                bool first = true;
                for (auto& [k, v] : dict->entries) {
                    if (!first) oss << ',';
                    // 键必须是字符串（JSON 要求）
                    oss << '"';
                    for (char c : k) {
                        if (c == '"') oss << "\\\"";
                        else if (c == '\\') oss << "\\\\";
                        else oss << c;
                    }
                    oss << '"' << ':';
                    encode(v);
                    first = false;
                }
                oss << '}';
            }
            else {
                throw std::runtime_error("JSON encode: unsupported type.");
            }
            };
        encode(v);
        return Value(oss.str());
    }

    static Value jsonDecode(const std::string& json) {
        size_t pos = 0;
        auto skipWhitespace = [&]() {
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                json[pos] == '\n' || json[pos] == '\r')) pos++;
            };

        std::function<Value()> parseValue;  // 前向声明

        auto parseString = [&]() -> std::string {
            if (pos >= json.size() || json[pos] != '"')
                throw std::runtime_error("JSON decode: expected '\"'");
            pos++;
            std::string result;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\') {
                    pos++;
                    if (pos >= json.size()) throw std::runtime_error("JSON decode: unexpected end of string");
                    switch (json[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    default: result += json[pos]; break;
                    }
                }
                else {
                    result += json[pos];
                }
                pos++;
            }
            if (pos >= json.size()) throw std::runtime_error("JSON decode: unterminated string");
            pos++;
            return result;
            };

        auto parseNumber = [&]() -> double {
            size_t start = pos;
            if (json[pos] == '-') pos++;
            while (pos < json.size() && std::isdigit(json[pos])) pos++;
            if (pos < json.size() && json[pos] == '.') {
                pos++;
                while (pos < json.size() && std::isdigit(json[pos])) pos++;
            }
            if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
                pos++;
                if (json[pos] == '+' || json[pos] == '-') pos++;
                while (pos < json.size() && std::isdigit(json[pos])) pos++;
            }
            return std::stod(json.substr(start, pos - start));
            };

        auto parseArray = [&]() -> Value {
            if (json[pos] != '[') throw std::runtime_error("JSON decode: expected '['");
            pos++;
            skipWhitespace();
            auto arr = std::make_shared<MochiArray>();
            if (json[pos] == ']') {
                pos++;
                return Value(arr);
            }
            while (true) {
                skipWhitespace();
                arr->elements.push_back(parseValue());
                skipWhitespace();
                if (json[pos] == ',') {
                    pos++;
                }
                else if (json[pos] == ']') {
                    pos++;
                    break;
                }
                else {
                    throw std::runtime_error("JSON decode: expected ',' or ']'");
                }
            }
            return Value(arr);
            };

        auto parseObject = [&]() -> Value {
            if (json[pos] != '{') throw std::runtime_error("JSON decode: expected '{'");
            pos++;
            skipWhitespace();
            auto dict = std::make_shared<MochiDict>();
            if (json[pos] == '}') {
                pos++;
                return Value(dict);
            }
            while (true) {
                skipWhitespace();
                std::string key = parseString();
                skipWhitespace();
                if (json[pos] != ':') throw std::runtime_error("JSON decode: expected ':'");
                pos++;
                skipWhitespace();
                Value val = parseValue();
                dict->entries[key] = val;
                skipWhitespace();
                if (json[pos] == ',') {
                    pos++;
                }
                else if (json[pos] == '}') {
                    pos++;
                    break;
                }
                else {
                    throw std::runtime_error("JSON decode: expected ',' or '}'");
                }
            }
            return Value(dict);
            };

        parseValue = [&]() -> Value {
            skipWhitespace();
            if (pos >= json.size()) throw std::runtime_error("JSON decode: unexpected end");
            char c = json[pos];
            if (c == '"') return Value(parseString());
            if (c == '-' || std::isdigit(c)) return Value(parseNumber());
            if (json.compare(pos, 4, "true") == 0) { pos += 4; return Value(true); }
            if (json.compare(pos, 5, "false") == 0) { pos += 5; return Value(false); }
            if (json.compare(pos, 4, "null") == 0) { pos += 4; return Value{}; }
            if (c == '[') return parseArray();
            if (c == '{') return parseObject();
            throw std::runtime_error("JSON decode: unexpected character");
            };

        Value result = parseValue();
        skipWhitespace();
        if (pos != json.size()) throw std::runtime_error("JSON decode: unexpected trailing characters");
        return result;
    }

    // 在 Interpreter 类定义之后，所有成员函数实现之前添加
    static Value deepClone(Value v, Interpreter* interp) {
        if (!v.has_value()) return Value{}; // nil
        // 不可变类型直接返回
        if (v.type() == typeid(bool) || v.type() == typeid(double) ||
            v.type() == typeid(std::string) || v.type() == typeid(std::shared_ptr<MochiFunction>) ||
            v.type() == typeid(std::shared_ptr<NativeFunction>) ||
            v.type() == typeid(std::shared_ptr<MochiClass>)) {
            return v;
        }
        // 数组深拷贝
        if (v.type() == typeid(std::shared_ptr<MochiArray>)) {
            auto arr = std::any_cast<std::shared_ptr<MochiArray>>(v);
            auto newArr = std::make_shared<MochiArray>();
            for (auto& elem : arr->elements) {
                newArr->elements.push_back(deepClone(elem, interp));
            }
            return Value(newArr);
        }
        // 字典深拷贝
        if (v.type() == typeid(std::shared_ptr<MochiDict>)) {
            auto dict = std::any_cast<std::shared_ptr<MochiDict>>(v);
            auto newDict = std::make_shared<MochiDict>();
            for (auto& [key, val] : dict->entries) {
                newDict->entries[key] = deepClone(val, interp);
            }
            return Value(newDict);
        }
        // 实例通过 __clone__ 方法拷贝
        if (v.type() == typeid(std::shared_ptr<MochiInstance>)) {
            auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(v);
            // 优先使用用户自定义的 __clone__ 方法
            auto cloneMethod = instance->klass->findMethod("__clone__");
            if (cloneMethod) {
                auto bound = interp->bindFunction(cloneMethod, instance);
                return interp->callFunction(bound, {});
            }
            // 默认深拷贝：创建同类新实例，递归拷贝所有字段
            auto newInstance = std::make_shared<MochiInstance>(instance->klass, instance->globals);
            for (auto& [key, val] : instance->fields) {
                newInstance->fields[key] = deepClone(val, interp);
            }
            return Value(newInstance);
        }
        throw std::runtime_error("Cannot clone this type.");
    }

    void Interpreter::registerBuiltinModules() 
    {
        // "fs" 标准库
        builtinModules["fs"] = [this]() -> Value {
            auto fs = std::make_shared<MochiDict>();

            // fs.read(path)
            auto readFunc = std::make_shared<NativeFunction>();
            readFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("fs.read(path)");
                std::string path = std::any_cast<std::string>(args[0]);
                std::ifstream file(path);
                if (!file) throw std::runtime_error("Cannot open file: " + path);
                std::stringstream buf;
                buf << file.rdbuf();
                return Value(buf.str());
                };
            fs->entries["read"] = Value(readFunc);

            // fs.write(path, content)
            auto writeFunc = std::make_shared<NativeFunction>();
            writeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("fs.write(path, content)");
                std::string path = std::any_cast<std::string>(args[0]);
                std::string content = stringify(args[1]);
                std::ofstream file(path);
                if (!file) throw std::runtime_error("Cannot write file: " + path);
                file << content;
                return Value{};
                };
            fs->entries["write"] = Value(writeFunc);

            // fs.append(path, content)
            auto appendFunc = std::make_shared<NativeFunction>();
            appendFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("fs.append(path, content)");
                std::string path = std::any_cast<std::string>(args[0]);
                std::string content = stringify(args[1]);
                std::ofstream file(path, std::ios::app);
                if (!file) throw std::runtime_error("Cannot append to file: " + path);
                file << content;
                return Value{};
                };
            fs->entries["append"] = Value(appendFunc);

            // fs.exists(path)
            auto existsFunc = std::make_shared<NativeFunction>();
            existsFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("fs.exists(path)");
                std::string path = std::any_cast<std::string>(args[0]);
                std::ifstream f(path);
                return Value((bool)f.good());
                };
            fs->entries["exists"] = Value(existsFunc);

            // fs.remove(path)
            auto removeFunc = std::make_shared<NativeFunction>();
            removeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("fs.remove(path)");
                std::string path = std::any_cast<std::string>(args[0]);
                if (std::remove(path.c_str()) != 0)
                    throw std::runtime_error("Cannot remove file: " + path);
                return Value{};
                };
            fs->entries["remove"] = Value(removeFunc);

            // fs.listdir(path)  (需要 C++17 <filesystem>)
            auto listdirFunc = std::make_shared<NativeFunction>();
            listdirFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                std::string path = ".";
                if (!args.empty()) path = std::any_cast<std::string>(args[0]);
                auto arr = std::make_shared<MochiArray>();
#if __has_include(<filesystem>)
                namespace fs = std::filesystem;
                for (const auto& entry : fs::directory_iterator(path)) {
                    arr->elements.push_back(Value(entry.path().filename().string()));
                }
#else
                throw std::runtime_error("listdir requires <filesystem> support.");
#endif
                return Value(arr);
                };
            fs->entries["listdir"] = Value(listdirFunc);

            return Value(fs);
            };

        // math 标准库
        builtinModules["math"] = [this]() -> Value {
            auto math = std::make_shared<MochiDict>();

            // 常量
            math->entries["pi"] = Value(3.14159265358979323846);
            math->entries["e"] = Value(2.71828182845904523536);

            // 基本函数
            auto addMathFunc = [&](const std::string& name,
                std::function<double(double)> func) {
                    auto f = std::make_shared<NativeFunction>();
                    f->func = [func, name](Interpreter*, const std::vector<Value>& args) -> Value {
                        if (args.empty()) throw std::runtime_error("math." + name + " expects 1 argument");
                        double x = std::any_cast<double>(args[0]);
                        return Value(func(x));
                        };
                    math->entries[name] = Value(f);
                };

            auto addMathFunc2 = [&](const std::string& name,
                std::function<double(double, double)> func) {
                    auto f = std::make_shared<NativeFunction>();
                    f->func = [func, name](Interpreter*, const std::vector<Value>& args) -> Value {
                        if (args.size() < 2) throw std::runtime_error("math." + name + " expects 2 arguments");
                        double a = std::any_cast<double>(args[0]);
                        double b = std::any_cast<double>(args[1]);
                        return Value(func(a, b));
                        };
                    math->entries[name] = Value(f);
                };

            // 一元函数
            addMathFunc("sin", [](double x) { return std::sin(x); });
            addMathFunc("cos", [](double x) { return std::cos(x); });
            addMathFunc("tan", [](double x) { return std::tan(x); });
            addMathFunc("asin", [](double x) { return std::asin(x); });
            addMathFunc("acos", [](double x) { return std::acos(x); });
            addMathFunc("atan", [](double x) { return std::atan(x); });
            addMathFunc("sinh", [](double x) { return std::sinh(x); });
            addMathFunc("cosh", [](double x) { return std::cosh(x); });
            addMathFunc("tanh", [](double x) { return std::tanh(x); });
            addMathFunc("exp", [](double x) { return std::exp(x); });
            addMathFunc("log", [](double x) { return std::log(x); }); // 自然对数
            addMathFunc("log10", [](double x) { return std::log10(x); });
            addMathFunc("sqrt", [](double x) { return std::sqrt(x); });
            addMathFunc("ceil", [](double x) { return std::ceil(x); });
            addMathFunc("floor", [](double x) { return std::floor(x); });
            addMathFunc("abs", [](double x) { return std::abs(x); }); // 虽然全局有，但 math.abs 同样提供

            // 二元函数
            addMathFunc2("pow", [](double a, double b) { return std::pow(a, b); });
            addMathFunc2("atan2", [](double a, double b) { return std::atan2(a, b); });

            // 常量生成函数（无参数）
            auto addMathFunc0 = [&](const std::string& name,
                std::function<double()> func) {
                    auto f = std::make_shared<NativeFunction>();
                    f->func = [func](Interpreter*, const std::vector<Value>&) -> Value {
                        return Value(func());
                        };
                    math->entries[name] = Value(f);
                };

            // 例如 math.random() 返回 [0,1) 随机数（为了不与全局 random 冲突，放在 math 模块）
            addMathFunc0("random", []() {
                static thread_local std::mt19937 gen(std::random_device{}());
                static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
                return dist(gen);
                });

            return Value(math);
            };

        // json 标准库
        builtinModules["json"] = [this]() -> Value {
            auto jsonMod = std::make_shared<MochiDict>();

            // json.encode(value)
            auto encodeFunc = std::make_shared<NativeFunction>();
            encodeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("json.encode expects 1 argument.");
                return jsonEncode(args[0]);
                };
            jsonMod->entries["encode"] = Value(encodeFunc);

            // json.decode(str)
            auto decodeFunc = std::make_shared<NativeFunction>();
            decodeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("json.decode expects 1 argument.");
                std::string str = std::any_cast<std::string>(args[0]);
                return jsonDecode(str);
                };
            jsonMod->entries["decode"] = Value(decodeFunc);

            return Value(jsonMod);
            };

        // random 标准库
        builtinModules["random"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            // 线程安全的随机数生成器（使用 thread_local 引擎）
            auto getEngine = []() -> std::mt19937& {
                static thread_local std::mt19937 engine(std::random_device{}());
                return engine;
                };

            // 内部辅助：生成 [0, 1) 浮点数
            auto random01 = [&]() -> double {
                static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
                return dist(getEngine());
                };

            // random.seed(n)  设置种子
            auto seedFunc = std::make_shared<NativeFunction>();
            seedFunc->func = [getEngine](Interpreter*, const std::vector<Value>& args) -> Value {
                if (!args.empty() && args[0].type() == typeid(double)) {
                    uint32_t s = static_cast<uint32_t>(std::any_cast<double>(args[0]));
                    getEngine().seed(s);
                }
                else {
                    // 无参数使用硬件随机数重新播种
                    getEngine().seed(std::random_device{}());
                }
                return Value{};
                };
            mod->entries["seed"] = Value(seedFunc);

            // random.random()  返回 [0,1) 浮点数
            auto randFunc = std::make_shared<NativeFunction>();
            randFunc->func = [random01](Interpreter*, const std::vector<Value>&) -> Value {
                return Value(random01());
                };
            mod->entries["random"] = Value(randFunc);

            // random.uniform(a, b)  返回 [a,b) 浮点数
            auto uniformFunc = std::make_shared<NativeFunction>();
            uniformFunc->func = [getEngine](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("uniform requires 2 arguments.");
                double a = std::any_cast<double>(args[0]);
                double b = std::any_cast<double>(args[1]);
                std::uniform_real_distribution<double> dist(a, b);
                return Value(dist(getEngine()));
                };
            mod->entries["uniform"] = Value(uniformFunc);

            // random.randint(min, max)  返回 [min, max] 整数（包含两端）
            auto randintFunc = std::make_shared<NativeFunction>();
            randintFunc->func = [getEngine](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("randint requires 2 arguments.");
                int min = static_cast<int>(std::any_cast<double>(args[0]));
                int max = static_cast<int>(std::any_cast<double>(args[1]));
                std::uniform_int_distribution<int> dist(min, max);
                return Value(static_cast<double>(dist(getEngine())));
                };
            mod->entries["randint"] = Value(randintFunc);

            // random.choice(array)  随机选取一个元素
            auto choiceFunc = std::make_shared<NativeFunction>();
            choiceFunc->func = [random01](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("choice requires an array.");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                if (arr->elements.empty()) throw std::runtime_error("choice() empty array.");
                size_t idx = static_cast<size_t>(random01() * arr->elements.size());
                if (idx >= arr->elements.size()) idx = arr->elements.size() - 1; // 边界保护
                return arr->elements[idx];
                };
            mod->entries["choice"] = Value(choiceFunc);

            // random.shuffle(array)  原地打乱数组
            auto shuffleFunc = std::make_shared<NativeFunction>();
            shuffleFunc->func = [getEngine](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("shuffle requires an array.");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                auto& elems = arr->elements;
                std::shuffle(elems.begin(), elems.end(), getEngine());
                return Value{}; // nil
                };
            mod->entries["shuffle"] = Value(shuffleFunc);

            // random.sample(array, k)  无重复抽取 k 个元素，返回新数组
            auto sampleFunc = std::make_shared<NativeFunction>();
            sampleFunc->func = [getEngine](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("sample requires an array and k.");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                int k = static_cast<int>(std::any_cast<double>(args[1]));
                if (k < 0 || k > static_cast<int>(arr->elements.size()))
                    throw std::runtime_error("sample: k out of range.");
                // 复制一份索引并随机抽取
                std::vector<size_t> indices(arr->elements.size());
                std::iota(indices.begin(), indices.end(), 0);
                std::shuffle(indices.begin(), indices.end(), getEngine());
                auto result = std::make_shared<MochiArray>();
                for (int i = 0; i < k; ++i) {
                    result->elements.push_back(arr->elements[indices[i]]);
                }
                return Value(result);
                };
            mod->entries["sample"] = Value(sampleFunc);

            // random.bytes(n)  返回 n 个随机字节（0-255）组成的字符串（可用于加密、测试）
            auto bytesFunc = std::make_shared<NativeFunction>();
            bytesFunc->func = [getEngine](Interpreter*, const std::vector<Value>& args) -> Value {
                int n = 1;
                if (!args.empty() && args[0].type() == typeid(double)) {
                    n = static_cast<int>(std::any_cast<double>(args[0]));
                }
                std::uniform_int_distribution<int> byteDist(0, 255);
                std::string bytes;
                bytes.reserve(n);
                for (int i = 0; i < n; ++i) {
                    bytes.push_back(static_cast<char>(byteDist(getEngine())));
                }
                return Value(bytes);
                };
            mod->entries["bytes"] = Value(bytesFunc);

            return Value(mod);
            };

        // string 标准库
        builtinModules["string"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            // string.split(str, delimiter) 返回数组
            auto splitFunc = std::make_shared<NativeFunction>();
            splitFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("string.split(str, delim)");
                std::string s = std::any_cast<std::string>(args[0]);
                std::string delim = std::any_cast<std::string>(args[1]);
                auto arr = std::make_shared<MochiArray>();
                if (delim.empty()) {
                    for (char c : s) arr->elements.push_back(Value(std::string(1, c)));
                    return Value(arr);
                }
                size_t pos = 0;
                while (pos <= s.size()) {
                    size_t next = s.find(delim, pos);
                    if (next == std::string::npos) {
                        arr->elements.push_back(Value(s.substr(pos)));
                        break;
                    }
                    arr->elements.push_back(Value(s.substr(pos, next - pos)));
                    pos = next + delim.size();
                }
                return Value(arr);
                };
            mod->entries["split"] = Value(splitFunc);

            // string.join(arr, delimiter) 返回字符串
            auto joinFunc = std::make_shared<NativeFunction>();
            joinFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("string.join(arr, delim)");
                if (args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("string.join first arg must be array");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                std::string delim = std::any_cast<std::string>(args[1]);
                std::ostringstream oss;
                for (size_t i = 0; i < arr->elements.size(); ++i) {
                    if (i > 0) oss << delim;
                    oss << stringify(arr->elements[i]);
                }
                return Value(oss.str());
                };
            mod->entries["join"] = Value(joinFunc);

            // string.startsWith(str, prefix) 返回 bool
            auto startsWithFunc = std::make_shared<NativeFunction>();
            startsWithFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("string.startsWith(str, prefix)");
                std::string s = std::any_cast<std::string>(args[0]);
                std::string pre = std::any_cast<std::string>(args[1]);
                return Value(s.rfind(pre, 0) == 0);
                };
            mod->entries["startsWith"] = Value(startsWithFunc);

            // string.endsWith(str, suffix) 返回 bool
            auto endsWithFunc = std::make_shared<NativeFunction>();
            endsWithFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("string.endsWith(str, suffix)");
                std::string s = std::any_cast<std::string>(args[0]);
                std::string suf = std::any_cast<std::string>(args[1]);
                if (suf.size() > s.size()) return Value(false);
                return Value(s.compare(s.size() - suf.size(), suf.size(), suf) == 0);
                };
            mod->entries["endsWith"] = Value(endsWithFunc);

            // string.replace(str, old, new) 返回新字符串
            auto replaceFunc = std::make_shared<NativeFunction>();
            replaceFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 3) throw std::runtime_error("string.replace(str, old, new)");
                std::string s = std::any_cast<std::string>(args[0]);
                std::string oldStr = std::any_cast<std::string>(args[1]);
                std::string newStr = std::any_cast<std::string>(args[2]);
                size_t pos = 0;
                while ((pos = s.find(oldStr, pos)) != std::string::npos) {
                    s.replace(pos, oldStr.size(), newStr);
                    pos += newStr.size();
                }
                return Value(s);
                };
            mod->entries["replace"] = Value(replaceFunc);

            // string.trim(str) 去除首尾空白
            auto trimFunc = std::make_shared<NativeFunction>();
            trimFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("string.trim(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                auto isspace = [](unsigned char c) { return std::isspace(c); };
                s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), isspace));
                s.erase(std::find_if_not(s.rbegin(), s.rend(), isspace).base(), s.end());
                return Value(s);
                };
            mod->entries["trim"] = Value(trimFunc);

            // string.lower(str) / upper(str)
            auto lowerFunc = std::make_shared<NativeFunction>();
            lowerFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("string.lower(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                return Value(s);
                };
            mod->entries["lower"] = Value(lowerFunc);

            auto upperFunc = std::make_shared<NativeFunction>();
            upperFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("string.upper(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                std::transform(s.begin(), s.end(), s.begin(), ::toupper);
                return Value(s);
                };
            mod->entries["upper"] = Value(upperFunc);

            // string.repeat(str, count) 重复字符串
            auto repeatFunc = std::make_shared<NativeFunction>();
            repeatFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("string.repeat(str, count)");
                std::string s = std::any_cast<std::string>(args[0]);
                int n = static_cast<int>(std::any_cast<double>(args[1]));
                if (n < 0) throw std::runtime_error("repeat count must be >= 0");
                std::string result;
                result.reserve(s.size() * n);
                for (int i = 0; i < n; ++i) result += s;
                return Value(result);
                };
            mod->entries["repeat"] = Value(repeatFunc);

            return Value(mod);
            };

        // array 标准库
        builtinModules["array"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            // array.map(arr, fn) 返回新数组
            auto mapFunc = std::make_shared<NativeFunction>();
            mapFunc->func = [](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.map(arr, fn)");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                Value fnArg = args[1];
                std::shared_ptr<MochiFunction> func;
                if (fnArg.type() == typeid(std::shared_ptr<MochiFunction>))
                    func = std::any_cast<std::shared_ptr<MochiFunction>>(fnArg);
                else if (fnArg.type() == typeid(std::shared_ptr<NativeFunction>)) {
                    // 包装为 MochiFunction 不太可行，要求用户传 Mochi 函数
                    throw std::runtime_error("array.map requires a Mochi function");
                }
                else throw std::runtime_error("array.map second arg must be function");
                auto result = std::make_shared<MochiArray>();
                for (auto& elem : arr->elements) {
                    Value ret = interp->callFunction(func, { elem });
                    result->elements.push_back(ret);
                }
                return Value(result);
                };
            mod->entries["map"] = Value(mapFunc);

            // array.filter(arr, fn) 返回新数组
            auto filterFunc = std::make_shared<NativeFunction>();
            filterFunc->func = [this](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.filter(arr, fn)");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                auto func = std::any_cast<std::shared_ptr<MochiFunction>>(args[1]);
                auto newArr = std::make_shared<MochiArray>();
                for (auto& elem : arr->elements) {
                    Value res = interp->callFunction(func, { elem });
                    if (isTruthy(res)) newArr->elements.push_back(elem);
                }
                return Value(newArr);
                };
            mod->entries["filter"] = Value(filterFunc);

            // filterInPlace(arr, fn) - 原地过滤
            auto filterInPlaceFunc = std::make_shared<NativeFunction>();
            filterInPlaceFunc->func = [this](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.filterInPlace(arr, fn)");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                auto func = std::any_cast<std::shared_ptr<MochiFunction>>(args[1]);
                auto& elems = arr->elements;
                elems.erase(std::remove_if(elems.begin(), elems.end(),
                    [&](const Value& v) {
                        Value res = interp->callFunction(func, { v });
                        return !isTruthy(res);
                    }), elems.end());
                return Value{};
                };
            mod->entries["filterInPlace"] = Value(filterInPlaceFunc);

            // reduce(arr, fn, initial?)
            auto reduceFunc = std::make_shared<NativeFunction>();
            reduceFunc->func = [this](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.reduce(arr, fn [, initial])");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                auto func = std::any_cast<std::shared_ptr<MochiFunction>>(args[1]);
                if (arr->elements.empty()) {
                    if (args.size() >= 3) return args[2];
                    throw std::runtime_error("reduce of empty array with no initial value");
                }
                size_t i = 0;
                Value acc;
                if (args.size() >= 3) {
                    acc = args[2];
                }
                else {
                    acc = arr->elements[0];
                    i = 1;
                }
                for (; i < arr->elements.size(); ++i) {
                    acc = interp->callFunction(func, { acc, arr->elements[i] });
                }
                return acc;
                };
            mod->entries["reduce"] = Value(reduceFunc);

            // array.find(arr, fn) 返回第一个满足条件的元素，否则 nil
            auto findFunc = std::make_shared<NativeFunction>();
            findFunc->func = [](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.find(arr, fn)");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                Value fnArg = args[1];
                if (fnArg.type() != typeid(std::shared_ptr<MochiFunction>))
                    throw std::runtime_error("array.find second arg must be function");
                auto func = std::any_cast<std::shared_ptr<MochiFunction>>(fnArg);
                for (auto& elem : arr->elements) {
                    if (isTruthy(interp->callFunction(func, { elem }))) return elem;
                }
                return Value{};
                };
            mod->entries["find"] = Value(findFunc);

            // array.reverse(arr) 原地反转，返回 nil
            auto reverseFunc = std::make_shared<NativeFunction>();
            reverseFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.reverse(arr)");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                std::reverse(arr->elements.begin(), arr->elements.end());
                return Value{};
                };
            mod->entries["reverse"] = Value(reverseFunc);

            // array.copy(arr) 浅拷贝（与 arr.clone() 一致，但提供模块形式）
            auto copyFunc = std::make_shared<NativeFunction>();
            copyFunc->func = [](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.copy(arr)");
                return deepClone(args[0], interp); // 使用深拷贝，若想浅拷贝可用手动遍历
                };
            mod->entries["copy"] = Value(copyFunc);

            // array.flatten(arr) 扁平化一层
            auto flattenFunc = std::make_shared<NativeFunction>();
            flattenFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.flatten(arr)");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                auto result = std::make_shared<MochiArray>();
                for (auto& elem : arr->elements) {
                    if (elem.type() == typeid(std::shared_ptr<MochiArray>)) {
                        auto sub = std::any_cast<std::shared_ptr<MochiArray>>(elem);
                        result->elements.insert(result->elements.end(), sub->elements.begin(), sub->elements.end());
                    }
                    else {
                        result->elements.push_back(elem);
                    }
                }
                return Value(result);
                };
            mod->entries["flatten"] = Value(flattenFunc);

            // some(arr, fn) - 是否有任意满足
            auto someFunc = std::make_shared<NativeFunction>();
            someFunc->func = [this](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.some(arr, fn)");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                auto func = std::any_cast<std::shared_ptr<MochiFunction>>(args[1]);
                for (auto& elem : arr->elements) {
                    Value res = interp->callFunction(func, { elem });
                    if (isTruthy(res)) return Value(true);
                }
                return Value(false);
                };
            mod->entries["some"] = Value(someFunc);

            // every(arr, fn) - 是否全部满足
            auto everyFunc = std::make_shared<NativeFunction>();
            everyFunc->func = [this](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.every(arr, fn)");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                auto func = std::any_cast<std::shared_ptr<MochiFunction>>(args[1]);
                for (auto& elem : arr->elements) {
                    Value res = interp->callFunction(func, { elem });
                    if (!isTruthy(res)) return Value(false);
                }
                return Value(true);
                };
            mod->entries["every"] = Value(everyFunc);

            // sort(arr [, compareFn]) - 原地排序
            auto sortFunc = std::make_shared<NativeFunction>();
            sortFunc->func = [this](Interpreter* interp, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiArray>))
                    throw std::runtime_error("array.sort(arr [, fn])");
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                if (args.size() >= 2 && args[1].type() == typeid(std::shared_ptr<MochiFunction>)) {
                    auto cmp = std::any_cast<std::shared_ptr<MochiFunction>>(args[1]);
                    std::sort(arr->elements.begin(), arr->elements.end(),
                        [&](const Value& a, const Value& b) {
                            Value res = interp->callFunction(cmp, { a, b });
                            if (res.type() == typeid(double)) return std::any_cast<double>(res) < 0;
                            return false;
                        });
                }
                else {
                    std::sort(arr->elements.begin(), arr->elements.end(),
                        [](const Value& a, const Value& b) {
                            return stringify(a) < stringify(b);
                        });
                }
                return Value{};
                };
            mod->entries["sort"] = Value(sortFunc);

            return Value(mod);
            };

        // dict 标准库
        builtinModules["dict"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            // dict.keys(d) 返回所有键的数组
            auto keysFunc = std::make_shared<NativeFunction>();
            keysFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.keys(dict)");
                auto d = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                auto arr = std::make_shared<MochiArray>();
                for (auto& [k, _] : d->entries) arr->elements.push_back(Value(k));
                return Value(arr);
                };
            mod->entries["keys"] = Value(keysFunc);

            // dict.values(d) 返回所有值的数组
            auto valuesFunc = std::make_shared<NativeFunction>();
            valuesFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.values(dict)");
                auto d = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                auto arr = std::make_shared<MochiArray>();
                for (auto& [_, v] : d->entries) arr->elements.push_back(v);
                return Value(arr);
                };
            mod->entries["values"] = Value(valuesFunc);

            // entries(dict) -> [[key, value], ...]
            auto entriesFunc = std::make_shared<NativeFunction>();
            entriesFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.entries(dict)");
                auto dict = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                auto pairs = std::make_shared<MochiArray>();
                for (auto& [k, v] : dict->entries) {
                    auto pair = std::make_shared<MochiArray>();
                    pair->elements.push_back(Value(k));
                    pair->elements.push_back(v);
                    pairs->elements.push_back(Value(pair));
                }
                return Value(pairs);
                };
            mod->entries["entries"] = Value(entriesFunc);

            // merge(dict1, dict2, ...) - 合并多个字典，返回新字典
            auto mergeFunc = std::make_shared<NativeFunction>();
            mergeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                auto newDict = std::make_shared<MochiDict>();
                for (auto& arg : args) {
                    if (arg.type() != typeid(std::shared_ptr<MochiDict>))
                        throw std::runtime_error("dict.merge expects dict arguments");
                    auto d = std::any_cast<std::shared_ptr<MochiDict>>(arg);
                    for (auto& [k, v] : d->entries) newDict->entries[k] = v;
                }
                return Value(newDict);
                };
            mod->entries["merge"] = Value(mergeFunc);

            // dict.items(d) 返回 [key, value] 对的数组
            auto itemsFunc = std::make_shared<NativeFunction>();
            itemsFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.items(dict)");
                auto d = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                auto arr = std::make_shared<MochiArray>();
                for (auto& [k, v] : d->entries) {
                    auto pair = std::make_shared<MochiArray>();
                    pair->elements.push_back(Value(k));
                    pair->elements.push_back(v);
                    arr->elements.push_back(Value(pair));
                }
                return Value(arr);
                };
            mod->entries["items"] = Value(itemsFunc);

            // dict.has(d, key) 返回 bool
            auto hasFunc = std::make_shared<NativeFunction>();
            hasFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.has(dict, key)");
                auto d = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string key = std::any_cast<std::string>(args[1]);
                return Value(d->entries.find(key) != d->entries.end());
                };
            mod->entries["has"] = Value(hasFunc);

            // dict.get(d, key, default?) 安全获取，不存在返回 nil 或 default
            auto getFunc = std::make_shared<NativeFunction>();
            getFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.get(dict, key, default?)");
                auto d = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string key = std::any_cast<std::string>(args[1]);
                auto it = d->entries.find(key);
                if (it != d->entries.end()) return it->second;
                if (args.size() >= 3) return args[2];
                return Value{};
                };
            mod->entries["get"] = Value(getFunc);

            // dict.set(d, key, value) 设置值，返回旧值或 nil
            auto setFunc = std::make_shared<NativeFunction>();
            setFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 3 || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.set(dict, key, value)");
                auto d = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string key = std::any_cast<std::string>(args[1]);
                Value old = Value{};
                auto it = d->entries.find(key);
                if (it != d->entries.end()) old = it->second;
                d->entries[key] = args[2];
                return old;
                };
            mod->entries["set"] = Value(setFunc);

            // dict.remove(d, key) 删除键，返回被删除的值或 nil
            auto removeFunc = std::make_shared<NativeFunction>();
            removeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2 || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.remove(dict, key)");
                auto d = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string key = std::any_cast<std::string>(args[1]);
                auto it = d->entries.find(key);
                if (it != d->entries.end()) {
                    Value removed = it->second;
                    d->entries.erase(it);
                    return removed;
                }
                return Value{};
                };
            mod->entries["remove"] = Value(removeFunc);

            // dict.clear(d) 清空字典
            auto clearFunc = std::make_shared<NativeFunction>();
            clearFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty() || args[0].type() != typeid(std::shared_ptr<MochiDict>))
                    throw std::runtime_error("dict.clear(dict)");
                auto d = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                d->entries.clear();
                return Value{};
                };
            mod->entries["clear"] = Value(clearFunc);

            return Value(mod);
            };

        // utf 标准库
        builtinModules["utf"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            /* 解码一个 UTF-8 字符，返回码点；若非法返回 -1，并只跳过 1 个字节 */
            auto decodeOne = [](const char*& p, const char* end) -> int {
                if (p >= end) return -1;
                unsigned char c = *p;
                int len = 0, code = 0;

                if (c < 0x80) {
                    len = 1;
                    code = c;
                }
                else if ((c & 0xE0) == 0xC0) {
                    len = 2;
                    code = c & 0x1F;
                }
                else if ((c & 0xF0) == 0xE0) {
                    len = 3;
                    code = c & 0x0F;
                }
                else if ((c & 0xF8) == 0xF0) {
                    len = 4;
                    code = c & 0x07;
                }
                else {
                    // 非法头字节，跳过 1 字节
                    ++p;
                    return -1;
                }

                if (p + len > end) {
                    // 不完整序列
                    ++p;
                    return -1;
                }
                for (int i = 1; i < len; ++i) {
                    unsigned char next = (unsigned char)p[i];
                    if ((next & 0xC0) != 0x80) {
                        // 后续字节非法，只跳过当前这个头字节
                        ++p;
                        return -1;
                    }
                    code = (code << 6) | (next & 0x3F);
                }
                p += len;
                return code;
                };

            // ---------- utf.valid(str) 是否合法 UTF-8 ----------
            auto validFunc = std::make_shared<NativeFunction>();
            validFunc->func = [decodeOne](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("utf.valid(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                const char* p = s.data();
                const char* end = p + s.size();
                while (p < end) {
                    if (decodeOne(p, end) < 0) return Value(false);
                }
                return Value(true);
                };
            mod->entries["valid"] = Value(validFunc);

            // ---------- utf.len(str) 字符数（跳过非法字节）----------
            auto lenFunc = std::make_shared<NativeFunction>();
            lenFunc->func = [decodeOne](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("utf.len(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                const char* p = s.data();
                const char* end = p + s.size();
                int count = 0;
                while (p < end) {
                    int code = decodeOne(p, end);
                    if (code >= 0) ++count;
                    // 非法字节已在 decodeOne 中跳过 1 字节，继续循环
                }
                return Value(static_cast<double>(count));
                };
            mod->entries["len"] = Value(lenFunc);

            // ---------- utf.sub(str, start, count?) ----------
            auto subFunc = std::make_shared<NativeFunction>();
            subFunc->func = [decodeOne](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("utf.sub(str, start [, count])");
                std::string s = std::any_cast<std::string>(args[0]);
                int start = static_cast<int>(std::any_cast<double>(args[1]));
                int count = -1;
                if (args.size() >= 3) count = static_cast<int>(std::any_cast<double>(args[2]));

                const char* p = s.data();
                const char* end = p + s.size();
                int idx = 0;
                const char* subStart = nullptr;
                const char* subEnd = nullptr;

                while (p < end) {
                    if (idx == start) subStart = p;
                    if (count >= 0 && idx == start + count) {
                        subEnd = p;
                        break;
                    }
                    int code = decodeOne(p, end);
                    if (code >= 0) ++idx;
                    // 非法字节也被跳过，但不会增加 idx
                }
                if (!subStart) return Value(std::string(""));
                if (!subEnd) subEnd = end;
                return Value(std::string(subStart, subEnd - subStart));
                };
            mod->entries["sub"] = Value(subFunc);

            // ---------- utf.charAt(str, index) ----------
            auto charAtFunc = std::make_shared<NativeFunction>();
            charAtFunc->func = [decodeOne](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("utf.charAt(str, index)");
                std::string s = std::any_cast<std::string>(args[0]);
                int index = static_cast<int>(std::any_cast<double>(args[1]));
                const char* p = s.data();
                const char* end = p + s.size();
                int idx = 0;
                while (p < end) {
                    const char* charStart = p;
                    int code = decodeOne(p, end);
                    if (code >= 0) {
                        if (idx == index) {
                            return Value(std::string(charStart, p - charStart));
                        }
                        ++idx;
                    }
                }
                throw std::runtime_error("utf.charAt: index out of range");
                };
            mod->entries["charAt"] = Value(charAtFunc);

            // ---------- utf.codes(str) ----------
            auto codesFunc = std::make_shared<NativeFunction>();
            codesFunc->func = [decodeOne](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("utf.codes(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                auto arr = std::make_shared<MochiArray>();
                const char* p = s.data();
                const char* end = p + s.size();
                while (p < end) {
                    int code = decodeOne(p, end);
                    if (code >= 0) arr->elements.push_back(Value(static_cast<double>(code)));
                }
                return Value(arr);
                };
            mod->entries["codes"] = Value(codesFunc);

            return Value(mod);
            };

        // dir 标准库
        builtinModules["dir"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            // 辅助：将 filesystem::path 转为 Mochi 字符串
            auto pathToStr = [](const std::filesystem::path& p) -> std::string {
                return p.string();
                };

            // ---------- dir.mkdir(path) ----------
            auto mkdirFunc = std::make_shared<NativeFunction>();
            mkdirFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("dir.mkdir(path)");
                std::string path = std::any_cast<std::string>(args[0]);
                if (std::filesystem::create_directory(path))
                    return Value{};
                throw std::runtime_error("Cannot create directory: " + path);
                };
            mod->entries["mkdir"] = Value(mkdirFunc);

            // ---------- dir.rmdir(path) ----------
            auto rmdirFunc = std::make_shared<NativeFunction>();
            rmdirFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("dir.rmdir(path)");
                std::string path = std::any_cast<std::string>(args[0]);
                if (std::filesystem::remove(path))
                    return Value{};
                throw std::runtime_error("Cannot remove directory: " + path);
                };
            mod->entries["rmdir"] = Value(rmdirFunc);

            // ---------- dir.isdir(path) ----------
            auto isdirFunc = std::make_shared<NativeFunction>();
            isdirFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("dir.isdir(path)");
                std::string path = std::any_cast<std::string>(args[0]);
                return Value(std::filesystem::is_directory(path));
                };
            mod->entries["isdir"] = Value(isdirFunc);

            // ---------- dir.cwd() ----------
            auto cwdFunc = std::make_shared<NativeFunction>();
            cwdFunc->func = [pathToStr](Interpreter*, const std::vector<Value>& args) -> Value {
                return Value(pathToStr(std::filesystem::current_path()));
                };
            mod->entries["cwd"] = Value(cwdFunc);

            // ---------- dir.chdir(path) ----------
            auto chdirFunc = std::make_shared<NativeFunction>();
            chdirFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("dir.chdir(path)");
                std::string path = std::any_cast<std::string>(args[0]);
                std::filesystem::current_path(path);
                return Value{};
                };
            mod->entries["chdir"] = Value(chdirFunc);

            // ---------- dir.walk(path) 返回可迭代对象 ----------
            auto walkFunc = std::make_shared<NativeFunction>();
            walkFunc->func = [pathToStr](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("dir.walk(path)");
                std::string root = std::any_cast<std::string>(args[0]);

                auto obj = std::make_shared<MochiDict>();
                auto iterFactory = std::make_shared<NativeFunction>();
                iterFactory->func = [root, pathToStr](Interpreter*, const std::vector<Value>&) -> Value {
                    using Iter = std::filesystem::recursive_directory_iterator;
                    struct State {
                        Iter iter;
                        Iter end;
                        State(const std::string& path) : iter(path), end() {}
                    };
                    auto state = std::make_shared<State>(root);

                    auto iterator = std::make_shared<NativeFunction>();
                    iterator->func = [state, pathToStr](Interpreter*, const std::vector<Value>&) -> Value {
                        if (state->iter == state->end) {
                            return Value{}; // nil 表示遍历结束
                        }
                        auto& entry = *state->iter;
                        auto info = std::make_shared<MochiDict>();
                        info->entries["path"] = Value(pathToStr(entry.path()));
                        info->entries["name"] = Value(pathToStr(entry.path().filename()));
                        info->entries["isdir"] = Value(entry.is_directory());
                        ++state->iter;
                        return info;
                        };
                    return Value(iterator);
                    };
                obj->entries["iter"] = Value(iterFactory);
                return Value(obj);
                };
            mod->entries["walk"] = Value(walkFunc);

            return Value(mod);
            };

        // ========== regex 模块 ==========
        builtinModules["regex"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            // 辅助：将 Mochi 字符串转为 std::string
            auto getStr = [](const Value& v) -> std::string {
                return std::any_cast<std::string>(v);
                };

            // regex.match(pattern, text) → bool
            auto matchFunc = std::make_shared<NativeFunction>();
            matchFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("regex.match(pattern, text)");
                std::string pattern = std::any_cast<std::string>(args[0]);
                std::string text = std::any_cast<std::string>(args[1]);
                try {
                    std::regex re(pattern);
                    return Value(std::regex_match(text, re));
                }
                catch (const std::regex_error& e) {
                    throw std::runtime_error("regex error: " + std::string(e.what()));
                }
                };
            mod->entries["match"] = Value(matchFunc);

            // regex.search(pattern, text) → dict { start, end, match, groups[] } 或 nil
            auto searchFunc = std::make_shared<NativeFunction>();
            searchFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("regex.search(pattern, text)");
                std::string pattern = std::any_cast<std::string>(args[0]);
                std::string text = std::any_cast<std::string>(args[1]);
                try {
                    std::regex re(pattern);
                    std::smatch matches;
                    if (std::regex_search(text, matches, re)) {
                        auto result = std::make_shared<MochiDict>();
                        result->entries["match"] = Value(matches.str(0));
                        result->entries["start"] = Value(static_cast<double>(matches.position(0)));
                        result->entries["end"] = Value(static_cast<double>(matches.position(0) + matches.length(0)));
                        auto groups = std::make_shared<MochiArray>();
                        for (size_t i = 1; i < matches.size(); ++i) {
                            if (matches[i].matched)
                                groups->elements.push_back(Value(matches.str(i)));
                            else
                                groups->elements.push_back(Value{}); // nil
                        }
                        result->entries["groups"] = Value(groups);
                        return Value(result);
                    }
                    return Value{}; // nil 未找到
                }
                catch (const std::regex_error& e) {
                    throw std::runtime_error("regex error: " + std::string(e.what()));
                }
                };
            mod->entries["search"] = Value(searchFunc);

            // regex.replace(pattern, replacement, text) → string
            auto replaceFunc = std::make_shared<NativeFunction>();
            replaceFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 3) throw std::runtime_error("regex.replace(pattern, replacement, text)");
                std::string pattern = std::any_cast<std::string>(args[0]);
                std::string replacement = std::any_cast<std::string>(args[1]);
                std::string text = std::any_cast<std::string>(args[2]);
                try {
                    std::regex re(pattern);
                    return Value(std::regex_replace(text, re, replacement));
                }
                catch (const std::regex_error& e) {
                    throw std::runtime_error("regex error: " + std::string(e.what()));
                }
                };
            mod->entries["replace"] = Value(replaceFunc);

            return Value(mod);
            };

        // ========== gui 模块 (基于 SDL2，需链接 -lSDL2) ==========
        builtinModules["gui"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            // ---------- 辅助：加载默认字体（主线程执行）----------
            auto loadFontTask = []() {
                if (!g_font) {
                    TTF_Init();
                    // 尝试几个常见字体路径
                    g_font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 18);
                    if (!g_font) g_font = TTF_OpenFont("C:\\Windows\\Fonts\\arial.ttf", 18);
                    if (!g_font) g_font = TTF_OpenFont("/System/Library/Fonts/Helvetica.ttc", 18);
                    if (!g_font) {
                        // 如果没找到，用内置简图？直接 nullptr，图形模式运行
                        std::cerr << "Warning: No TTF font found, text will be invisible." << std::endl;
                    }
                }
                };

            // ---------- gui.createWindow ----------
            auto createWindowFunc = std::make_shared<NativeFunction>();
            createWindowFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 3) throw std::runtime_error("gui.createWindow(title, width, height)");
                std::string title = std::any_cast<std::string>(args[0]);
                int w = static_cast<int>(std::any_cast<double>(args[1]));
                int h = static_cast<int>(std::any_cast<double>(args[2]));
                int win_style = SDL_WINDOW_SHOWN;
                if (args.size() > 3 && args[3].type() == typeid(double))
                    win_style = static_cast<int>(std::any_cast<double>(args[3]));
                    

                int id = g_nextWidgetId++;
                g_widgets[id] = { "window", {0,0,w,h}, title, nullptr };
                static bool sdlInited = false;
                pushGUITask([title, w, h, id, win_style]() {
                    if (!sdlInited) {
                        SDL_Init(SDL_INIT_VIDEO);
                        sdlInited = true;
                    }
                    g_window = SDL_CreateWindow(title.c_str(),
                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, win_style);
                    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
                    initGUIFont();  // 直接调用全局函数初始化字体
                    });

                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(id));
                proxy->entries["type"] = Value(std::string("window"));
                return Value(proxy);
                };
            mod->entries["createWindow"] = Value(createWindowFunc);

            // ---------- gui.addLayer ----------
            auto addLayerFunc = std::make_shared<NativeFunction>();
            addLayerFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.addLayer(win, name)");
                auto winProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int winId = static_cast<int>(std::any_cast<double>(winProxy->entries["__id__"]));
                std::string name = std::any_cast<std::string>(args[1]);
                int layerId = g_nextWidgetId++;
                g_widgets[layerId] = { "layer", {0,0,0,0}, name, nullptr };
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(layerId));
                proxy->entries["type"] = Value(std::string("layer"));
                proxy->entries["winId"] = Value(static_cast<double>(winId));
                return Value(proxy);
                };
            mod->entries["addLayer"] = Value(addLayerFunc);

            // ---------- gui.addButton ----------
            auto addButtonFunc = std::make_shared<NativeFunction>();
            addButtonFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 7) throw std::runtime_error("gui.addButton(layer, name, text, x, y, w, h [, callback])");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                std::string text = std::any_cast<std::string>(args[2]);
                int x = static_cast<int>(std::any_cast<double>(args[3]));
                int y = static_cast<int>(std::any_cast<double>(args[4]));
                int w = static_cast<int>(std::any_cast<double>(args[5]));
                int h = static_cast<int>(std::any_cast<double>(args[6]));
                std::shared_ptr<MochiFunction> callback = nullptr;
                if (args.size() >= 8 && args[7].type() == typeid(std::shared_ptr<MochiFunction>))
                    callback = std::any_cast<std::shared_ptr<MochiFunction>>(args[7]);

                int btnId = g_nextWidgetId++;
                g_widgets[btnId] = { "button", {x, y, w, h}, text, callback };
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(btnId));
                proxy->entries["type"] = Value(std::string("button"));
                return Value(proxy);
                };
            mod->entries["addButton"] = Value(addButtonFunc);

            // ---------- gui.setCallback ----------
            auto setCallbackFunc = std::make_shared<NativeFunction>();
            setCallbackFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.setCallback(btn, fn)");
                auto btnProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(btnProxy->entries["__id__"]));
                auto fn = std::any_cast<std::shared_ptr<MochiFunction>>(args[1]);
                pushGUITask([id, fn]() {
                    if (g_widgets.count(id)) g_widgets[id].callback = fn;
                    });
                return Value{};
                };
            mod->entries["setCallback"] = Value(setCallbackFunc);

            // ========== gui.addLabel ==========
            auto addLabelFunc = std::make_shared<NativeFunction>();
            addLabelFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 5) throw std::runtime_error("gui.addLabel(layer, name, text, x, y)");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                std::string text = std::any_cast<std::string>(args[2]);
                int x = static_cast<int>(std::any_cast<double>(args[3]));
                int y = static_cast<int>(std::any_cast<double>(args[4]));

                int id = g_nextWidgetId++;
                g_widgets[id] = { "label", {x, y, 0, 0}, text, nullptr };
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(id));
                proxy->entries["type"] = Value(std::string("label"));
                return Value(proxy);
                };
            mod->entries["addLabel"] = Value(addLabelFunc);

            // ========== gui.addTextField ==========
            auto addTextFieldFunc = std::make_shared<NativeFunction>();
            addTextFieldFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 7) throw std::runtime_error("gui.addTextField(layer, name, text, x, y, w, h [, onChange])");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                std::string text = std::any_cast<std::string>(args[2]);
                int x = static_cast<int>(std::any_cast<double>(args[3]));
                int y = static_cast<int>(std::any_cast<double>(args[4]));
                int w = static_cast<int>(std::any_cast<double>(args[5]));
                int h = static_cast<int>(std::any_cast<double>(args[6]));
                std::shared_ptr<MochiFunction> onChange = nullptr;
                if (args.size() >= 8 && args[7].type() == typeid(std::shared_ptr<MochiFunction>))
                    onChange = std::any_cast<std::shared_ptr<MochiFunction>>(args[7]);

                int id = g_nextWidgetId++;
                g_widgets[id] = { "textfield", {x, y, w, h}, text, nullptr };
                g_widgets[id].onChange = onChange;
                g_widgets[id].cursorCol = utf8Length(text);   // 初始光标在文本末尾
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(id));
                proxy->entries["type"] = Value(std::string("textfield"));
                return Value(proxy);
                };
            mod->entries["addTextField"] = Value(addTextFieldFunc);

            // ========== gui.addCheckBox ==========
            auto addCheckBoxFunc = std::make_shared<NativeFunction>();
            addCheckBoxFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 6) throw std::runtime_error("gui.addCheckBox(layer, name, text, x, y, w [, callback])");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                std::string text = std::any_cast<std::string>(args[2]);
                int x = static_cast<int>(std::any_cast<double>(args[3]));
                int y = static_cast<int>(std::any_cast<double>(args[4]));
                int w = static_cast<int>(std::any_cast<double>(args[5])); // 总宽度（含方框和文字）
                std::shared_ptr<MochiFunction> callback = nullptr;
                if (args.size() >= 7 && args[6].type() == typeid(std::shared_ptr<MochiFunction>))
                    callback = std::any_cast<std::shared_ptr<MochiFunction>>(args[6]);

                int id = g_nextWidgetId++;
                g_widgets[id] = { "checkbox", {x, y, w, 20}, text, callback }; // 固定高度20
                g_widgets[id].checked = false;
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(id));
                proxy->entries["type"] = Value(std::string("checkbox"));
                return Value(proxy);
                };
            mod->entries["addCheckBox"] = Value(addCheckBoxFunc);

            // ========== gui.addSlider ==========
            auto addSliderFunc = std::make_shared<NativeFunction>();
            addSliderFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 7) throw std::runtime_error("gui.addSlider(layer, name, min, max, step, x, y, w [, callback])");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                double min = std::any_cast<double>(args[2]);
                double max = std::any_cast<double>(args[3]);
                double step = std::any_cast<double>(args[4]);
                int x = static_cast<int>(std::any_cast<double>(args[5]));
                int y = static_cast<int>(std::any_cast<double>(args[6]));
                int w = static_cast<int>(std::any_cast<double>(args[7]));
                std::shared_ptr<MochiFunction> callback = nullptr;
                if (args.size() >= 9 && args[8].type() == typeid(std::shared_ptr<MochiFunction>))
                    callback = std::any_cast<std::shared_ptr<MochiFunction>>(args[8]);

                int id = g_nextWidgetId++;
                g_widgets[id] = { "slider", {x, y, w, 20}, std::to_string((min + max) / 2), callback }; // 默认值中间
                g_widgets[id].min = min; g_widgets[id].max = max; g_widgets[id].step = step;
                g_widgets[id].value = (min + max) / 2;
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(id));
                proxy->entries["type"] = Value(std::string("slider"));
                return Value(proxy);
                };
            mod->entries["addSlider"] = Value(addSliderFunc);

            // ========== gui.addProgressBar ==========
            auto addProgressBarFunc = std::make_shared<NativeFunction>();
            addProgressBarFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 5) throw std::runtime_error("gui.addProgressBar(layer, name, x, y, w)");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                int x = static_cast<int>(std::any_cast<double>(args[2]));
                int y = static_cast<int>(std::any_cast<double>(args[3]));
                int w = static_cast<int>(std::any_cast<double>(args[4]));

                int id = g_nextWidgetId++;
                g_widgets[id] = { "progressbar", {x, y, w, 20}, "", nullptr };
                g_widgets[id].value = 0.0;
                g_widgets[id].max = 100.0;
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(id));
                proxy->entries["type"] = Value(std::string("progressbar"));
                return Value(proxy);
                };
            mod->entries["addProgressBar"] = Value(addProgressBarFunc);

            // ========== gui.addMemo(layer, name, x, y, w, h) ==========
            auto addMemoFunc = std::make_shared<NativeFunction>();
            addMemoFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 6) throw std::runtime_error("gui.addMemo(layer, name, x, y, w, h)");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                int x = static_cast<int>(std::any_cast<double>(args[2]));
                int y = static_cast<int>(std::any_cast<double>(args[3]));
                int w = static_cast<int>(std::any_cast<double>(args[4]));
                int h = static_cast<int>(std::any_cast<double>(args[5]));

                int id = g_nextWidgetId++;
                g_widgets[id] = { "memo", {x, y, w, h}, "", nullptr };
                g_widgets[id].lines.push_back("");   // 至少有一行
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(id));
                proxy->entries["type"] = Value(std::string("memo"));
                return Value(proxy);
                };
            mod->entries["addMemo"] = Value(addMemoFunc);

            // ---------- 辅助：创建布局代理并绑定方法 ----------
            auto createLayoutProxy = [&](int id, const std::string& type) -> Value {
                auto proxy = std::make_shared<MochiDict>();
                proxy->entries["__id__"] = Value(static_cast<double>(id));
                proxy->entries["type"] = Value(type);

                // 内部辅助：添加控件后自动布局
                auto addToLayout = [id](int childId) {
                    g_widgets[id].children.push_back(childId);
                    relayout(id);
                    };

                // 属性设置
                auto setSpacing = std::make_shared<NativeFunction>();
                setSpacing->func = [id](Interpreter*, const std::vector<Value>& args) {
                    if (args.empty()) throw std::runtime_error("setSpacing(n)");
                    int n = static_cast<int>(std::any_cast<double>(args[0]));
                    g_widgets[id].spacing = n;
                    relayout(id);
                    return Value{};
                    };
                proxy->entries["setSpacing"] = Value(setSpacing);

                auto setPadding = std::make_shared<NativeFunction>();
                setPadding->func = [id](Interpreter*, const std::vector<Value>& args) {
                    if (args.empty()) throw std::runtime_error("setPadding(n)");
                    int n = static_cast<int>(std::any_cast<double>(args[0]));
                    g_widgets[id].padding = n;
                    relayout(id);
                    return Value{};
                    };
                proxy->entries["setPadding"] = Value(setPadding);

                /// addLabel（自动测量）
                auto addLabel = std::make_shared<NativeFunction>();
                addLabel->func = [id, addToLayout](Interpreter*, const std::vector<Value>& args) {
                    if (args.empty()) throw std::runtime_error("label(text)");
                    std::string text = std::any_cast<std::string>(args[0]);
                    int newId = g_nextWidgetId++;

                    int labelW = 80;   // 默认宽度（字体未加载时使用）
                    int labelH = 22;   // 固定高度
                    if (g_font) {
                        TTF_SizeUTF8(g_font, text.c_str(), &labelW, &labelH);
                        labelW += 4;   // 加一点内边距，避免文字贴边
                    }

                    g_widgets[newId] = { "label", {0, 0, labelW, labelH}, text, nullptr };
                    addToLayout(newId);
                    return Value(static_cast<double>(newId));
                    };
                proxy->entries["addLabel"] = Value(addLabel);

                // addButton
                auto addButton = std::make_shared<NativeFunction>();
                addButton->func = [id, addToLayout](Interpreter* interp, const std::vector<Value>& args) {
                    if (args.size() < 3) throw std::runtime_error("button(text, w, h [, cb])");
                    std::string text = std::any_cast<std::string>(args[0]);
                    int w = static_cast<int>(std::any_cast<double>(args[1]));
                    int h = static_cast<int>(std::any_cast<double>(args[2]));
                    std::shared_ptr<MochiFunction> cb = nullptr;
                    if (args.size() >= 4 && args[3].type() == typeid(std::shared_ptr<MochiFunction>))
                        cb = std::any_cast<std::shared_ptr<MochiFunction>>(args[3]);
                    int newId = g_nextWidgetId++;
                    g_widgets[newId] = { "button", {0,0,w,h}, text, cb };
                    addToLayout(newId);
                    return Value(static_cast<double>(newId));
                    };
                proxy->entries["addButton"] = Value(addButton);

                // addTextField
                auto addTextField = std::make_shared<NativeFunction>();
                addTextField->func = [id, addToLayout](Interpreter*, const std::vector<Value>& args) {
                    if (args.size() < 3) throw std::runtime_error("textField(text, w, h [, onChange])");
                    std::string text = std::any_cast<std::string>(args[0]);
                    int w = static_cast<int>(std::any_cast<double>(args[1]));
                    int h = static_cast<int>(std::any_cast<double>(args[2]));
                    std::shared_ptr<MochiFunction> onChange = nullptr;
                    if (args.size() >= 4 && args[3].type() == typeid(std::shared_ptr<MochiFunction>))
                        onChange = std::any_cast<std::shared_ptr<MochiFunction>>(args[3]);
                    int newId = g_nextWidgetId++;
                    g_widgets[newId] = { "textfield", {0,0,w,h}, text, nullptr };
                    g_widgets[newId].onChange = onChange;
                    g_widgets[newId].cursorCol = utf8Length(text);
                    addToLayout(newId);
                    return Value(static_cast<double>(newId));
                    };
                proxy->entries["addTextField"] = Value(addTextField);

                // addCheckBox
                auto addCheckBox = std::make_shared<NativeFunction>();
                addCheckBox->func = [id, addToLayout](Interpreter*, const std::vector<Value>& args) {
                    if (args.size() < 2) throw std::runtime_error("checkBox(text, w [, cb])");
                    std::string text = std::any_cast<std::string>(args[0]);
                    int w = static_cast<int>(std::any_cast<double>(args[1]));
                    std::shared_ptr<MochiFunction> cb = nullptr;
                    if (args.size() >= 3 && args[2].type() == typeid(std::shared_ptr<MochiFunction>))
                        cb = std::any_cast<std::shared_ptr<MochiFunction>>(args[2]);
                    int newId = g_nextWidgetId++;
                    g_widgets[newId] = { "checkbox", {0,0,w,20}, text, cb };
                    addToLayout(newId);
                    return Value(static_cast<double>(newId));
                    };
                proxy->entries["addCheckBox"] = Value(addCheckBox);

                // addSlider
                auto addSlider = std::make_shared<NativeFunction>();
                addSlider->func = [id, addToLayout](Interpreter*, const std::vector<Value>& args) {
                    if (args.size() < 4) throw std::runtime_error("slider(min, max, step, w [, cb])");
                    double min = std::any_cast<double>(args[0]);
                    double max = std::any_cast<double>(args[1]);
                    double step = std::any_cast<double>(args[2]);
                    int w = static_cast<int>(std::any_cast<double>(args[3]));
                    std::shared_ptr<MochiFunction> cb = nullptr;
                    if (args.size() >= 5 && args[4].type() == typeid(std::shared_ptr<MochiFunction>))
                        cb = std::any_cast<std::shared_ptr<MochiFunction>>(args[4]);
                    int newId = g_nextWidgetId++;
                    g_widgets[newId] = { "slider", {0,0,w,24}, std::to_string((min + max) / 2), cb };
                    g_widgets[newId].min = min; g_widgets[newId].max = max; g_widgets[newId].step = step;
                    g_widgets[newId].value = (min + max) / 2;
                    addToLayout(newId);
                    return Value(static_cast<double>(newId));
                    };
                proxy->entries["addSlider"] = Value(addSlider);

                // addProgressBar
                auto addProgressBar = std::make_shared<NativeFunction>();
                addProgressBar->func = [id, addToLayout](Interpreter*, const std::vector<Value>& args) {
                    if (args.empty()) throw std::runtime_error("progressBar(w)");
                    int w = static_cast<int>(std::any_cast<double>(args[0]));
                    int newId = g_nextWidgetId++;
                    g_widgets[newId] = { "progressbar", {0,0,w,20}, "", nullptr };
                    g_widgets[newId].max = 100;
                    addToLayout(newId);
                    return Value(static_cast<double>(newId));
                    };
                proxy->entries["addProgressBar"] = Value(addProgressBar);

                // addMemo
                auto addMemo = std::make_shared<NativeFunction>();
                addMemo->func = [id, addToLayout](Interpreter*, const std::vector<Value>& args) {
                    if (args.size() < 2) throw std::runtime_error("memo(w, h)");
                    int w = static_cast<int>(std::any_cast<double>(args[0]));
                    int h = static_cast<int>(std::any_cast<double>(args[1]));
                    int newId = g_nextWidgetId++;
                    g_widgets[newId] = { "memo", {0,0,w,h}, "", nullptr };
                    g_widgets[newId].lines.push_back("");
                    addToLayout(newId);
                    return Value(static_cast<double>(newId));
                    };
                proxy->entries["addMemo"] = Value(addMemo);

                return Value(proxy);
                };

            // ---------- addHLayout / addVLayout ----------
            auto addHLayoutFunc = std::make_shared<NativeFunction>();
            addHLayoutFunc->func = [createLayoutProxy](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 5) throw std::runtime_error("gui.addHLayout(layer, name, x, y, w, h)");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                int x = static_cast<int>(std::any_cast<double>(args[2]));
                int y = static_cast<int>(std::any_cast<double>(args[3]));
                int w = static_cast<int>(std::any_cast<double>(args[4]));
                int h = static_cast<int>(std::any_cast<double>(args[5]));

                int id = g_nextWidgetId++;
                g_widgets[id] = { "hlayout", {x,y,w,h}, name, nullptr };
                g_widgets[id].layoutType = "h";
                return createLayoutProxy(id, "hlayout");
                };
            mod->entries["addHLayout"] = Value(addHLayoutFunc);

            auto addVLayoutFunc = std::make_shared<NativeFunction>();
            addVLayoutFunc->func = [createLayoutProxy](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 5) throw std::runtime_error("gui.addVLayout(layer, name, x, y, w, h)");
                auto layerProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                std::string name = std::any_cast<std::string>(args[1]);
                int x = static_cast<int>(std::any_cast<double>(args[2]));
                int y = static_cast<int>(std::any_cast<double>(args[3]));
                int w = static_cast<int>(std::any_cast<double>(args[4]));
                int h = static_cast<int>(std::any_cast<double>(args[5]));

                int id = g_nextWidgetId++;
                g_widgets[id] = { "vlayout", {x,y,w,h}, name, nullptr };
                g_widgets[id].layoutType = "v";
                return createLayoutProxy(id, "vlayout");
                };
            mod->entries["addVLayout"] = Value(addVLayoutFunc);

            // gui.getMemoText(ctrl)
            auto getMemoTextFunc = std::make_shared<NativeFunction>();
            getMemoTextFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 1) throw std::runtime_error("gui.getMemoText(control)");
                auto ctrl = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrl->entries["__id__"]));
                if (!g_widgets.count(id)) return Value(std::string(""));
                std::string full;
                for (size_t i = 0; i < g_widgets[id].lines.size(); ++i) {
                    if (i > 0) full += "\n";
                    full += g_widgets[id].lines[i];
                }
                return Value(full);
                };
            mod->entries["getMemoText"] = Value(getMemoTextFunc);

            // gui.setMemoText(ctrl, text)
            auto setMemoTextFunc = std::make_shared<NativeFunction>();
            setMemoTextFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.setMemoText(control, text)");
                auto ctrl = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrl->entries["__id__"]));
                std::string text = std::any_cast<std::string>(args[1]);
                pushGUITask([id, text]() {
                    if (!g_widgets.count(id)) return;
                    auto& w = g_widgets[id];
                    w.lines.clear();
                    std::istringstream iss(text);
                    std::string line;
                    while (std::getline(iss, line)) w.lines.push_back(line);
                    if (w.lines.empty()) w.lines.push_back("");
                    w.cursorLine = 0; w.cursorCol = 0; w.scrollY = 0;
                    });
                return Value{};
                };
            mod->entries["setMemoText"] = Value(setMemoTextFunc);

            // gui.setText(control, newText)
            auto setTextFunc = std::make_shared<NativeFunction>();
            setTextFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.setText(control, newText)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                std::string newText = std::any_cast<std::string>(args[1]);
                pushGUITask([id, newText]() {
                    if (g_widgets.count(id)) g_widgets[id].text = newText;
                    });
                return Value{};
                };
            mod->entries["setText"] = Value(setTextFunc);

            // gui.setValue(control, value)  -- 用于 slider, progressbar
            auto setValueFunc = std::make_shared<NativeFunction>();
            setValueFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.setValue(control, value)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                double newVal = std::any_cast<double>(args[1]);
                pushGUITask([id, newVal]() {
                    if (g_widgets.count(id)) {
                        auto& w = g_widgets[id];
                        if (w.type == "slider" || w.type == "progressbar") {
                            w.value = newVal;
                            // 更新显示文本（slider 可显示值）
                            if (w.type == "slider") w.text = std::to_string(newVal);
                        }
                    }
                    });
                return Value{};
                };
            mod->entries["setValue"] = Value(setValueFunc);

            // gui.setChecked(control, checked)
            auto setCheckedFunc = std::make_shared<NativeFunction>();
            setCheckedFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.setChecked(control, bool)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                bool checked = std::any_cast<bool>(args[1]);
                pushGUITask([id, checked]() {
                    if (g_widgets.count(id) && g_widgets[id].type == "checkbox") {
                        g_widgets[id].checked = checked;
                    }
                    });
                return Value{};
                };
            mod->entries["setChecked"] = Value(setCheckedFunc);

            // gui.getValue(control) -> 获取值（仅主线程同步安全，但也可以返回缓存值）
            // 由于异步，获取可能不是最新，但方便简单场景，直接返回 widget 存储的 value
            auto getValueFunc = std::make_shared<NativeFunction>();
            getValueFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 1) throw std::runtime_error("gui.getValue(control)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                // 直接返回全局 widget 的 value（主线程访问，但调用 getValue 通常在回调中，是主线程，安全）
                if (g_widgets.count(id)) return Value(g_widgets[id].value);
                return Value(0.0);
                };
            mod->entries["getValue"] = Value(getValueFunc);

            // ---------- gui.getText(control) ----------
            auto getTextFunc = std::make_shared<NativeFunction>();
            getTextFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 1) throw std::runtime_error("gui.getText(control)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                return g_widgets.count(id) ? Value(g_widgets[id].text) : Value(std::string(""));
                };
            mod->entries["getText"] = Value(getTextFunc);

            // ---------- gui.isChecked(control) ----------
            auto isCheckedFunc = std::make_shared<NativeFunction>();
            isCheckedFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 1) throw std::runtime_error("gui.isChecked(control)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                return (g_widgets.count(id) && g_widgets[id].type == "checkbox") ? Value(g_widgets[id].checked) : Value(false);
                };
            mod->entries["isChecked"] = Value(isCheckedFunc);

            // ---------- gui.setEnabled(control, bool) ----------
            auto setEnabledFunc = std::make_shared<NativeFunction>();
            setEnabledFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.setEnabled(control, bool)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                bool enabled = std::any_cast<bool>(args[1]);
                pushGUITask([id, enabled]() {
                    if (g_widgets.count(id)) g_widgets[id].enabled = enabled;
                    });
                return Value{};
                };
            mod->entries["setEnabled"] = Value(setEnabledFunc);

            // ---------- gui.setVisible(control, bool) ----------
            auto setVisibleFunc = std::make_shared<NativeFunction>();
            setVisibleFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.setVisible(control, bool)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                bool visible = std::any_cast<bool>(args[1]);
                pushGUITask([id, visible]() {
                    if (g_widgets.count(id)) g_widgets[id].visible = visible;
                    });
                return Value{};
                };
            mod->entries["setVisible"] = Value(setVisibleFunc);

            // ---------- gui.setPassword(control, bool) ----------
            auto setPasswordFunc = std::make_shared<NativeFunction>();
            setPasswordFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("gui.setPassword(control, bool)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                bool isPassword = std::any_cast<bool>(args[1]);
                pushGUITask([id, isPassword]() {
                    if (g_widgets.count(id)) g_widgets[id].password = isPassword;
                    });
                return Value{};
                };
            mod->entries["setPassword"] = Value(setPasswordFunc);

            // ---------- gui.setFocus(control) ----------
            auto setFocusFunc = std::make_shared<NativeFunction>();
            setFocusFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 1) throw std::runtime_error("gui.setFocus(control)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                pushGUITask([id]() {
                    for (auto& [_, w] : g_widgets) w.focused = false;
                    if (g_widgets.count(id)) {
                        g_widgets[id].focused = true;
                        SDL_StartTextInput();
                    }
                    });
                return Value{};
                };
            mod->entries["setFocus"] = Value(setFocusFunc);

            // ---------- gui.clear(control) ----------
            auto clearFunc = std::make_shared<NativeFunction>();
            clearFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 1) throw std::runtime_error("gui.clear(control)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                pushGUITask([id]() {
                    if (g_widgets.count(id)) g_widgets[id].text = "";
                    });
                return Value{};
                };
            mod->entries["clear"] = Value(clearFunc);

            // ---------- gui.setProgressRange(control, min, max) ----------
            auto setProgressRangeFunc = std::make_shared<NativeFunction>();
            setProgressRangeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 3) throw std::runtime_error("gui.setProgressRange(control, min, max)");
                auto ctrlProxy = std::any_cast<std::shared_ptr<MochiDict>>(args[0]);
                int id = static_cast<int>(std::any_cast<double>(ctrlProxy->entries["__id__"]));
                double min = std::any_cast<double>(args[1]);
                double max = std::any_cast<double>(args[2]);
                pushGUITask([id, min, max]() {
                    if (g_widgets.count(id) && g_widgets[id].type == "progressbar") {
                        g_widgets[id].min = min;
                        g_widgets[id].max = max;
                    }
                    });
                return Value{};
                };
            mod->entries["setProgressRange"] = Value(setProgressRangeFunc);

            // ---------- gui.run() 美化版主循环 ----------
            auto runFunc = std::make_shared<NativeFunction>();
            runFunc->func = [this](Interpreter*, const std::vector<Value>&) -> Value {
                initGUIFont();

                // 安全回调辅助函数（捕获 this）
                auto safeCall = [this](std::shared_ptr<MochiFunction> fn, const std::vector<Value>& args) {
                    if (!fn) return;
                    try {
                        this->callFunction(fn, args);
                    }
                    catch (const ReturnException&) {
                        // 忽略回调中的 return
                    }
                    catch (const std::exception& ex) {
                        std::cerr << "GUI callback error: " << ex.what() << std::endl;
                    }
                    };

                while (g_guiRunning) {
                    // 处理任务队列
                    {
                        std::unique_lock<std::mutex> lock(g_guiTaskMutex);
                        while (!g_guiTaskQueue.empty()) {
                            auto task = std::move(g_guiTaskQueue.front());
                            g_guiTaskQueue.pop();
                            lock.unlock();
                            task.action();
                            lock.lock();
                        }
                    }

                    // 事件处理
                    SDL_Event e;
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    g_hoveredWidgetId = -1;

                    while (SDL_PollEvent(&e)) {
                        if (e.type == SDL_QUIT) {
                            g_guiRunning = false;
                        }
                        else if (e.type == SDL_MOUSEBUTTONDOWN) {
                            if (e.button.button == SDL_BUTTON_LEFT) {
                                for (auto& [id, w] : g_widgets) {
                                    if (!w.visible || !w.enabled) continue;
                                    bool inside = (mx >= w.rect.x && mx <= w.rect.x + w.rect.w &&
                                        my >= w.rect.y && my <= w.rect.y + w.rect.h);
                                    if (!inside) continue;

                                    if (w.type == "button") {
                                        safeCall(w.callback, {});
                                    }
                                    else if (w.type == "checkbox") {
                                        w.checked = !w.checked;
                                        safeCall(w.callback, { Value(w.checked) });
                                    }
                                    else if (w.type == "textfield" && inside) {
                                        for (auto& [id2, w2] : g_widgets) w2.focused = false;
                                        w.focused = true;
                                        SDL_StartTextInput();
                                        int xInField = mx - w.rect.x - 8;
                                        if (xInField < 0) xInField = 0;

                                        if (w.password) {
                                            std::string stars(utf8Length(w.text), '*');   // 字符数
                                            w.cursorCol = getCharColFromX(stars, g_font, xInField);
                                            int maxCol = utf8Length(w.text);
                                            if (w.cursorCol > maxCol) w.cursorCol = maxCol;
                                        }
                                        else {
                                            w.cursorCol = getCharColFromX(w.text, g_font, xInField);
                                            int maxCol = utf8Length(w.text);
                                            if (w.cursorCol > maxCol) w.cursorCol = maxCol;
                                        }
                                    }
                                    else if (w.type == "memo") {
                                        for (auto& [id2, w2] : g_widgets) w2.focused = false;
                                        w.focused = true;
                                        SDL_StartTextInput();
                                        int lineH = 22;
                                        int yOffset = w.rect.y + 4;
                                        int clickedLine = w.scrollY + (my - yOffset) / lineH;
                                        if (clickedLine >= (int)w.lines.size()) clickedLine = (int)w.lines.size() - 1;
                                        if (clickedLine < 0) clickedLine = 0;
                                        w.cursorLine = clickedLine;
                                        int xInLine = mx - w.rect.x - 8;
                                        if (xInLine < 0) xInLine = 0;
                                        w.cursorCol = getCharColFromX(w.lines[clickedLine], g_font, xInLine);
                                        int maxCol = utf8Length(w.lines[clickedLine]);
                                        if (w.cursorCol > maxCol) w.cursorCol = maxCol;
                                    }
                                    else if (w.type == "slider") {
                                        w.focused = true;
                                    }
                                }
                            }
                        }
                        else if (e.type == SDL_MOUSEBUTTONUP) {
                            for (auto& [id, w] : g_widgets) {
                                if (w.type == "slider") w.focused = false;
                            }
                        }
                        else if (e.type == SDL_MOUSEMOTION) {
                            for (auto& [id, w] : g_widgets) {
                                if (w.type == "slider" && w.focused) {
                                    double ratio = (double)(mx - w.rect.x) / (double)w.rect.w;
                                    if (ratio < 0.0) ratio = 0.0;
                                    if (ratio > 1.0) ratio = 1.0;
                                    double newVal = w.min + ratio * (w.max - w.min);
                                    if (w.step > 0) newVal = std::round(newVal / w.step) * w.step;
                                    if (newVal < w.min) newVal = w.min;
                                    if (newVal > w.max) newVal = w.max;
                                    w.value = newVal;
                                    w.text = std::to_string(newVal);
                                    safeCall(w.callback, { Value(newVal) });
                                }
                            }
                        }
                        else if (e.type == SDL_TEXTINPUT) {
                            for (auto& [id, w] : g_widgets) {
                                if (!w.visible || !w.enabled || !w.focused) continue;
                                if (w.type == "textfield") {
                                    int bytePos = charIndexToByte(w.text, w.cursorCol);
                                    w.text.insert(bytePos, e.text.text);
                                    w.cursorCol += utf8Length(e.text.text);
                                    safeCall(w.onChange, { Value(w.text) });
                                }
                                else if (w.type == "memo") {
                                    int bytePos = charIndexToByte(w.lines[w.cursorLine], w.cursorCol);
                                    w.lines[w.cursorLine].insert(bytePos, e.text.text);
                                    w.cursorCol += utf8Length(e.text.text);
                                    safeCall(w.onChange, { Value(w.lines[w.cursorLine]) });
                                }
                            }
                        }
                        else if (e.type == SDL_KEYDOWN) {
                            for (auto& [id, w] : g_widgets) {
                                if (!w.visible || !w.enabled || !w.focused) continue;
                                if (w.type == "textfield") {
                                    int maxCol = utf8Length(w.text);
                                    switch (e.key.keysym.sym) {
                                    case SDLK_BACKSPACE:
                                        if (w.cursorCol > 0) { eraseCharAt(w.text, w.cursorCol - 1); w.cursorCol--; }
                                        break;
                                    case SDLK_DELETE:
                                        if (w.cursorCol < maxCol) { eraseCharAt(w.text, w.cursorCol); }
                                        break;
                                    case SDLK_LEFT: if (w.cursorCol > 0) w.cursorCol--; break;
                                    case SDLK_RIGHT: if (w.cursorCol < maxCol) w.cursorCol++; break;
                                    case SDLK_HOME: w.cursorCol = 0; break;
                                    case SDLK_END: w.cursorCol = maxCol; break;
                                    }
                                    safeCall(w.onChange, { Value(w.text) });
                                }
                                else if (w.type == "memo") {
                                    switch (e.key.keysym.sym) {
                                    case SDLK_BACKSPACE:
                                        if (w.cursorCol > 0) {
                                            eraseCharAt(w.lines[w.cursorLine], w.cursorCol - 1);
                                            w.cursorCol--;
                                        }
                                        else if (w.cursorLine > 0) {
                                            int prevLen = utf8Length(w.lines[w.cursorLine - 1]);
                                            w.lines[w.cursorLine - 1] += w.lines[w.cursorLine];
                                            w.lines.erase(w.lines.begin() + w.cursorLine);
                                            w.cursorLine--;
                                            w.cursorCol = prevLen;
                                        }
                                        break;
                                    case SDLK_DELETE:
                                        if (w.cursorCol < utf8Length(w.lines[w.cursorLine])) {
                                            eraseCharAt(w.lines[w.cursorLine], w.cursorCol);
                                        }
                                        else if (w.cursorLine < (int)w.lines.size() - 1) {
                                            w.lines[w.cursorLine] += w.lines[w.cursorLine + 1];
                                            w.lines.erase(w.lines.begin() + w.cursorLine + 1);
                                        }
                                        break;
                                    case SDLK_LEFT:
                                        if (w.cursorCol > 0) w.cursorCol--;
                                        else if (w.cursorLine > 0) {
                                            w.cursorLine--;
                                            w.cursorCol = utf8Length(w.lines[w.cursorLine]);
                                        }
                                        break;
                                    case SDLK_RIGHT:
                                        if (w.cursorCol < utf8Length(w.lines[w.cursorLine])) w.cursorCol++;
                                        else if (w.cursorLine < (int)w.lines.size() - 1) {
                                            w.cursorLine++;
                                            w.cursorCol = 0;
                                        }
                                        break;
                                    case SDLK_UP:
                                        if (w.cursorLine > 0) w.cursorLine--;
                                        if (w.cursorCol > utf8Length(w.lines[w.cursorLine]))
                                            w.cursorCol = utf8Length(w.lines[w.cursorLine]);
                                        break;
                                    case SDLK_DOWN:
                                        if (w.cursorLine < (int)w.lines.size() - 1) w.cursorLine++;
                                        if (w.cursorCol > utf8Length(w.lines[w.cursorLine]))
                                            w.cursorCol = utf8Length(w.lines[w.cursorLine]);
                                        break;
                                    case SDLK_RETURN:
                                    {
                                        int bytePos = charIndexToByte(w.lines[w.cursorLine], w.cursorCol);
                                        std::string newLine = w.lines[w.cursorLine].substr(bytePos);
                                        w.lines[w.cursorLine] = w.lines[w.cursorLine].substr(0, bytePos);
                                        w.lines.insert(w.lines.begin() + w.cursorLine + 1, newLine);
                                        w.cursorLine++;
                                        w.cursorCol = 0;
                                    }
                                    break;
                                    }
                                    safeCall(w.onChange, { Value(w.lines[w.cursorLine]) });
                                }
                            }
                        }
                        else if (e.type == SDL_MOUSEWHEEL) {
                            for (auto& [id, w] : g_widgets) {
                                if (w.type == "memo" && w.focused) {
                                    w.scrollY -= e.wheel.y;
                                    if (w.scrollY < 0) w.scrollY = 0;
                                    int maxScroll = (int)w.lines.size() - w.rect.h / 22;
                                    if (maxScroll < 0) maxScroll = 0;
                                    if (w.scrollY > maxScroll) w.scrollY = maxScroll;
                                }
                            }
                        }
                    }

                    // 悬停检测（用于按钮高亮）
                    for (auto& [id, w] : g_widgets) {
                        if (w.type == "button" && w.visible && w.enabled) {
                            if (mx >= w.rect.x && mx <= w.rect.x + w.rect.w &&
                                my >= w.rect.y && my <= w.rect.y + w.rect.h) {
                                g_hoveredWidgetId = id;
                                break;
                            }
                        }
                    }

                    // ================= 扁平化渲染 =================
                    SDL_SetRenderDrawColor(g_renderer, themeBg.r, themeBg.g, themeBg.b, themeBg.a);
                    SDL_RenderClear(g_renderer);

                    for (auto& [id, w] : g_widgets) {
                        if (w.type == "hlayout" || w.type == "vlayout") {
                            // 半透明背景
                            SDL_SetRenderDrawColor(g_renderer, 200, 200, 220, 80);
                            SDL_RenderFillRect(g_renderer, &w.rect);
                            SDL_SetRenderDrawColor(g_renderer, 150, 150, 180, 255);
                            // 虚线边框可省略，仅简单框线
                            SDL_RenderDrawRect(g_renderer, &w.rect);
                            continue; // 布局本身不渲染文字
                        }

                        if (!w.visible) continue;
                        int alpha = w.enabled ? 255 : 100;

                        if (w.type == "button") {
                            bool hover = (id == g_hoveredWidgetId);
                            SDL_Color bg = hover ? themePrimaryHover : themePrimary;
                            bg.a = alpha;
                            drawRoundedRect(g_renderer, w.rect, themeRadius, bg, true);
                            if (g_font && !w.text.empty()) {
                                SDL_Texture* tex = renderText(w.text, themeText);
                                if (tex) {
                                    int tw, th;
                                    SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
                                    SDL_Rect dst = { w.rect.x + (w.rect.w - tw) / 2, w.rect.y + (w.rect.h - th) / 2, tw, th };
                                    SDL_RenderCopy(g_renderer, tex, nullptr, &dst);
                                    SDL_DestroyTexture(tex);
                                }
                            }
                        }
                        else if (w.type == "label") {
                            if (g_font && !w.text.empty()) {
                                SDL_Color col = themeTextDark;
                                col.a = alpha;
                                std::istringstream stream(w.text);
                                std::string line;
                                int lineY = w.rect.y;
                                while (std::getline(stream, line)) {
                                    SDL_Texture* tex = renderText(line, col);
                                    if (tex) {
                                        int tw, th;
                                        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
                                        SDL_Rect dst = { w.rect.x, lineY, tw, th };
                                        SDL_RenderCopy(g_renderer, tex, nullptr, &dst);
                                        SDL_DestroyTexture(tex);
                                        lineY += th + 2;   // 行间距 2 像素
                                    }
                                }
                            }
                        }
                        else if (w.type == "textfield") {
                            SDL_Color bg = themeSurface;
                            bg.a = alpha;
                            drawRoundedRect(g_renderer, w.rect, themeRadius, bg, true);
                            drawRoundedRect(g_renderer, w.rect, themeRadius, themeBorder, false);

                            std::string displayText = w.text;
                            if (w.password) {
                                displayText.assign(utf8Length(w.text), '*');   // 字符数
                            }
                            else {
                                displayText = w.text;
                            }

                            if (g_font && !displayText.empty()) {
                                SDL_Color textCol = themeTextDark;
                                textCol.a = alpha;
                                SDL_Texture* tex = renderText(displayText, textCol);
                                if (tex) {
                                    int tw, th;
                                    SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
                                    SDL_Rect dst = { w.rect.x + 8, w.rect.y + (w.rect.h - th) / 2, tw, th };
                                    SDL_RenderCopy(g_renderer, tex, nullptr, &dst);
                                    SDL_DestroyTexture(tex);
                                }
                            }
                            // 光标（按实际文本计算）
                            if (w.focused && w.enabled) {
                                int cursorX = w.rect.x + 8;
                                if (g_font) {
                                    std::string before;
                                    if (w.password) {
                                        before = std::string(w.cursorCol, '*');   // 字符数
                                    }
                                    else {
                                        int bytePos = charIndexToByte(w.text, w.cursorCol);
                                        before = w.text.substr(0, bytePos);
                                    }
                                    int textW, textH;
                                    TTF_SizeUTF8(g_font, before.c_str(), &textW, &textH);
                                    cursorX += textW;
                                }
                                SDL_SetRenderDrawColor(g_renderer, themePrimary.r, themePrimary.g, themePrimary.b, 255);
                                SDL_RenderDrawLine(g_renderer, cursorX, w.rect.y + 4, cursorX, w.rect.y + w.rect.h - 4);
                            }
                        }
                        else if (w.type == "checkbox") {
                            SDL_Rect box = { w.rect.x, w.rect.y, 20, 20 };
                            SDL_Color boxBg = w.checked ? themePrimary : themeSurface;
                            boxBg.a = alpha;
                            drawRoundedRect(g_renderer, box, 4, boxBg, true);
                            drawRoundedRect(g_renderer, box, 4, themeBorder, false);
                            if (w.checked) {
                                SDL_SetRenderDrawColor(g_renderer, 255, 255, 255, 255);
                                SDL_RenderDrawLine(g_renderer, box.x + 4, box.y + 10, box.x + 8, box.y + 14);
                                SDL_RenderDrawLine(g_renderer, box.x + 8, box.y + 14, box.x + 15, box.y + 5);
                            }
                            if (g_font && !w.text.empty()) {
                                SDL_Texture* tex = renderText(w.text, themeTextDark);
                                if (tex) {
                                    int tw, th;
                                    SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
                                    SDL_Rect dst = { w.rect.x + 28, w.rect.y, tw, th };
                                    SDL_RenderCopy(g_renderer, tex, nullptr, &dst);
                                    SDL_DestroyTexture(tex);
                                }
                            }
                        }
                        else if (w.type == "slider") {
                            SDL_Rect track = { w.rect.x, w.rect.y + w.rect.h / 2 - 2, w.rect.w, 4 };
                            drawRoundedRect(g_renderer, track, 2, themeBorder, true);
                            double ratio = (w.value - w.min) / (w.max - w.min);
                            if (ratio < 0) ratio = 0; if (ratio > 1) ratio = 1;
                            int knobX = w.rect.x + (int)(ratio * w.rect.w);
                            int knobR = w.rect.h / 2;
                            SDL_Color knobColor = w.focused ? themePrimaryHover : themePrimary;
                            knobColor.a = alpha;
                            filledCircleRGBA(g_renderer, knobX, w.rect.y + w.rect.h / 2, knobR,
                                knobColor.r, knobColor.g, knobColor.b, knobColor.a);
                            aacircleRGBA(g_renderer, knobX, w.rect.y + w.rect.h / 2, knobR,
                                255, 255, 255, 200);
                            filledCircleRGBA(g_renderer, knobX, w.rect.y + w.rect.h / 2, knobR / 3,
                                255, 255, 255, 100);
                        }
                        else if (w.type == "progressbar") {
                            drawRoundedRect(g_renderer, w.rect, themeRadius, themeSurface, true);
                            drawRoundedRect(g_renderer, w.rect, themeRadius, themeBorder, false);
                            double ratio = (w.value - w.min) / (w.max - w.min);
                            if (ratio > 0.0) {
                                int fillW = (int)(w.rect.w * ratio);
                                if (fillW > 0) {   // 宽度必须大于 0
                                    SDL_Rect fill = { w.rect.x, w.rect.y, fillW, w.rect.h };
                                    drawRoundedRect(g_renderer, fill, themeRadius, themeSuccess, true);
                                }
                            }
                        }
                        else if (w.type == "memo") {
                            drawRoundedRect(g_renderer, w.rect, themeRadius, themeSurface, true);
                            drawRoundedRect(g_renderer, w.rect, themeRadius, themeBorder, false);
                            SDL_RenderSetClipRect(g_renderer, &w.rect);
                            int lineH = 22;
                            int yOffset = w.rect.y + 4;
                            for (int i = w.scrollY; i < (int)w.lines.size(); ++i) {
                                int drawY = yOffset + (i - w.scrollY) * lineH;
                                if (drawY + lineH > w.rect.y + w.rect.h) break;
                                if (g_font) {
                                    SDL_Texture* tex = renderText(w.lines[i], themeTextDark);
                                    if (tex) {
                                        int tw, th;
                                        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
                                        SDL_Rect dst = { w.rect.x + 8, drawY, tw, th };
                                        SDL_RenderCopy(g_renderer, tex, nullptr, &dst);
                                        SDL_DestroyTexture(tex);
                                    }
                                }
                            }
                            if (w.focused && w.enabled) {
                                int cursorY = yOffset + (w.cursorLine - w.scrollY) * lineH;
                                int cursorX = w.rect.x + 8;
                                if (g_font) {
                                    int bytePos = charIndexToByte(w.lines[w.cursorLine], w.cursorCol);
                                    std::string before = w.lines[w.cursorLine].substr(0, bytePos);
                                    int textW, textH;
                                    TTF_SizeUTF8(g_font, before.c_str(), &textW, &textH);
                                    cursorX += textW;
                                }
                                SDL_SetRenderDrawColor(g_renderer, themePrimary.r, themePrimary.g, themePrimary.b, 255);
                                SDL_RenderDrawLine(g_renderer, cursorX, cursorY, cursorX, cursorY + lineH);
                            }
                            SDL_RenderSetClipRect(g_renderer, nullptr);
                        }
                    }

                    SDL_RenderPresent(g_renderer);
                    SDL_Delay(16);
                }

                // 清理
                if (g_font) TTF_CloseFont(g_font);
                SDL_DestroyRenderer(g_renderer);
                SDL_DestroyWindow(g_window);
                TTF_Quit();
                SDL_Quit();
                return Value{};
                };
            mod->entries["run"] = Value(runFunc);

            // gui.quit() – 安全退出事件循环
            auto quitFunc = std::make_shared<NativeFunction>();
            quitFunc->func = [](Interpreter*, const std::vector<Value>&) -> Value {
                g_guiRunning = false;
                return Value{};
                };
            mod->entries["quit"] = Value(quitFunc);

            return Value(mod);
            };


        builtinModules["dialog"] = [this]() -> Value {
                auto mod = std::make_shared<MochiDict>();

                // info
                auto infoFunc = std::make_shared<NativeFunction>();
                infoFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                    std::string title = "Info", msg = "";
                    if (args.size() >= 1) title = std::any_cast<std::string>(args[0]);
                    if (args.size() >= 2) msg = std::any_cast<std::string>(args[1]);
                    dialog::info(title, msg);
                    return Value{};
                    };
                mod->entries["info"] = Value(infoFunc);

                // warning
                auto warnFunc = std::make_shared<NativeFunction>();
                warnFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                    std::string title = "Warning", msg = "";
                    if (args.size() >= 1) title = std::any_cast<std::string>(args[0]);
                    if (args.size() >= 2) msg = std::any_cast<std::string>(args[1]);
                    dialog::warning(title, msg);
                    return Value{};
                    };
                mod->entries["warning"] = Value(warnFunc);

                // error
                auto errorFunc = std::make_shared<NativeFunction>();
                errorFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                    std::string title = "Error", msg = "";
                    if (args.size() >= 1) title = std::any_cast<std::string>(args[0]);
                    if (args.size() >= 2) msg = std::any_cast<std::string>(args[1]);
                    dialog::error(title, msg);
                    return Value{};
                    };
                mod->entries["error"] = Value(errorFunc);

                // question
                auto questionFunc = std::make_shared<NativeFunction>();
                questionFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                    std::string title = "Question", msg = "";
                    if (args.size() >= 1) title = std::any_cast<std::string>(args[0]);
                    if (args.size() >= 2) msg = std::any_cast<std::string>(args[1]);
                    bool yes = dialog::question(title, msg);
                    return Value(yes);
                    };
                mod->entries["question"] = Value(questionFunc);

                // input
                auto inputFunc = std::make_shared<NativeFunction>();
                inputFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                    std::string title = "Input", msg = "", def = "";
                    if (args.size() >= 1) title = std::any_cast<std::string>(args[0]);
                    if (args.size() >= 2) msg = std::any_cast<std::string>(args[1]);
                    if (args.size() >= 3) def = std::any_cast<std::string>(args[2]);
                    std::string result = dialog::input(title, msg, def);
                    if (result.empty()) return Value{}; // 用户取消
                    return Value(result);
                    };
                mod->entries["input"] = Value(inputFunc);

                // openFile
                auto openFileFunc = std::make_shared<NativeFunction>();
                openFileFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                    std::string title = "Open File";
                    std::string filters = "*.*";
                    bool multi = false;
                    if (args.size() >= 1) title = std::any_cast<std::string>(args[0]);
                    if (args.size() >= 2) filters = std::any_cast<std::string>(args[1]);
                    if (args.size() >= 3) multi = std::any_cast<bool>(args[2]);

                    std::string res = dialog::openFile(title, filters, multi);
                    if (res.empty()) return Value{};
                    if (multi) {
                        auto arr = std::make_shared<MochiArray>();
                        std::istringstream iss(res);
                        std::string token;
                        while (std::getline(iss, token, '|')) {
                            arr->elements.push_back(Value(token));
                        }
                        return Value(arr);
                    }
                    else {
                        return Value(res);
                    }
                    };
                mod->entries["openFile"] = Value(openFileFunc);

                // saveFile
                auto saveFileFunc = std::make_shared<NativeFunction>();
                saveFileFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                    std::string title = "Save File";
                    std::string filters = "*.*";
                    if (args.size() >= 1) title = std::any_cast<std::string>(args[0]);
                    if (args.size() >= 2) filters = std::any_cast<std::string>(args[1]);
                    std::string res = dialog::saveFile(title, filters);
                    if (res.empty()) return Value{};
                    return Value(res);
                    };
                mod->entries["saveFile"] = Value(saveFileFunc);

                // selectFolder
                auto folderFunc = std::make_shared<NativeFunction>();
                folderFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                    std::string title = "Select Folder";
                    std::string defPath = "";
                    if (args.size() >= 1) title = std::any_cast<std::string>(args[0]);
                    if (args.size() >= 2) defPath = std::any_cast<std::string>(args[1]);
                    std::string res = dialog::selectFolder(title, defPath);
                    if (res.empty()) return Value{};
                    return Value(res);
                    };
                mod->entries["selectFolder"] = Value(folderFunc);

                return Value(mod);
                };

                // 在 registerBuiltinModules() 中
        builtinModules["socket"] = [this]() -> Value {
                socket_wrap::init();  // 确保 winsock 初始化
                auto mod = std::make_shared<MochiDict>();

                // 在 socket 模块 lambda 开始处
                static std::map<SocketHandle, Value> proxyCache;
                static std::mutex proxyMutex;

                auto getOrCreateProxy = [](SocketHandle s) -> Value {
                    std::lock_guard<std::mutex> lock(proxyMutex);
                    auto it = proxyCache.find(s);
                    if (it != proxyCache.end()) return it->second;
                    auto d = std::make_shared<MochiDict>();
                    d->entries["__handle__"] = Value(static_cast<double>(s));
                    Value val(d);
                    proxyCache[s] = val;
                    return val;
                    };

                auto removeProxy = [](SocketHandle s) {
                    std::lock_guard<std::mutex> lock(proxyMutex);
                    proxyCache.erase(s);
                    };

                // 辅助：从代理字典中提取 SocketHandle
                auto getHandle = [](const Value& proxy) -> SocketHandle {
                    auto dict = std::any_cast<std::shared_ptr<MochiDict>>(proxy);
                    return static_cast<SocketHandle>(std::any_cast<double>(dict->entries.at("__handle__")));
                    };

                // socket.tcp() -> 返回 TCP socket 代理
                auto tcpFunc = std::make_shared<NativeFunction>();
                tcpFunc->func = [getOrCreateProxy](Interpreter*, const std::vector<Value>&) -> Value {
                    SocketHandle s = socket_wrap::create(1); // SOCK_STREAM
                    return getOrCreateProxy(s);
                    };
                mod->entries["tcp"] = Value(tcpFunc);

                // socket.udp() -> 返回 UDP socket 代理
                auto udpFunc = std::make_shared<NativeFunction>();
                udpFunc->func = [getOrCreateProxy](Interpreter*, const std::vector<Value>&) -> Value {
                    SocketHandle s = socket_wrap::create(2); // SOCK_DGRAM
                    return getOrCreateProxy(s);
                    };
                mod->entries["udp"] = Value(udpFunc);

                // socket.bind(sock, addr, port)
                auto bindFunc = std::make_shared<NativeFunction>();
                bindFunc->func = [getHandle](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.size() < 3) throw std::runtime_error("socket.bind(sock, addr, port)");
                    auto s = getHandle(args[0]);
                    std::string addr = std::any_cast<std::string>(args[1]);
                    int port = static_cast<int>(std::any_cast<double>(args[2]));
                    socket_wrap::bind(s, addr, port);
                    return Value{};
                    };
                mod->entries["bind"] = Value(bindFunc);

                // socket.listen(sock, backlog)
                auto listenFunc = std::make_shared<NativeFunction>();
                listenFunc->func = [getHandle](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.size() < 2) throw std::runtime_error("socket.listen(sock, backlog)");
                    auto s = getHandle(args[0]);
                    int backlog = static_cast<int>(std::any_cast<double>(args[1]));
                    socket_wrap::listen(s, backlog);
                    return Value{};
                    };
                mod->entries["listen"] = Value(listenFunc);

                // socket.accept(sock) -> 返回新 socket 代理
                auto acceptFunc = std::make_shared<NativeFunction>();
                acceptFunc->func = [getHandle, getOrCreateProxy](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.empty()) throw std::runtime_error("socket.accept(sock)");
                    auto s = getHandle(args[0]);
                    SocketHandle client = socket_wrap::accept(s);
                    return getOrCreateProxy(client);
                    };
                mod->entries["accept"] = Value(acceptFunc);

                // socket.connect(sock, addr, port)
                auto connectFunc = std::make_shared<NativeFunction>();
                connectFunc->func = [getHandle](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.size() < 3) throw std::runtime_error("socket.connect(sock, addr, port)");
                    auto s = getHandle(args[0]);
                    std::string addr = std::any_cast<std::string>(args[1]);
                    int port = static_cast<int>(std::any_cast<double>(args[2]));
                    socket_wrap::connect(s, addr, port);
                    return Value{};
                    };
                mod->entries["connect"] = Value(connectFunc);

                // socket.send(sock, data) -> 返回发送字节数
                auto sendFunc = std::make_shared<NativeFunction>();
                sendFunc->func = [getHandle](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.size() < 2) throw std::runtime_error("socket.send(sock, data)");
                    auto s = getHandle(args[0]);
                    std::string data = std::any_cast<std::string>(args[1]);
                    int bytes = socket_wrap::send(s, data);
                    return Value(static_cast<double>(bytes));
                    };
                mod->entries["send"] = Value(sendFunc);

                // socket.recv(sock, maxSize) -> 返回接收的字符串
                auto recvFunc = std::make_shared<NativeFunction>();
                recvFunc->func = [getHandle](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.size() < 2) throw std::runtime_error("socket.recv(sock, maxSize)");
                    auto s = getHandle(args[0]);
                    int maxSize = static_cast<int>(std::any_cast<double>(args[1]));
                    std::string data = socket_wrap::recv(s, maxSize);
                    return Value(data);
                    };
                mod->entries["recv"] = Value(recvFunc);

                // socket.close(sock)
                auto closeFunc = std::make_shared<NativeFunction>();
                closeFunc->func = [getHandle, removeProxy](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.empty()) throw std::runtime_error("socket.close(sock)");
                    SocketHandle s = getHandle(args[0]);
                    socket_wrap::close(s);
                    removeProxy(s);   // 从缓存中移除，确保句柄可重用
                    return Value{};
                    };
                mod->entries["close"] = Value(closeFunc);

                // socket.handle(sock) 返回套接字句柄（数字）
                auto handleFunc = std::make_shared<NativeFunction>();
                handleFunc->func = [getHandle](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.empty()) throw std::runtime_error("socket.handle(sock)");
                    return Value(static_cast<double>(getHandle(args[0])));
                    };
                mod->entries["handle"] = Value(handleFunc);

                // socket.select(socks, timeout) -> 返回可读的 socket 列表
                auto selectFunc = std::make_shared<NativeFunction>();
                selectFunc->func = [getHandle, getOrCreateProxy](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.size() < 1) throw std::runtime_error("socket.select(sockList, timeout?)");
                    auto socks = std::any_cast<std::shared_ptr<MochiArray>>(args[0]);
                    double timeout = -1.0;
                    if (args.size() >= 2 && args[1].type() == typeid(double))
                        timeout = std::any_cast<double>(args[1]);
                    std::vector<SocketHandle> handles;
                    for (auto& v : socks->elements)
                        handles.push_back(getHandle(v));
                    auto readable = socket_wrap::selectRead(handles, timeout);
                    auto arr = std::make_shared<MochiArray>();
                    for (auto h : readable)
                        arr->elements.push_back(getOrCreateProxy(h));
                    return Value(arr);
                    };
                mod->entries["select"] = Value(selectFunc);

                return Value(mod);
                };


        builtinModules["pack"] = [this]() -> Value {
            auto mod = std::make_shared<MochiDict>();

            // 辅助：解析格式字符长度前缀，如 "4s" 返回 4 并前进
            auto parseCount = [](const std::string& fmt, size_t& i) -> int {
                int count = 0;
                while (i < fmt.size() && isdigit(fmt[i])) {
                    count = count * 10 + (fmt[i] - '0');
                    ++i;
                }
                return count ? count : 1;  // 无数字时默认为 1（除 s 外，s 必须带数字）
                };

            // ---------- pack.pack(fmt, args...) ----------
            auto packFunc = std::make_shared<NativeFunction>();
            packFunc->func = [parseCount](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("pack.pack(fmt, args...)");
                std::string fmt = std::any_cast<std::string>(args[0]);
                size_t argIdx = 1;          // 当前参数索引
                std::string result;         // 二进制结果

                // 字节序：默认大端
                char endian = '>';
                size_t pos = 0;
                if (pos < fmt.size()) {
                    char c = fmt[pos];
                    if (c == '@' || c == '=' || c == '<' || c == '>' || c == '!') {
                        endian = (c == '!') ? '>' : c;
                        ++pos;
                    }
                }

                auto packInt = [&](int bytes, bool isSigned, Value v) {
                    int64_t val = 0;
                    if (v.type() == typeid(double)) {
                        val = static_cast<int64_t>(std::any_cast<double>(v));
                    }
                    else if (v.type() == typeid(bool)) {
                        val = std::any_cast<bool>(v) ? 1 : 0;
                    }
                    else throw std::runtime_error("pack: expected number");

                    // 处理符号扩展？简单截断即可
                    uint8_t buf[8] = { 0 };
                    memcpy(buf, &val, bytes);   // 只复制低 bytes 字节
                    // 转序
                    if (endian == '<') {
                        for (int i = 0; i < bytes / 2; ++i) std::swap(buf[i], buf[bytes - 1 - i]);
                    }
                    result.append(reinterpret_cast<char*>(buf), bytes);
                    };

                while (pos < fmt.size()) {
                    char type = fmt[pos];
                    int count = 1;
                    if (type >= '0' && type <= '9') {
                        count = parseCount(fmt, pos);
                        type = fmt[pos];   // 下一个字符是格式符
                    }

                    switch (type) {
                    case 'b': packInt(1, true, args[argIdx++]); break;
                    case 'B': packInt(1, false, args[argIdx++]); break;
                    case 'h': packInt(2, true, args[argIdx++]); break;
                    case 'H': packInt(2, false, args[argIdx++]); break;
                    case 'i': packInt(4, true, args[argIdx++]); break;
                    case 'I': packInt(4, false, args[argIdx++]); break;
                    case 'q': packInt(8, true, args[argIdx++]); break;
                    case 'Q': packInt(8, false, args[argIdx++]); break;
                    case 'f': {
                        float f = static_cast<float>(std::any_cast<double>(args[argIdx++]));
                        uint8_t buf[4];
                        memcpy(buf, &f, 4);
                        if (endian == '<') std::swap(buf[0], buf[3]), std::swap(buf[1], buf[2]);
                        result.append(reinterpret_cast<char*>(buf), 4);
                        break;
                    }
                    case 'd': {
                        double d = std::any_cast<double>(args[argIdx++]);
                        uint8_t buf[8];
                        memcpy(buf, &d, 8);
                        if (endian == '<') for (int i = 0; i < 4; ++i) std::swap(buf[i], buf[7 - i]);
                        result.append(reinterpret_cast<char*>(buf), 8);
                        break;
                    }
                    case 's': {
                        if (count == 0) throw std::runtime_error("pack: 's' requires count");
                        std::string str = std::any_cast<std::string>(args[argIdx++]);
                        if (str.size() > count) str = str.substr(0, count);
                        else str.append(count - str.size(), '\0');
                        result += str;
                        break;
                    }
                    case 'c': {
                        std::string str = std::any_cast<std::string>(args[argIdx++]);
                        if (str.empty()) result += '\0';
                        else result += str[0];
                        break;
                    }
                    default: throw std::runtime_error("pack: unknown format char " + std::string(1, type));
                    }
                    ++pos;
                }
                return Value(result);
                };
            mod->entries["pack"] = Value(packFunc);

            // ---------- pack.unpack(fmt, data) ----------
            auto unpackFunc = std::make_shared<NativeFunction>();
            unpackFunc->func = [parseCount](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("pack.unpack(fmt, data)");
                std::string fmt = std::any_cast<std::string>(args[0]);
                std::string data = std::any_cast<std::string>(args[1]);
                size_t offset = 0;
                auto arr = std::make_shared<MochiArray>();

                char endian = '>';
                size_t pos = 0;
                if (pos < fmt.size()) {
                    char c = fmt[pos];
                    if (c == '@' || c == '=' || c == '<' || c == '>' || c == '!') {
                        endian = (c == '!') ? '>' : c;
                        ++pos;
                    }
                }

                auto unpackInt = [&](int bytes, bool isSigned) -> Value {
                    if (offset + bytes > data.size()) throw std::runtime_error("unpack: insufficient data");
                    uint8_t buf[8] = { 0 };
                    memcpy(buf, data.data() + offset, bytes);
                    offset += bytes;
                    if (endian == '<') for (int i = 0; i < bytes / 2; ++i) std::swap(buf[i], buf[bytes - 1 - i]);
                    int64_t val = 0;
                    memcpy(&val, buf, sizeof(val));   // 扩展到 int64_t
                    if (isSigned) return Value(static_cast<double>(val));
                    else return Value(static_cast<double>((uint64_t)val));
                    };

                while (pos < fmt.size()) {
                    char type = fmt[pos];
                    int count = 1;
                    if (type >= '0' && type <= '9') {
                        count = parseCount(fmt, pos);
                        type = fmt[pos];
                    }
                    switch (type) {
                    case 'b': arr->elements.push_back(unpackInt(1, true)); break;
                    case 'B': arr->elements.push_back(unpackInt(1, false)); break;
                    case 'h': arr->elements.push_back(unpackInt(2, true)); break;
                    case 'H': arr->elements.push_back(unpackInt(2, false)); break;
                    case 'i': arr->elements.push_back(unpackInt(4, true)); break;
                    case 'I': arr->elements.push_back(unpackInt(4, false)); break;
                    case 'q': arr->elements.push_back(unpackInt(8, true)); break;
                    case 'Q': arr->elements.push_back(unpackInt(8, false)); break;
                    case 'f': {
                        if (offset + 4 > data.size()) throw std::runtime_error("unpack: insufficient data");
                        uint8_t buf[4];
                        memcpy(buf, data.data() + offset, 4);
                        offset += 4;
                        if (endian == '<') std::swap(buf[0], buf[3]), std::swap(buf[1], buf[2]);
                        float f;
                        memcpy(&f, buf, 4);
                        arr->elements.push_back(Value(static_cast<double>(f)));
                        break;
                    }
                    case 'd': {
                        if (offset + 8 > data.size()) throw std::runtime_error("unpack: insufficient data");
                        uint8_t buf[8];
                        memcpy(buf, data.data() + offset, 8);
                        offset += 8;
                        if (endian == '<') for (int i = 0; i < 4; ++i) std::swap(buf[i], buf[7 - i]);
                        double d;
                        memcpy(&d, buf, 8);
                        arr->elements.push_back(Value(d));
                        break;
                    }
                    case 's': {
                        if (count == 0) throw std::runtime_error("unpack: 's' requires count");
                        if (offset + count > data.size()) throw std::runtime_error("unpack: insufficient data");
                        std::string str = data.substr(offset, count);
                        offset += count;
                        // 去除尾随零？保留原样
                        size_t endpos = str.find_last_not_of('\0');
                        if (endpos != std::string::npos) str = str.substr(0, endpos + 1);
                        arr->elements.push_back(Value(str));
                        break;
                    }
                    case 'c': {
                        if (offset + 1 > data.size()) throw std::runtime_error("unpack: insufficient data");
                        std::string one(1, data[offset++]);
                        arr->elements.push_back(Value(one));
                        break;
                    }
                    default: throw std::runtime_error("unpack: unknown format char " + std::string(1, type));
                    }
                    ++pos;
                }
                return Value(arr);
                };
            mod->entries["unpack"] = Value(unpackFunc);

            // ---------- 位运算辅助函数 ----------
            // pack.hex(str) 将十六进制字符串转为数字
            auto hexFunc = std::make_shared<NativeFunction>();
            hexFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("pack.hex(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                int64_t val = 0;
                try {
                    val = std::stoll(s, nullptr, 16);
                }
                catch (...) {
                    throw std::runtime_error("hex: invalid hex string");
                }
                return Value(static_cast<double>(val));
                };
            mod->entries["hex"] = Value(hexFunc);

            // pack.oct(str) 八进制
            auto octFunc = std::make_shared<NativeFunction>();
            octFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("pack.oct(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                int64_t val = 0;
                try {
                    val = std::stoll(s, nullptr, 8);
                }
                catch (...) {
                    throw std::runtime_error("oct: invalid octal string");
                }
                return Value(static_cast<double>(val));
                };
            mod->entries["oct"] = Value(octFunc);

            // pack.bin(str) 二进制
            auto binFunc = std::make_shared<NativeFunction>();
            binFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.empty()) throw std::runtime_error("pack.bin(str)");
                std::string s = std::any_cast<std::string>(args[0]);
                int64_t val = 0;
                try {
                    val = std::stoll(s, nullptr, 2);
                }
                catch (...) {
                    throw std::runtime_error("bin: invalid binary string");
                }
                return Value(static_cast<double>(val));
                };
            mod->entries["bin"] = Value(binFunc);

            auto bitandFunc = std::make_shared<NativeFunction>();
            bitandFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("bitand(a, b)");
                int64_t a = static_cast<int64_t>(std::any_cast<double>(args[0]));
                int64_t b = static_cast<int64_t>(std::any_cast<double>(args[1]));
                return Value(static_cast<double>(a & b));
                };
            mod->entries["bitand"] = Value(bitandFunc);

            auto bitorFunc = std::make_shared<NativeFunction>();
            bitorFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("bitor(a, b)");
                int64_t a = static_cast<int64_t>(std::any_cast<double>(args[0]));
                int64_t b = static_cast<int64_t>(std::any_cast<double>(args[1]));
                return Value(static_cast<double>(a | b));
                };
            mod->entries["bitor"] = Value(bitorFunc);

            auto bitnotFunc = std::make_shared<NativeFunction>();
            bitnotFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 1) throw std::runtime_error("bitnot(a)");
                int64_t a = static_cast<int64_t>(std::any_cast<double>(args[0]));
                return Value(static_cast<double>(~a));
                };
            mod->entries["bitnot"] = Value(bitnotFunc);

            auto bitxorFunc = std::make_shared<NativeFunction>();
            bitxorFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("bitxor(a, b)");
                int64_t a = static_cast<int64_t>(std::any_cast<double>(args[0]));
                int64_t b = static_cast<int64_t>(std::any_cast<double>(args[1]));
                return Value(static_cast<double>(a ^ b));
                };
            mod->entries["bitxor"] = Value(bitxorFunc);

            auto lshiftFunc = std::make_shared<NativeFunction>();
            lshiftFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("lshift(a, n)");
                int64_t a = static_cast<int64_t>(std::any_cast<double>(args[0]));
                int n = static_cast<int>(std::any_cast<double>(args[1]));
                return Value(static_cast<double>(a << n));
                };
            mod->entries["lshift"] = Value(lshiftFunc);

            auto rshiftFunc = std::make_shared<NativeFunction>();
            rshiftFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("rshift(a, n)");
                int64_t a = static_cast<int64_t>(std::any_cast<double>(args[0]));
                int n = static_cast<int>(std::any_cast<double>(args[1]));
                // 逻辑右移：将 a 转为无符号右移，再转回有符号保持位数
                uint64_t u = static_cast<uint64_t>(a);
                u >>= n;
                return Value(static_cast<double>(static_cast<int64_t>(u)));
                };
            mod->entries["rshift"] = Value(rshiftFunc);

            auto arshiftFunc = std::make_shared<NativeFunction>();
            arshiftFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
                if (args.size() < 2) throw std::runtime_error("arshift(a, n)");
                int64_t a = static_cast<int64_t>(std::any_cast<double>(args[0]));
                int n = static_cast<int>(std::any_cast<double>(args[1]));
                // 算术右移：有符号整数的右移（C++ 中是实现定义的，但通常为算术右移）
                int64_t result = a >> n;
                return Value(static_cast<double>(result));
                };
            mod->entries["arshift"] = Value(arshiftFunc);

            return Value(mod);
            };
   }

    void Interpreter::initBuiltins()
    {
        // 内置 print
        auto printNative = std::make_shared<NativeFunction>();
        printNative->name = "print";
        printNative->func = [](Interpreter* interpreter, const std::vector<Value>& args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i > 0) std::cout << " ";
                Value arg = args[i];
                // 如果是实例且定义了 __str__ 方法，调用它获取字符串
                if (arg.type() == typeid(std::shared_ptr<MochiInstance>)) {
                    auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(arg);
                    auto method = instance->klass->findMethod("__str__");
                    if (method) {
                        auto bound = interpreter->bindFunction(method, instance);
                        Value result = interpreter->callFunction(bound, {});
                        // 递归调用 stringify 将结果输出（可能也是实例，但通常 __str__ 返回字符串）
                        std::cout << stringify(result);
                        continue;
                    }
                }
                std::cout << stringify(arg);
            }
            std::cout << std::endl;
            return Value{};
            };
        globals->define("print", Value(printNative));

        // 内置 input
        auto inputFunc = std::make_shared<NativeFunction>();
        inputFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            if (!args.empty() && args[0].type() == typeid(std::string))
                std::cout << std::any_cast<std::string>(args[0]);
            std::string line;
            if (!std::getline(std::cin, line))
                return Value(std::string("")); // EOF
            return Value(line);
            };
        globals->define("input", Value(inputFunc));

        // 内置 import
        auto importNative = std::make_shared<NativeFunction>();
        importNative->name = "import";
        importNative->func = [this](Interpreter* interpreter, const std::vector<Value>& args) -> Value {
            if (args.size() != 1) throw std::runtime_error("import() expects 1 argument.");
            if (args[0].type() != typeid(std::string))
                throw std::runtime_error("import() argument must be a string.");
            std::string path = std::any_cast<std::string>(args[0]);
            return importModule(path);
            };
        globals->define("import", Value(importNative));

        // 内置 system
        auto systemNative = std::make_shared<NativeFunction>();
        systemNative->name = "system";
        systemNative->func = [this](Interpreter* interpreter, const std::vector<Value>& args) -> Value {
            if (args.size() != 1) throw std::runtime_error("system() expects 1 argument.");
            if (args[0].type() != typeid(std::string))
                throw std::runtime_error("system() argument must be a string.");
            std::string path = std::any_cast<std::string>(args[0]);
            return system(path.c_str());
            };
        globals->define("system", Value(systemNative));


        // 内置spawn函数
        auto spawnNative = std::make_shared<NativeFunction>();
        spawnNative->name = "spawn";
        spawnNative->func = [this](Interpreter* mainInterp, const std::vector<Value>& args) -> Value {
            if (args.size() != 1) throw std::runtime_error("spawn() expects 1 argument.");
            Value arg = args[0];

            // 为 Actor 创建独立解释器，共享全局环境
            auto actorInterp = std::make_shared<Interpreter>();
            actorInterp->setGlobals(mainInterp->getGlobals());

            if (arg.type() == typeid(std::shared_ptr<MochiFunction>)) {
                auto func = std::any_cast<std::shared_ptr<MochiFunction>>(arg);
                auto handler = [actorInterp, func](Value msg) {
                    actorInterp->callFunction(func, { msg });
                    };
                auto actor = std::make_shared<MochiActor>(handler);
                return Value(actor);
            }

            if (arg.type() == typeid(std::shared_ptr<MochiClass>)) {
                auto klass = std::any_cast<std::shared_ptr<MochiClass>>(arg);
                auto instance = std::make_shared<MochiInstance>(klass, this->globals);
                // 调用 init（在 actor 独立解释器环境）
                auto init = klass->findMethod("init");
                if (init) {
                    auto boundInit = actorInterp->bindFunction(init, instance);
                    actorInterp->callFunction(boundInit, {});
                }
                auto handler = [actorInterp, instance](Value msg) {
                    auto method = instance->klass->findMethod("receive");
                    if (method) {
                        auto bound = actorInterp->bindFunction(method, instance);
                        actorInterp->callFunction(bound, { msg });
                    }
                    };
                auto actor = std::make_shared<MochiActor>(handler);
                return Value(actor);
            }

            throw std::runtime_error("spawn() expects a function or class.");
            };
        globals->define("spawn", Value(spawnNative));

        // reply内置函数
        auto replyNative = std::make_shared<NativeFunction>();
        replyNative->name = "reply";
        replyNative->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.size() != 1) throw std::runtime_error("reply() expects 1 argument.");
            if (!currentAsk) throw std::runtime_error("reply() called outside an ask context.");
            currentAsk->future->setValue(args[0]);
            return Value{};
            };
        globals->define("reply", Value(replyNative));

        // sleep内置函数
        auto sleepNative = std::make_shared<NativeFunction>();
        sleepNative->name = "sleep";
        sleepNative->func = [](Interpreter* interpreter,
            const std::vector<Value>& args) -> Value {
                if (args.size() != 1 || args[0].type() != typeid(double))
                    throw std::runtime_error("sleep() expects a number (seconds).");
                double secs = std::any_cast<double>(args[0]);
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(secs * 1000)));
                return Value{};
            };
        globals->define("sleep", Value(sleepNative));

        // ---- 创建 FFI 库 ----
        ffiLib = std::make_shared<FFILibrary>(this);
        auto ffiModule = std::make_shared<MochiDict>();

        // ffi.call(name, args...)
        auto callFunc = std::make_shared<NativeFunction>();
        callFunc->func = [lib = ffiLib](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.empty()) throw std::runtime_error("ffi.call expects a function name.");
            std::string name = std::any_cast<std::string>(args[0]);
            std::vector<Value> callArgs(args.begin() + 1, args.end());
            return lib->call(name, callArgs);
            };
        ffiModule->entries["call"] = Value(callFunc);

        // ffi.export(name, func)
        auto exportFunc = std::make_shared<NativeFunction>();
        exportFunc->func = [lib = ffiLib](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.size() != 2) throw std::runtime_error("ffi.export expects name and function.");
            std::string name = std::any_cast<std::string>(args[0]);
            if (args[1].type() != typeid(std::shared_ptr<MochiFunction>))
                throw std::runtime_error("Second argument must be a function.");
            auto func = std::any_cast<std::shared_ptr<MochiFunction>>(args[1]);
            lib->exportFunc(name, func);
            return Value{};
            };
        ffiModule->entries["export"] = Value(exportFunc);

        globals->define("ffi", Value(ffiModule));

        // 其他内置函数
        // type(v) – 返回值的类型名称字符串
        auto typeFunc = std::make_shared<NativeFunction>();
        typeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string("nil"));
            const auto& v = args[0];
            if (!v.has_value())           return Value(std::string("nil"));
            if (v.type() == typeid(bool)) return Value(std::string("bool"));
            if (v.type() == typeid(double)) return Value(std::string("number"));
            if (v.type() == typeid(std::string)) return Value(std::string("string"));
            if (v.type() == typeid(std::shared_ptr<MochiFunction>)) return Value(std::string("function"));
            if (v.type() == typeid(std::shared_ptr<NativeFunction>)) return Value(std::string("native_function"));
            if (v.type() == typeid(std::shared_ptr<MochiClass>)) return Value(std::string("class"));
            if (v.type() == typeid(std::shared_ptr<MochiInstance>)) return Value(std::string("instance"));
            if (v.type() == typeid(std::shared_ptr<MochiArray>)) return Value(std::string("array"));
            if (v.type() == typeid(std::shared_ptr<MochiDict>)) return Value(std::string("dict"));
            if (v.type() == typeid(std::shared_ptr<MochiActor>)) return Value(std::string("actor"));
            if (v.type() == typeid(std::shared_ptr<MochiFuture>)) return Value(std::string("future"));
            return Value(std::string("unknown"));
            };
        globals->define("type", Value(typeFunc));

        // str(v) – 将值转换为字符串（使用现有的 stringify）
        auto strFunc = std::make_shared<NativeFunction>();
        strFunc->func = [](Interpreter* interpreter, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(std::string(""));
            Value arg = args[0];
            if (arg.type() == typeid(std::shared_ptr<MochiInstance>)) {
                auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(arg);
                auto method = instance->klass->findMethod("__str__");
                if (method) {
                    auto bound = interpreter->bindFunction(method, instance);
                    Value result = interpreter->callFunction(bound, {});
                    // 如果结果是字符串，直接返回；否则用 stringify 再转一次
                    if (result.type() == typeid(std::string))
                        return result;
                    else
                        return Value(stringify(result));
                }
            }
            return Value(stringify(arg));
            };
        globals->define("str", Value(strFunc));

        // num(v) – 将值转换为数字（字符串解析或直接提取）
        auto numFunc = std::make_shared<NativeFunction>();
        numFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(0.0);
            const auto& v = args[0];
            if (v.type() == typeid(double)) return v;
            if (v.type() == typeid(std::string)) {
                try {
                    double d = std::stod(std::any_cast<std::string>(v));
                    return Value(d);
                }
                catch (...) {
                    return Value(0.0); // 或抛出异常？返回 0 更温和
                }
            }
            if (v.type() == typeid(bool)) return Value(std::any_cast<bool>(v) ? 1.0 : 0.0);
            return Value(0.0);
            };
        globals->define("num", Value(numFunc));

        // len(v) – 返回数组、字典、字符串的长度，数字和布尔返回0，对象无定义报错
        auto lenFunc = std::make_shared<NativeFunction>();
        lenFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(0.0);
            const auto& v = args[0];
            if (v.type() == typeid(std::shared_ptr<MochiArray>))
                return Value(static_cast<double>(std::any_cast<std::shared_ptr<MochiArray>>(v)->elements.size()));
            if (v.type() == typeid(std::shared_ptr<MochiDict>))
                return Value(static_cast<double>(std::any_cast<std::shared_ptr<MochiDict>>(v)->entries.size()));
            if (v.type() == typeid(std::string))
                return Value(static_cast<double>(std::any_cast<std::string>(v).size()));
            if (v.type() == typeid(double) || v.type() == typeid(bool) || !v.has_value())
                return Value(0.0);
            throw std::runtime_error("len() not supported for this type.");
            };
        globals->define("len", Value(lenFunc));

        // error(msg) – 抛出运行时错误（用于脚本主动报错）
        auto errorFunc = std::make_shared<NativeFunction>();
        errorFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            std::string msg = "error";
            if (!args.empty()) msg = stringify(args[0]);
            throw std::runtime_error(msg);
            };
        globals->define("error", Value(errorFunc));

        // abs(x) – 绝对值
        auto absFunc = std::make_shared<NativeFunction>();
        absFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(0.0);
            double x = 0.0;
            if (args[0].type() == typeid(double)) x = std::any_cast<double>(args[0]);
            else if (args[0].type() == typeid(bool)) x = std::any_cast<bool>(args[0]) ? 1.0 : 0.0;
            // 字符串等其他类型忽略
            return Value(std::abs(x));
            };
        globals->define("abs", Value(absFunc));

        // min(a, b) / max(a, b)
        auto minFunc = std::make_shared<NativeFunction>();
        minFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) throw std::runtime_error("min() requires 2 arguments.");
            double a = 0, b = 0;
            if (args[0].type() == typeid(double)) a = std::any_cast<double>(args[0]);
            if (args[1].type() == typeid(double)) b = std::any_cast<double>(args[1]);
            return Value(a < b ? a : b);
            };
        globals->define("min", Value(minFunc));

        auto maxFunc = std::make_shared<NativeFunction>();
        maxFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) throw std::runtime_error("max() requires 2 arguments.");
            double a = 0, b = 0;
            if (args[0].type() == typeid(double)) a = std::any_cast<double>(args[0]);
            if (args[1].type() == typeid(double)) b = std::any_cast<double>(args[1]);
            return Value(a > b ? a : b);
            };
        globals->define("max", Value(maxFunc));

        // random() / random(max) – 返回 [0,1) 或 [0,max) 的随机浮点数
        auto randomFunc = std::make_shared<NativeFunction>();
        randomFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            static thread_local std::mt19937 gen(std::random_device{}());
            static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
            double r = dist(gen);
            if (!args.empty() && args[0].type() == typeid(double)) {
                double maxVal = std::any_cast<double>(args[0]);
                if (maxVal > 0) r *= maxVal;
            }
            return Value(r);
            };
        globals->define("random", Value(randomFunc));


        // 递归 clone 辅助函数（由于 lambda 不能递归，我们定义一个独立的函数）
        auto cloneFunc = std::make_shared<NativeFunction>();
        cloneFunc->func = [](Interpreter* interp, const std::vector<Value>& args) -> Value {
            if (args.empty()) throw std::runtime_error("clone() requires 1 argument.");
            return deepClone(args[0], interp);
            };
        globals->define("clone", Value(cloneFunc));

        // range(start, stop, step?)  内置函数
        auto rangeFunc = std::make_shared<NativeFunction>();
        rangeFunc->func = [](Interpreter*, const std::vector<Value>& args) -> Value {
            double start = 0, stop = 0, step = 1;
            if (args.empty()) throw std::runtime_error("range() expects at least 1 argument.");
            // 解析参数
            if (args.size() == 1) {
                if (args[0].type() != typeid(double)) throw std::runtime_error("range() expects numbers.");
                stop = std::any_cast<double>(args[0]);
            }
            else if (args.size() == 2) {
                if (args[0].type() != typeid(double) || args[1].type() != typeid(double))
                    throw std::runtime_error("range() expects numbers.");
                start = std::any_cast<double>(args[0]);
                stop = std::any_cast<double>(args[1]);
            }
            else { // >=3
                if (args[0].type() != typeid(double) || args[1].type() != typeid(double) ||
                    args[2].type() != typeid(double))
                    throw std::runtime_error("range() expects numbers.");
                start = std::any_cast<double>(args[0]);
                stop = std::any_cast<double>(args[1]);
                step = std::any_cast<double>(args[2]);
            }
            if (step == 0) throw std::runtime_error("range() step cannot be zero.");

            // 构造一个可迭代对象（字典），包含 iter 方法
            auto obj = std::make_shared<MochiDict>();
            auto iterFunc = std::make_shared<NativeFunction>();
            // iter() 方法：每次调用返回一个新的迭代器函数
            iterFunc->func = [start, stop, step](Interpreter*, const std::vector<Value>&) -> Value {
                auto current = std::make_shared<double>(start);
                auto iterator = std::make_shared<NativeFunction>();
                iterator->func = [current, stop, step](Interpreter*, const std::vector<Value>&) -> Value {
                    if ((step > 0 && *current >= stop) || (step < 0 && *current <= stop)) {
                        return Value{}; // nil 表示结束
                    }
                    double val = *current;
                    *current += step;
                    return Value(val);
                    };
                return Value(iterator);
                };
            obj->entries["iter"] = Value(iterFunc);
            return Value(obj);
            };
        globals->define("range", Value(rangeFunc));
    }

    void Interpreter::interpret(const std::vector<StmtPtr>& statements) {
        for (auto& stmt : statements) {
            execute(*stmt);
        }
    }

    Value Interpreter::importModule(const std::string& path) {

        // 检查内置模块
        if (builtinModules.count(path)) {
            auto it = modules.find(path);
            if (it != modules.end()) return it->second;
            auto mod = builtinModules[path]();   // 调用工厂函数
            modules[path] = mod;
            return mod;
        }

        // 加载自定义块
        auto it = modules.find(path);
        if (it != modules.end()) return it->second;

        std::ifstream file(path);
        if (file.is_open() && path.substr(path.size() - 6) == ".mochi")
        {
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string source = buffer.str();

            Lexer lexer(source);
            auto tokens = lexer.scanTokens();
            Parser parser(std::move(tokens));
            auto statements = parser.parse();

            auto moduleInterpreter = std::make_shared<Interpreter>();
            auto moduleEnv = std::make_shared<Environment>(globals);
            moduleInterpreter->globals = moduleEnv;
            moduleInterpreter->environment = moduleEnv;
            moduleInterpreter->interpret(statements);

            auto exports = std::make_shared<MochiDict>();
            auto localVars = moduleEnv->getLocalValues();
            for (auto& [name, value] : localVars) {
                exports->entries[name] = value;
            }
            modules[path] = Value(exports);
            return Value(exports);
        }

        // 3. 动态库模块加载
        // 构建可能的库路径列表
        std::vector<std::string> candidates;
        if (path.size() > 9 && path.substr(path.size() - 9) == ".mochimod") {
            candidates.push_back(path);      // 直接使用原路径
        }
        else {
            candidates.push_back(path + ".mochimod");
            candidates.push_back(path + ".so");  // Linux 惯用后缀
#ifdef _WIN32
            candidates.push_back(path + ".dll");
#endif
        }

        void* handle = nullptr;
        for (const auto& libPath : candidates) {
#ifdef _WIN32
            handle = LoadLibraryA(libPath.c_str());
#else
            handle = dlopen(libPath.c_str(), RTLD_NOW);
#endif
            if (handle) break;
        }

        if (!handle) {
            throw std::runtime_error("Cannot load module: " + path);
        }

        // 获取初始化函数
        using InitFunc = Value(*)(Interpreter*);
        InitFunc init = nullptr;
#ifdef _WIN32
        init = reinterpret_cast<InitFunc>(GetProcAddress((HMODULE)handle, "mochi_module_init"));
#else
        init = reinterpret_cast<InitFunc>(dlsym(handle, "mochi_module_init"));
#endif
        if (!init) {
#ifdef _WIN32
            FreeLibrary((HMODULE)handle);
#else
            dlclose(handle);
#endif
            throw std::runtime_error("Module does not export mochi_module_init: " + path);
        }

        // 调用初始化，获取模块字典
        Value modVal = init(this);
        if (modVal.type() != typeid(std::shared_ptr<MochiDict>)) {
#ifdef _WIN32
            FreeLibrary((HMODULE)handle);
#else
            dlclose(handle);
#endif
            throw std::runtime_error("mochi_module_init must return a dict.");
        }

        // 缓存并返回
        modules[path] = modVal;
        // 成功加载：记录句柄（保证句柄生命周期与解释器相同）
        if (std::find(loadedHandles.begin(), loadedHandles.end(), handle) == loadedHandles.end()) {
            loadedHandles.push_back(handle);
        }

        return modVal;
    }

    Value Interpreter::evaluate(const Expr& expr) {
        if (auto* e = dynamic_cast<const Literal*>(&expr)) return e->value;
        if (auto* e = dynamic_cast<const Variable*>(&expr)) return environment->get(e->name);
        if (auto* e = dynamic_cast<const Assign*>(&expr)) {
            Value value = evaluate(*e->value);
            environment->assign(e->name, value);
            return value;
        }
        if (auto* e = dynamic_cast<const Binary*>(&expr)) return evaluateBinary(*e);
        if (auto* e = dynamic_cast<const Unary*>(&expr)) return evaluateUnary(*e);
        if (auto* e = dynamic_cast<const Grouping*>(&expr)) return evaluate(*e->expression);
        if (auto* e = dynamic_cast<const Call*>(&expr)) return evaluateCall(*e);
        if (auto* e = dynamic_cast<const Get*>(&expr)) return evaluateGet(*e);
        if (auto* e = dynamic_cast<const Set*>(&expr)) return evaluateSet(*e);
        if (auto* e = dynamic_cast<const IndexGet*>(&expr)) {
            Value object = evaluate(*e->object);
            Value index = evaluate(*e->index);

            // 实例：调用 __getitem__
            if (object.type() == typeid(std::shared_ptr<MochiInstance>)) {
                auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(object);
                auto method = instance->klass->findMethod("__getitem__");
                if (method) {
                    auto bound = bindFunction(method, instance);
                    return callFunction(bound, { index });
                }
                throw std::runtime_error("Instance does not support indexing.");
            }

            // 字符串索引
            if (object.type() == typeid(std::string)) {
                auto str = std::any_cast<std::string>(object);
                if (index.type() != typeid(double))
                    throw std::runtime_error("String index must be a number.");
                int i = static_cast<int>(std::any_cast<double>(index));
                if (i < 0 || i >= static_cast<int>(str.size()))
                    throw std::runtime_error("String index out of bounds.");
                return Value(std::string(1, str[i]));
            }

            // 数字索引
            if (object.type() == typeid(std::shared_ptr<MochiArray>)) {
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(object);
                if (index.type() != typeid(double)) throw std::runtime_error("Array index must be a number.");
                int i = static_cast<int>(std::any_cast<double>(index));
                if (i < 0 || i >= arr->elements.size()) throw std::runtime_error("Array index out of bounds.");
                return arr->elements[i];
            }

            // 字符串索引
            if (object.type() == typeid(std::shared_ptr<MochiDict>)) {
                auto dict = std::any_cast<std::shared_ptr<MochiDict>>(object);
                std::string key;
                if (index.type() == typeid(std::string)) key = std::any_cast<std::string>(index);
                else throw std::runtime_error("Dictionary key must be a string.");
                auto it = dict->entries.find(key);
                if (it == dict->entries.end()) throw std::runtime_error("Key not found.");
                return it->second;
            }
            throw std::runtime_error("Index operation not supported.");
        }
        if (auto* e = dynamic_cast<const IndexSet*>(&expr)) {
            Value object = evaluate(*e->object);
            Value index = evaluate(*e->index);
            Value value = evaluate(*e->value);

            if (object.type() == typeid(std::shared_ptr<MochiInstance>)) {
                auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(object);
                auto method = instance->klass->findMethod("__setitem__");
                if (method) {
                    auto bound = bindFunction(method, instance);
                    return callFunction(bound, { index, value });
                }
                throw std::runtime_error("Instance does not support index assignment.");
            }

            if (object.type() == typeid(std::shared_ptr<MochiArray>)) {
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(object);
                if (index.type() != typeid(double)) throw std::runtime_error("Array index must be a number.");
                int i = static_cast<int>(std::any_cast<double>(index));
                if (i < 0 || i >= arr->elements.size()) throw std::runtime_error("Array index out of bounds.");
                arr->elements[i] = value;
                return value;
            }
            if (object.type() == typeid(std::shared_ptr<MochiDict>)) {
                auto dict = std::any_cast<std::shared_ptr<MochiDict>>(object);
                std::string key;
                if (index.type() == typeid(std::string)) key = std::any_cast<std::string>(index);
                else throw std::runtime_error("Dictionary key must be a string.");
                dict->entries[key] = value;
                return value;
            }
            throw std::runtime_error("Index assignment not supported.");
        }
        if (dynamic_cast<const This*>(&expr)) return environment->get("this");
        if (auto* e = dynamic_cast<const Super*>(&expr)) {
            auto superClassVal = environment->get("super");
            auto superClass = std::any_cast<std::shared_ptr<MochiClass>>(superClassVal);
            auto instanceVal = environment->get("this");
            auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(instanceVal);
            auto method = superClass->findMethod(e->method);
            if (!method) throw std::runtime_error("Undefined method '" + e->method + "'.");
            return Value(bindFunction(method, instance));
        }
        if (auto* e = dynamic_cast<const FunctionExpr*>(&expr)) {
            return Value(std::make_shared<MochiFunction>("lambda", e->params, e->body, environment));
        }
        if (auto* e = dynamic_cast<const ArrayLiteral*>(&expr)) {
            auto arr = std::make_shared<MochiArray>();
            for (auto& elem : e->elements) arr->elements.push_back(evaluate(*elem));
            return Value(arr);
        }
        if (auto* e = dynamic_cast<const DictLiteral*>(&expr)) {
            auto dict = std::make_shared<MochiDict>();
            for (auto& [k, v] : e->entries) dict->entries[k] = evaluate(*v);
            return Value(dict);
        }
        throw std::runtime_error("Unknown expression.");
    }

    Value Interpreter::evaluateBinary(const Binary& expr) {
        // 短路逻辑...
        if (expr.op.type == TokenType::AND) {
            Value left = evaluate(*expr.left);
            if (!isTruthy(left)) return left;
            return evaluate(*expr.right);
        }

        if (expr.op.type == TokenType::OR) {
            Value left = evaluate(*expr.left);
            if (isTruthy(left)) return left;
            return evaluate(*expr.right);
        }

        // --- 运算符重载：尝试左操作数是实例 ---
        Value left = evaluate(*expr.left);
        if (left.type() == typeid(std::shared_ptr<MochiInstance>)) {
            auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(left);
            std::string methodName;
            switch (expr.op.type) {
            case TokenType::PLUS:   methodName = "__add__"; break;
            case TokenType::MINUS:  methodName = "__sub__"; break;
            case TokenType::STAR:   methodName = "__mul__"; break;
            case TokenType::SLASH:  methodName = "__div__"; break;
            case TokenType::GREATER:        methodName = "__gt__"; break;
            case TokenType::GREATER_EQUAL:  methodName = "__ge__"; break;
            case TokenType::LESS:           methodName = "__lt__"; break;
            case TokenType::LESS_EQUAL:     methodName = "__le__"; break;
            case TokenType::EQUAL_EQUAL:    methodName = "__eq__"; break;
            case TokenType::BANG_EQUAL:     methodName = "__ne__"; break;
            case TokenType::MOD:            methodName = "__mod__"; break;
            default: break;
            }
            if (!methodName.empty()) {
                auto method = instance->klass->findMethod(methodName);
                if (method) {
                    Value right = evaluate(*expr.right);
                    auto bound = bindFunction(method, instance);
                    return callFunction(bound, { right });
                }
                // 如果没找到方法，对于某些运算符（如 +）还可以尝试右操作数（可交换）
                // 简单起见，直接回退到默认错误处理
            }
        }
        // 如果左操作数不是实例或没有对应方法，继续原有逻辑
        Value right = evaluate(*expr.right);
        switch (expr.op.type) {
        case TokenType::PLUS: {
            if (left.type() == typeid(double) && right.type() == typeid(double))
                return std::any_cast<double>(left) + std::any_cast<double>(right);
            if (left.type() == typeid(std::string) && right.type() == typeid(std::string))
                return std::any_cast<std::string>(left) + std::any_cast<std::string>(right);
            if (left.type() == typeid(std::string)) return std::any_cast<std::string>(left) + stringify(right);
            if (left.type() == typeid(std::shared_ptr<MochiArray>) && right.type() == typeid(std::shared_ptr<MochiArray>)) {
                auto leftArr = std::any_cast<std::shared_ptr<MochiArray>>(left);
                auto rightArr = std::any_cast<std::shared_ptr<MochiArray>>(right);
                auto result = std::make_shared<MochiArray>();
                result->elements.insert(result->elements.end(), leftArr->elements.begin(), leftArr->elements.end());
                result->elements.insert(result->elements.end(), rightArr->elements.begin(), rightArr->elements.end());
                return Value(result);
            }
            throw std::runtime_error("Invalid operands for +.");
        }
        case TokenType::MINUS: return std::any_cast<double>(left) - std::any_cast<double>(right);
        case TokenType::STAR: return std::any_cast<double>(left) * std::any_cast<double>(right);
        case TokenType::SLASH: return std::any_cast<double>(left) / std::any_cast<double>(right);
        case TokenType::GREATER: return std::any_cast<double>(left) > std::any_cast<double>(right);
        case TokenType::GREATER_EQUAL: return std::any_cast<double>(left) >= std::any_cast<double>(right);
        case TokenType::LESS: return std::any_cast<double>(left) < std::any_cast<double>(right);
        case TokenType::LESS_EQUAL: return std::any_cast<double>(left) <= std::any_cast<double>(right);
        case TokenType::EQUAL_EQUAL: return isEqual(left, right);
        case TokenType::BANG_EQUAL: return !isEqual(left, right);
        case TokenType::MOD: {
            double l = std::any_cast<double>(left);
            double r = std::any_cast<double>(right);
            return Value(std::fmod(l, r));   // 或者用 l - r * std::floor(l / r)
        }
        default: throw std::runtime_error("Unknown binary operator.");
        }
    }

    Value Interpreter::evaluateUnary(const Unary& expr) {
        Value right = evaluate(*expr.right);
        if (right.type() == typeid(std::shared_ptr<MochiInstance>)) {
            auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(right);
            std::string methodName;
            if (expr.op.type == TokenType::MINUS) methodName = "__neg__";
            else if (expr.op.type == TokenType::BANG) methodName = "__not__";
            if (!methodName.empty()) {
                auto method = instance->klass->findMethod(methodName);
                if (method) {
                    auto bound = bindFunction(method, instance);
                    return callFunction(bound, {});
                }
            }
            // 未找到方法则报错（不能对实例使用一元运算符）
            throw std::runtime_error("Unary operator not supported for this instance.");
        }

        if (expr.op.type == TokenType::BANG) return !isTruthy(right);
        if (expr.op.type == TokenType::MINUS) return -std::any_cast<double>(right);
        throw std::runtime_error("Unknown unary operator.");
    }

    Value Interpreter::evaluateCall(const Call& expr) {
        Value callee = evaluate(*expr.callee);
        std::vector<Value> arguments;
        for (auto& arg : expr.arguments) arguments.push_back(evaluate(*arg));

        if (callee.type() == typeid(std::shared_ptr<MochiFunction>)) {
            return callFunction(std::any_cast<std::shared_ptr<MochiFunction>>(callee), arguments);
        }
        if (callee.type() == typeid(std::shared_ptr<NativeFunction>)) {
            auto native = std::any_cast<std::shared_ptr<NativeFunction>>(callee);
            return native->func(this, arguments);
        }
        if (callee.type() == typeid(std::shared_ptr<MochiClass>)) {
            auto klass = std::any_cast<std::shared_ptr<MochiClass>>(callee);
            auto destroyMethod = klass->findMethod("destroy");
            std::shared_ptr<MochiInstance> instance;
            auto g = this->globals;

            if (destroyMethod) {
                instance = std::shared_ptr<MochiInstance>(
                    new MochiInstance(klass, g),
                    [destroyMethod, g](MochiInstance* ptr) {
                        Interpreter tempInterp(g);
                        auto nonOwning = std::shared_ptr<MochiInstance>(ptr, [](MochiInstance*) {});
                        auto boundDestroy = tempInterp.bindFunction(destroyMethod, nonOwning);
                        try {
                            tempInterp.callFunction(boundDestroy, {});
                        }
                        catch (const std::exception& e) {
                            std::cout << "Destroy error: " << e.what() << std::endl;
                        }
                        delete ptr;
                    });
            }
            else {
                instance = std::make_shared<MochiInstance>(klass, g);
            }

            auto init = klass->findMethod("init");
            if (init) {
                auto boundInit = bindFunction(init, instance);
                callFunction(boundInit, arguments);
            }
            return Value(instance);
        }
        throw std::runtime_error("Can only call functions and classes.");
    }

    Value Interpreter::evaluateGet(const Get& expr) {
        Value object = evaluate(*expr.object);

        // 字符串属性
        if (object.type() == typeid(std::string)) {
            auto str = std::any_cast<std::string>(object);
            if (expr.name == "size" || expr.name == "length") {
                return Value(static_cast<double>(str.size()));
            }
            throw std::runtime_error("String has no property '" + expr.name + "'.");
        }

        // 数组属性
        if (object.type() == typeid(std::shared_ptr<MochiArray>)) {
            auto arr = std::any_cast<std::shared_ptr<MochiArray>>(object);
            if (expr.name == "size") return Value(static_cast<double>(arr->elements.size()));
            if (expr.name == "push") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "push";
                func->func = [arr](Interpreter* interp, const std::vector<Value>& args) -> Value {
                    if (args.size() != 1) throw std::runtime_error("push() expects 1 argument.");
                    arr->elements.push_back(args[0]);
                    return Value{};
                    };
                return Value(func);
            }
            if (expr.name == "pop") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "pop";
                func->func = [arr](Interpreter* interp, const std::vector<Value>& args) -> Value {
                    if (!args.empty()) throw std::runtime_error("pop() expects no arguments.");
                    if (arr->elements.empty()) throw std::runtime_error("pop from empty array.");
                    Value back = arr->elements.back();
                    arr->elements.pop_back();
                    return back;
                    };
                return Value(func);
            }
            if (expr.name == "clone") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "clone";
                func->func = [arr](Interpreter* interp, const std::vector<Value>&) -> Value {
                    return deepClone(Value(arr), interp);
                    };
                return Value(func);
            }
            throw std::runtime_error("Array has no property '" + expr.name + "'.");
        }

        // 字典属性
        if (object.type() == typeid(std::shared_ptr<MochiDict>)) {
            auto dict = std::any_cast<std::shared_ptr<MochiDict>>(object);
            if (expr.name == "size") return Value(static_cast<double>(dict->entries.size()));
            // 删除以下 keys 分支（或注释掉）
            // if (expr.name == "keys") {
            //     auto keysArr = std::make_shared<MochiArray>();
            //     for (auto& [k, _] : dict->entries) keysArr->elements.push_back(Value(k));
            //     return Value(keysArr);
            // }
            // 通用键查找（用于模块导出的变量/函数）

            if (expr.name == "clone") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "clone";
                func->func = [dict](Interpreter* interp, const std::vector<Value>&) -> Value {
                    return deepClone(Value(dict), interp);
                    };
                return Value(func);
            }

            auto it = dict->entries.find(expr.name);
            if (it != dict->entries.end()) return it->second;
            throw std::runtime_error("Dictionary has no property '" + expr.name + "'.");
        }

        // MochiActor属性
        if (object.type() == typeid(std::shared_ptr<MochiActor>)) {
            auto actor = std::any_cast<std::shared_ptr<MochiActor>>(object);
            if (expr.name == "send") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "send";
                func->func = [actor](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.size() != 1) throw std::runtime_error("send() expects 1 argument.");
                    actor->send(args[0]);
                    return Value{};
                    };
                return Value(func);
            }
            if (expr.name == "stop") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "stop";
                func->func = [actor](Interpreter*, const std::vector<Value>& args) -> Value {
                    actor->stop();
                    return Value{};
                    };
                return Value(func);
            }
            if (expr.name == "close") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "close";
                func->func = [actor](Interpreter*, const std::vector<Value>& args) -> Value {
                    actor->close();
                    return Value{};
                    };
                return Value(func);
            }
            if (expr.name == "join") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "join";
                func->func = [actor](Interpreter*, const std::vector<Value>& args) -> Value {
                    actor->join();
                    return Value{};
                    };
                return Value(func);
            }
            if (expr.name == "ask") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "ask";
                func->func = [actor](Interpreter*, const std::vector<Value>& args) -> Value {
                    if (args.size() != 1) throw std::runtime_error("ask() expects 1 argument.");
                    auto future = actor->ask(args[0]);
                    return Value(future);
                    };
                return Value(func);
            }
            throw std::runtime_error("Actor has no property '" + expr.name + "'.");
        }

        // 为 MochiFuture 增加 get 属性
        if (object.type() == typeid(std::shared_ptr<MochiFuture>)) {
            auto future = std::any_cast<std::shared_ptr<MochiFuture>>(object);
            if (expr.name == "get") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "get";
                func->func = [future](Interpreter*, const std::vector<Value>& args) -> Value {
                    return future->get();
                    };
                return Value(func);
            }
            throw std::runtime_error("Future has no property '" + expr.name + "'.");
        }

        // 实例属性
        if (object.type() == typeid(std::shared_ptr<MochiInstance>)) {
            auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(object);
            auto it = instance->fields.find(expr.name);
            if (it != instance->fields.end()) {
                Value value = it->second;
                if (value.type() == typeid(std::shared_ptr<MochiFunction>))
                    return Value(bindFunction(std::any_cast<std::shared_ptr<MochiFunction>>(value), instance));
                return value;
            }
            if (expr.name == "clone") {
                auto func = std::make_shared<NativeFunction>();
                func->name = "clone";
                func->func = [instance](Interpreter* interp, const std::vector<Value>&) -> Value {
                    return deepClone(Value(instance), interp);
                    };
                return Value(func);
            }
            auto method = instance->klass->findMethod(expr.name);
            if (method) return Value(bindFunction(method, instance));
            throw std::runtime_error("Undefined property '" + expr.name + "'.");
        }

        throw std::runtime_error("Only instances, arrays and dicts have properties.");
    }

    Value Interpreter::evaluateSet(const Set& expr) {
        Value object = evaluate(*expr.object);
        if (object.type() == typeid(std::shared_ptr<MochiInstance>)) {
            auto instance = std::any_cast<std::shared_ptr<MochiInstance>>(object);
            Value value = evaluate(*expr.value);
            instance->fields[expr.name] = value;
            return value;
        }
        throw std::runtime_error("Only instances have fields.");
    }

    std::shared_ptr<MochiFunction> Interpreter::bindFunction(std::shared_ptr<MochiFunction> func,
        std::shared_ptr<MochiInstance> instance) {
        auto env = std::make_shared<Environment>(func->closure);
        env->define("this", Value(instance));
        return std::make_shared<MochiFunction>(func->name, func->params, func->body, env);
    }

    Value Interpreter::callFunction(std::shared_ptr<MochiFunction> func, const std::vector<Value>& arguments) {
        auto previousEnv = environment;
        auto env = std::make_shared<Environment>(func->closure);
        environment = env;
        for (size_t i = 0; i < func->params.size(); i++)
            env->define(func->params[i], i < arguments.size() ? arguments[i] : Value{});
        try {
            executeBlock(func->body, env);
        }
        catch (const ReturnException& ret) {
            environment = previousEnv;
            return ret.value;
        }
        environment = previousEnv;
        return Value{};
    }

    void Interpreter::executeBlock(const std::vector<StmtPtr>& statements, std::shared_ptr<Environment> env) {
        auto previousEnv = environment;
        environment = env;
        for (auto& stmt : statements) execute(*stmt);
        environment = previousEnv;
    }

    bool Interpreter::isEqual(const Value& a, const Value& b) {
        if (!a.has_value() && !b.has_value()) return true;
        if (!a.has_value() || !b.has_value()) return false;
        if (a.type() != b.type()) return false;
        if (a.type() == typeid(bool)) return std::any_cast<bool>(a) == std::any_cast<bool>(b);
        if (a.type() == typeid(double)) return std::any_cast<double>(a) == std::any_cast<double>(b);
        if (a.type() == typeid(std::string)) return std::any_cast<std::string>(a) == std::any_cast<std::string>(b);
        return false;
    }

    void Interpreter::execute(const Stmt& stmt) {
        if (auto* s = dynamic_cast<const ExpressionStmt*>(&stmt)) {
            evaluate(*s->expression);
        }
        else if (auto* s = dynamic_cast<const PrintStmt*>(&stmt)) {
            Value value = evaluate(*s->expression);
            std::cout << stringify(value) << std::endl;
        }
        else if (auto* s = dynamic_cast<const VarStmt*>(&stmt)) {
            Value value;
            if (s->initializer) value = evaluate(*s->initializer);
            environment->define(s->name, value);
        }
        else if (auto* s = dynamic_cast<const BlockStmt*>(&stmt)) {
            executeBlock(s->statements, std::make_shared<Environment>(environment));
        }
        else if (auto* s = dynamic_cast<const IfStmt*>(&stmt)) {
            if (isTruthy(evaluate(*s->condition))) execute(*s->thenBranch);
            else if (s->elseBranch) execute(*s->elseBranch);
        }
        else if (auto* s = dynamic_cast<const WhileStmt*>(&stmt)) {
            while (isTruthy(evaluate(*s->condition))) {
                try {
                    execute(*s->body);
                }
                catch (const BreakException&) {
                    break;
                }
                catch (const ContinueException&) {
                    // 直接继续下一次迭代
                }
            }
        }
        else if (dynamic_cast<const EmptyStmt*>(&stmt)) {
            // 空语句，不做任何事
        }
        else if (dynamic_cast<const BreakStmt*>(&stmt)) {
            throw BreakException();
        }
        else if (dynamic_cast<const ContinueStmt*>(&stmt)) {
            throw ContinueException();
        }
        else if (auto* s = dynamic_cast<const ForStmt*>(&stmt)) {
            auto previousEnv = environment;
            auto env = std::make_shared<Environment>(environment);
            environment = env;
            if (s->initializer) execute(*s->initializer);
            while (!s->condition || isTruthy(evaluate(*s->condition))) {
                try {
                    execute(*s->body);
                }
                catch (const BreakException&) {
                    break;
                }
                catch (const ContinueException&) {
                    // 执行 increment 然后继续循环
                }
                if (s->increment) evaluate(*s->increment);
            }
            environment = previousEnv;
        }
        else if (auto* s = dynamic_cast<const ReturnStmt*>(&stmt)) {
            Value value;
            if (s->value) value = evaluate(*s->value);
            throw ReturnException(value);
        }
        else if (auto* s = dynamic_cast<const FunctionStmt*>(&stmt)) {
            auto func = std::make_shared<MochiFunction>(s->name, s->params, s->body, environment);
            environment->define(s->name, Value(func));
        }
        else if (auto* s = dynamic_cast<const ClassStmt*>(&stmt)) {
            std::shared_ptr<MochiClass> superClass;
            if (!s->superClass.empty()) {
                auto superVal = environment->get(s->superClass);
                if (superVal.type() != typeid(std::shared_ptr<MochiClass>))
                    throw std::runtime_error("Superclass must be a class.");
                superClass = std::any_cast<std::shared_ptr<MochiClass>>(superVal);
            }
            auto previousEnv = environment;
            if (superClass) {
                environment = std::make_shared<Environment>(environment);
                environment->define("super", Value(superClass));
            }
            std::map<std::string, std::shared_ptr<MochiFunction>> methods;
            for (auto& m : s->methods) {
                auto func = std::make_shared<MochiFunction>(m.name, m.params, m.body, environment);
                methods[m.name] = func;
            }
            auto klass = std::make_shared<MochiClass>(s->name, superClass, methods);
            if (superClass) environment = previousEnv;
            environment->define(s->name, Value(klass));
        }
        else if (auto* s = dynamic_cast<const ForInStmt*>(&stmt)) {
            Value iterable = evaluate(*s->iterable);
            bool useProtocol = false;
            Value iterator;
            try {
                auto getIter = Get(std::make_unique<Literal>(iterable), "iter");
                Value iterMethod = evaluate(getIter);
                if (iterMethod.type() == typeid(std::shared_ptr<MochiFunction>)) {
                    auto func = std::any_cast<std::shared_ptr<MochiFunction>>(iterMethod);
                    iterator = callFunction(func, {});
                    useProtocol = true;
                }
                else if (iterMethod.type() == typeid(std::shared_ptr<NativeFunction>)) {
                    auto native = std::any_cast<std::shared_ptr<NativeFunction>>(iterMethod);
                    iterator = native->func(this, {});
                    useProtocol = true;
                }
            }
            catch (...) {}
            if (useProtocol) {
                while (true) {
                    Value result;
                    if (iterator.type() == typeid(std::shared_ptr<MochiFunction>)) {
                        auto func = std::any_cast<std::shared_ptr<MochiFunction>>(iterator);
                        result = callFunction(func, {});
                    }
                    else if (iterator.type() == typeid(std::shared_ptr<NativeFunction>)) {
                        auto native = std::any_cast<std::shared_ptr<NativeFunction>>(iterator);
                        result = native->func(this, {});
                    }
                    else {
                        auto nextGet = Get(std::make_unique<Literal>(iterator), "next");
                        Value nextMethod = evaluate(nextGet);
                        if (nextMethod.type() == typeid(std::shared_ptr<MochiFunction>))
                            result = callFunction(std::any_cast<std::shared_ptr<MochiFunction>>(nextMethod), {});
                        else if (nextMethod.type() == typeid(std::shared_ptr<NativeFunction>))
                            result = std::any_cast<std::shared_ptr<NativeFunction>>(nextMethod)->func(this, {});
                        else throw std::runtime_error("Iterator must be a function or have 'next' method.");
                    }
                    if (!result.has_value()) break;
                    auto loopEnv = std::make_shared<Environment>(environment);
                    environment = loopEnv;
                    environment->define(s->varName, result);
                    try { execute(*s->body); }
                    catch (const ReturnException&) { environment = environment->getEnclosing(); throw; }
                    environment = environment->getEnclosing();
                }
            }
            else if (iterable.type() == typeid(std::shared_ptr<MochiArray>)) {
                auto arr = std::any_cast<std::shared_ptr<MochiArray>>(iterable);
                for (auto& elem : arr->elements) {
                    auto loopEnv = std::make_shared<Environment>(environment);
                    environment = loopEnv;
                    environment->define(s->varName, elem);
                    try {
                        try {
                            execute(*s->body);
                        }
                        catch (const ContinueException&) {
                            // 直接进入下一次迭代
                        }
                    }
                    catch (const BreakException&) {
                        environment = environment->getEnclosing();
                        break;
                    }
                    environment = environment->getEnclosing();
                }
            }
            else if (iterable.type() == typeid(std::shared_ptr<MochiDict>)) {
                auto dict = std::any_cast<std::shared_ptr<MochiDict>>(iterable);
                for (auto& [k, v] : dict->entries) {
                    auto loopEnv = std::make_shared<Environment>(environment);
                    environment = loopEnv;
                    environment->define(s->varName, Value(k));
                    try { execute(*s->body); }
                    catch (const ReturnException&) { environment = environment->getEnclosing(); throw; }
                    environment = environment->getEnclosing();
                }
            }
            else {
                throw std::runtime_error("Object is not iterable.");
            }
        }
        else if (auto* s = dynamic_cast<const TryStmt*>(&stmt)) {
            try {
                execute(*s->tryBody);
            }
            catch (const ThrowException& te) {
                auto catchEnv = std::make_shared<Environment>(environment);
                environment = catchEnv;
                environment->define(s->catchVar, te.value);
                try { execute(*s->catchBody); }
                catch (...) { environment = environment->getEnclosing(); throw; }
                environment = environment->getEnclosing();
            }
            catch (const std::exception& e) {
                auto catchEnv = std::make_shared<Environment>(environment);
                environment = catchEnv;
                environment->define(s->catchVar, Value(std::string(e.what())));
                try { execute(*s->catchBody); }
                catch (...) { environment = environment->getEnclosing(); throw; }
                environment = environment->getEnclosing();
            }
        }
        else if (auto* s = dynamic_cast<const ThrowStmt*>(&stmt)) {
            Value value = evaluate(*s->value);
            throw ThrowException(value);
        }

    }
    }
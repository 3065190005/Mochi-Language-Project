#include "MochiUtils.h"

namespace Mochi
{
    bool isTruthy(const Value& v) {
        if (!v.has_value()) return false;
        if (v.type() == typeid(bool)) return std::any_cast<bool>(v);
        if (v.type() == typeid(double)) return std::any_cast<double>(v) != 0;
        return true;
    }


    std::string stringify(const Value& v) {
        if (!v.has_value()) return "nil";
        if (v.type() == typeid(bool)) return std::any_cast<bool>(v) ? "true" : "false";
        if (v.type() == typeid(double)) {
            std::ostringstream oss;
            oss << std::any_cast<double>(v);
            return oss.str();
        }
        if (v.type() == typeid(int)) {
            return std::to_string(std::any_cast<int>(v));
        }
        if (v.type() == typeid(std::string)) return std::any_cast<std::string>(v);
        if (v.type() == typeid(std::shared_ptr<MochiFunction>)) return "<fn>";
        if (v.type() == typeid(std::shared_ptr<NativeFunction>)) return "<native fn>";
        if (v.type() == typeid(std::shared_ptr<MochiClass>)) return "<class>";
        if (v.type() == typeid(std::shared_ptr<MochiInstance>)) return "<instance>";
        if (v.type() == typeid(std::shared_ptr<MochiArray>)) {
            auto arr = std::any_cast<std::shared_ptr<MochiArray>>(v);
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << stringify(arr->elements[i]);
            }
            oss << "]";
            return oss.str();
        }
        if (v.type() == typeid(std::shared_ptr<MochiDict>)) {
            auto dict = std::any_cast<std::shared_ptr<MochiDict>>(v);
            std::ostringstream oss;
            oss << "{";
            bool first = true;
            for (auto& [k, val] : dict->entries) {
                if (!first) oss << ", ";
                oss << k << ": " << stringify(val);
                first = false;
            }
            oss << "}";
            return oss.str();
        }
        return "<unknown>";
    }
}
#include "MochiEnv.h"

namespace Mochi
{
        void Environment::define(const std::string& name, Value value) { values[name] = std::move(value); }
        Value Environment::get(const std::string& name) {
            auto it = values.find(name);
            if (it != values.end()) return it->second;
            if (enclosing) return enclosing->get(name);
            throw std::runtime_error("Undefined variable '" + name + "'.");
        }
        void Environment::assign(const std::string& name, Value value) {
            auto it = values.find(name);
            if (it != values.end()) {
                it->second = std::move(value);
                return;
            }
            if (enclosing) {
                enclosing->assign(name, std::move(value));
                return;
            }
            throw std::runtime_error("Undefined variable '" + name + "'.");
        }
        std::shared_ptr<Environment> Environment::getEnclosing() const { return enclosing; }
        std::map<std::string, Value> Environment::getLocalValues() const { return values; }
}
#include "MochiInclude.h"
#include "MochiRuntime.h"

#ifndef MOCHIENV
#define MOCHIENV


namespace Mochi
{
    // ============================================================
    //  »·¾³
    // ============================================================
    class Environment {
        std::map<std::string, Value> values;
        std::shared_ptr<Environment> enclosing;
    public:
        explicit Environment(std::shared_ptr<Environment> e = nullptr) : enclosing(std::move(e)) {}
        void define(const std::string& name, Value value);
        Value get(const std::string& name);
        void assign(const std::string& name, Value value);
        std::shared_ptr<Environment> getEnclosing() const;
        std::map<std::string, Value> getLocalValues() const;
    };

}
#endif // !MOCHIENV
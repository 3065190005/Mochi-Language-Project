// dialog_wrapper.h
#pragma once
#include <string>
#include <vector>

namespace dialog {
    void info(const std::string& title, const std::string& msg);
    void warning(const std::string& title, const std::string& msg);
    void error(const std::string& title, const std::string& msg);
    bool question(const std::string& title, const std::string& msg);
    std::string input(const std::string& title, const std::string& msg, const std::string& def);
    std::string openFile(const std::string& title, const std::string& filters, bool multi);
    std::string saveFile(const std::string& title, const std::string& filters);
    std::string selectFolder(const std::string& title, const std::string& defPath);
}
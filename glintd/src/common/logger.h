#pragma once
#include <mutex>
#include <string>
#include <iostream>

class Logger {
public:
    static Logger& instance();

    void info(const std::string& msg);
    void error(const std::string& msg);
private:
    void log(const std::string &msg, const std::string &level);

    std::mutex mtx_;
};

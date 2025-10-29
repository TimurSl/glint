#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <iostream>

class Logger {
public:
    static Logger& instance();

    void info(const std::string& msg);

    void warn(const std::string &msg);

    void error(const std::string& msg);

    void to_file(const std::string& path);

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(const std::string &msg, const std::string &level);

    std::ofstream file_stream_;
    std::mutex mtx_;
};

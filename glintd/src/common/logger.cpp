#include "logger.h"

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::info(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::cout << "[INFO] " << msg << std::endl;
}

void Logger::error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::cerr << "[ERROR] " << msg << std::endl;
}

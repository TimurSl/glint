#include "logger.h"

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::log(const std::string &msg, const std::string &level) {
    std::lock_guard<std::mutex> lock(mtx_);
        std::cout << "[" << level << "] " << msg << std::endl;
}

void Logger::info(const std::string& msg) {
    log(msg, "INFO");
}

void Logger::error(const std::string& msg) {
    log(msg, "ERROR");
}

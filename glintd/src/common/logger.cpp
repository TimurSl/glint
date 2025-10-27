#include "logger.h"

#include <filesystem>
#include <iomanip>

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::log(const std::string& msg, const std::string& level) {
    std::lock_guard<std::mutex> lock(mtx_);
    std::string line = "[" + level + "] " + msg;
    std::cout << line << std::endl;
    if (file_stream_.is_open())
        file_stream_ << line << std::endl;
}


void Logger::info(const std::string& msg) {
    log(msg, "INFO");
}

void Logger::error(const std::string& msg) {
    log(msg, "ERROR");
}

void Logger::to_file(const std::string& base_path) {
    std::lock_guard<std::mutex> lock(mtx_);

    std::filesystem::path base_dir = std::filesystem::path(base_path).parent_path();
    if (base_dir.empty()) base_dir = ".";
    std::filesystem::path logs_dir = base_dir / "logs";
    std::error_code ec;
    std::filesystem::create_directories(logs_dir, ec);
    if (ec) {
        std::cerr << "[ERROR] Failed to create logs directory: " << ec.message() << std::endl;
    }

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S");
    std::string dated_name = std::filesystem::path(base_path).stem().string()
                           + "_" + oss.str() + ".log";

    auto full_path = logs_dir / dated_name;

    file_stream_.open(full_path, std::ios::app);
    if (!file_stream_.is_open()) {
        std::cerr << "[ERROR] Cannot open log file: " << full_path << std::endl;
    } else {
        std::cout << "[INFO] Logging to file: " << full_path << std::endl;
    }
}

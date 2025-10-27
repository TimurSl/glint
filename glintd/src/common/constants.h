#pragma once
#include <string>

namespace glintd::consts {
    inline constexpr const char* VERSION = "0.1.0-dev";
    inline constexpr const char* LOG_FILE = "glintd.log";

    // Название пайпа/сокета по умолчанию
#ifdef _WIN32
    inline constexpr const char* DEFAULT_PIPE_PATH = R"(\\.\pipe\glintd)";
#else
    inline std::string default_socket_path() {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        if (xdg) return std::string(xdg) + "/glintd.sock";
        return "/run/user/" + std::to_string(getuid()) + "/glintd.sock";
    }
#endif

    // Файлы экспорта
    inline constexpr const char* EXPORT_LAST_CLIP = "export_last_clip.txt";
    inline constexpr const char* EXPORT_MANUAL    = "export_manual.txt";

} // namespace glintd::consts

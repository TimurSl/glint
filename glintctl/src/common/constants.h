#pragma once
#include <string>

namespace glintctl::consts {
#ifdef _WIN32
    inline constexpr const char* DEFAULT_PIPE_PATH = R"(\\.\pipe\glintd)";
    inline constexpr int PIPE_TIMEOUT_MS = 2000;
#else
    inline std::string default_socket_path();
    inline constexpr int SOCKET_TIMEOUT_SEC = 5;
#endif
}

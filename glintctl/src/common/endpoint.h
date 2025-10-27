#include "common/constants.h"
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace glintctl {

#ifdef _WIN32
    std::string default_endpoint() {
        return consts::DEFAULT_PIPE_PATH;
    }
#else
    std::string default_endpoint() {
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        if (xdg && *xdg)
            return std::string(xdg) + "/glintd.sock";
        return "/run/user/" + std::to_string(getuid()) + "/glintd.sock";
    }
#endif

}

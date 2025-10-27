#pragma once
#include <string>

namespace glintctl {
    bool send_recv(const std::string& endpoint, const std::string& req, std::string& resp);
}

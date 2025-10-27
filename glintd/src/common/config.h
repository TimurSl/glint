#pragma once
#include <string>

struct Config {
    int target_fps = 60;
    double pre_seconds = 20.0;
    double post_seconds = 10.0;
    std::string ipc_mode = "stdin";
};

Config load_default_config();

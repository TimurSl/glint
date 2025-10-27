#include "handlers.h"
#include <nlohmann/json.hpp>
#include <iostream>

using nlohmann::json;

namespace glintd::rpc {

    std::string handle_command(const std::string& line) {
        try {
            std::string clean = line;
            while (!clean.empty() && (clean.back() == '\n' || clean.back() == '\r' || clean.back() == '\0'))
                clean.pop_back();

            auto cmd = json::parse(clean);
            std::string name = cmd.value("cmd", "");
            json resp;

            if (name == "status") {
                resp = {{"ok", true}, {"msg", "daemon alive"}};
            }
            else if (name == "start") {
                resp = {{"ok", true}, {"msg", "recording started"}};
            }
            else if (name == "stop") {
                resp = {{"ok", true}, {"msg", "recording stopped"}};
            }
            else if (name == "marker") {
                int pre = cmd.value("pre", 0);
                int post = cmd.value("post", 0);
                resp = {{"ok", true}, {"msg", "marker created"}, {"pre", pre}, {"post", post}};
            }
            else if (name == "export") {
                std::string mode = cmd.value("mode", "last");
                resp = {{"ok", true}, {"msg", "export done"}, {"mode", mode}};
            }
            else {
                resp = {{"ok", false}, {"error", "unknown command"}};
            }

            return resp.dump() + "\n";
        }
        catch (std::exception& e) {
            std::cerr << "[ERROR] handle_command: " << e.what() << std::endl;
            return std::string("{\"ok\":false,\"error\":\"exception: ") + e.what() + "\"}\n";
        }
    }

} // namespace glintd::rpc

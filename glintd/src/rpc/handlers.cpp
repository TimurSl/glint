#include "handlers.h"
#include "common/logger.h"
#include "common/constants.h"
#include <nlohmann/json.hpp>
#include <fstream>

using nlohmann::json;

namespace glintd::rpc {

    std::string handle_command(const std::string& line) {
        auto& log = Logger::instance();

        try {
            std::string clean = line;
            while (!clean.empty() && (clean.back() == '\n' || clean.back() == '\r' || clean.back() == '\0'))
                clean.pop_back();

            auto cmd = json::parse(clean);
            std::string name = cmd.value("cmd", "");
            json resp;

            log.info(std::string("RPC command: ") + name);

            if (name == "status") {
                resp = {{"ok", true}, {"msg", "daemon alive"}};
            }
            else if (name == "start") {
                log.info("Starting recording...");
                resp = {{"ok", true}, {"msg", "recording started"}};
            }
            else if (name == "stop") {
                log.info("Stopping recording...");
                resp = {{"ok", true}, {"msg", "recording stopped"}};
            }
            else if (name == "marker") {
                int pre = cmd.value("pre", 0);
                int post = cmd.value("post", 0);
                log.info("Creating marker: pre=" + std::to_string(pre) + " post=" + std::to_string(post));
                resp = {{"ok", true}, {"msg", "marker created"}, {"pre", pre}, {"post", post}};
            }
            else if (name == "export") {
                std::string mode = cmd.value("mode", "last");
                log.info("Export requested, mode=" + mode);

                // пример: создать фиктивный файл экспорта
                std::ofstream out("export_" + mode + "_clip.txt");
                out << "Fake exported clip data";
                out.close();

                resp = {{"ok", true}, {"msg", "export done"}, {"mode", mode}};
            }
            else if (name == "version") {
                resp = {
                    {"ok", true},
                    {"version", consts::VERSION},
                    {"msg", "glint daemon version info"}
                };
            }
            else if (name == "quit") {
                log.info("Quit requested by client");
                resp = {{"ok", true}, {"msg", "shutting down"}};
                // Возврат с пометкой, чтобы сервер мог завершиться
                return resp.dump() + "\nQUIT\n";
            }
            else {
                resp = {{"ok", false}, {"error", "unknown command"}};
            }

            return resp.dump() + "\n";
        }
        catch (std::exception& e) {
            log.error(std::string("handle_command exception: ") + e.what());
            return std::string("{\"ok\":false,\"error\":\"exception: ") + e.what() + "\"}\n";
        }
    }

} // namespace glintd::rpc

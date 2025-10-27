#include "handlers.h"
#include "common/logger.h"
#include "common/constants.h"
#include <nlohmann/json.hpp>
#include <fstream>

#include "db.h"
#include "sqlite3.h"

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
                return resp.dump() + "\nQUIT\n";
            }
            else if (name == "list_sessions") {
                log.info("Listing last 50 sessions");
                sqlite3* db = DB::instance().handle();
                sqlite3_stmt* stmt;
                sqlite3_prepare_v2(db,
                    "SELECT id, game, started_at, stopped_at, container, output_mp4 FROM sessions ORDER BY id DESC LIMIT 50",
                    -1, &stmt, nullptr);

                nlohmann::json arr = nlohmann::json::array();
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    nlohmann::json obj;
                    obj["id"] = sqlite3_column_int(stmt, 0);
                    obj["game"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    obj["started_at"] = sqlite3_column_int(stmt, 2);
                    obj["stopped_at"] = sqlite3_column_int(stmt, 3);
                    obj["container"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                    obj["output_mp4"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                    arr.push_back(obj);
                }
                sqlite3_finalize(stmt);

                nlohmann::json result;
                result["ok"] = true;
                result["sessions"] = arr;
                return result.dump();
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

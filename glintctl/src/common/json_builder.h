#include "common/json_builder.h"

namespace glintctl {

    std::string build_json(const std::vector<std::string>& args) {
        if (args.empty()) return {};
        const std::string& cmd = args[0];

        auto flag = [&](const std::string& n)->const char* {
            for (size_t i = 1; i + 1 < args.size(); ++i)
                if (args[i] == n) return args[i + 1].c_str();
            return nullptr;
        };

        if (cmd == "status") return R"({"cmd":"status"})";
        if (cmd == "start")  return R"({"cmd":"start"})";
        if (cmd == "stop")   return R"({"cmd":"stop"})";
        if (cmd == "marker") {
            const char *pre = flag("--pre"), *post = flag("--post");
            if (!pre || !post) return {};
            return std::string(R"({"cmd":"marker","pre":)") + pre + R"(,"post":)" + post + "}";
        }
        if (cmd == "export") {
            const char *mode = flag("--mode");
            if (!mode) return {};
            return std::string(R"({"cmd":"export","mode":")") + mode + "\"}";
        }
        if (cmd == "raw") {
            const char *js = flag("--json");
            if (!js) return {};
            return std::string(js);
        }
        return {};
    }

}

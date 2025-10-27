// glintctl/main.cpp
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/types.h>
#endif

static std::string join(const std::vector<std::string> &v, const char *sep) {
    std::string out;
    bool first = true;
    for (auto &s: v) {
        if (!first) out += sep;
        first = false;
        out += s;
    }
    return out;
}

static void usage(const char *prog) {
    std::cerr <<
            "Usage:\n"
            "  " << prog << " [--socket <path_or_pipe>] <command> [args]\n"
            "\n"
            "Commands:\n"
            "  status\n"
            "  start\n"
            "  stop\n"
            "  marker --pre <sec> --post <sec>\n"
            "  export --mode <last|all>\n"
            "  raw --json '{\"cmd\":\"...\"}'\n"
            "\n"
            "Defaults:\n"
            "  Windows pipe: \\\\.\\pipe\\glintd\n"
                        "  Linux  socket: $XDG_RUNTIME_DIR/glintd.sock or /run/user/$UID/glintd.sock\n";
}

#ifdef _WIN32
static std::string default_endpoint() { return R"(\\.\pipe\glintd)"; }
#else
static std::string getenv_str(const char *k) {
    const char *v = std::getenv(k);
    return v ? std::string(v) : std::string();
}
static std::string default_endpoint() {
    std::string xdg = getenv_str("XDG_RUNTIME_DIR");
    if (!xdg.empty()) return xdg + "/glintd.sock";
    return "/run/user/" + std::to_string((unsigned) getuid()) + "/glintd.sock";
}
#endif

static std::string build_json(const std::vector<std::string> &args) {
    if (args.empty()) return {};
    const std::string &cmd = args[0];
    auto flag = [&](const std::string &n)-> const char * {
        for (size_t i = 1; i + 1 < args.size(); ++i) if (args[i] == n) return args[i + 1].c_str();
        return nullptr;
    };

    if (cmd == "status") return R"({"cmd":"status"})";
    if (cmd == "start") return R"({"cmd":"start"})";
    if (cmd == "stop") return R"({"cmd":"stop"})";
    if (cmd == "quit") return R"({"cmd":"quit"})";
    if (cmd == "list_sessions") return R"({"cmd":"list_sessions"})";
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

#ifdef _WIN32
static bool send_recv_win(const std::string &pipe, const std::string &line, std::string &out) {
    HANDLE h = CreateFileA(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (!WaitNamedPipeA(pipe.c_str(), 2000)) {
            std::cerr << "WaitNamedPipe failed " << GetLastError() << "\n";
            return false;
        }
        h = CreateFileA(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                        NULL);
        if (h == INVALID_HANDLE_VALUE) {
            std::cerr << "CreateFile(pipe) failed " << GetLastError() << "\n";
            return false;
        }
    }
    std::string payload = line;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    DWORD wr = 0;
    if (!WriteFile(h, payload.data(), (DWORD) payload.size(), &wr, NULL)) {
        std::cerr << "WriteFile " << GetLastError() << "\n";
        CloseHandle(h);
        return false;
    }
    out.clear();
    char buf[4096];
    while (true) {
        DWORD rd = 0;
        if (!ReadFile(h, buf, sizeof(buf), &rd, NULL)) {
            DWORD ec = GetLastError();
            if (ec == ERROR_BROKEN_PIPE) break;
            std::cerr << "ReadFile " << ec << "\n";
            CloseHandle(h);
            return false;
        }
        if (rd == 0) break;
        out.append(buf, buf + rd);
        if (out.find('\n') != std::string::npos) break;
    }
    if (auto p = out.find('\n'); p != std::string::npos) out.resize(p);
    CloseHandle(h);
    return true;
}
#else
static bool send_recv_unix(const std::string &path, const std::string &line, std::string &out) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "Socket path too long\n";
        close(fd);
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return false;
    }
    std::string payload = line;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    if (send(fd, payload.data(), payload.size(), 0) < 0) {
        perror("send");
        close(fd);
        return false;
    }
    out.clear();
    char buf[4096];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            perror("recv");
            close(fd);
            return false;
        }
        if (n == 0) break;
        out.append(buf, buf + n);
        if (out.find('\n') != std::string::npos) break;
    }
    if (auto p = out.find('\n'); p != std::string::npos) out.resize(p);
    close(fd);
    return true;
}
#endif

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    std::string endpoint;
    for (size_t i = 0; i < args.size();) {
        if (args[i] == "--socket" && i + 1 < args.size()) {
            endpoint = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
        } else if (args[i] == "-h" || args[i] == "--help") {
            usage(argv[0]);
            return 0;
        } else { ++i; }
    }
    if (args.empty()) {
        usage(argv[0]);
        return 2;
    }

    std::string json = build_json(args);
    if (json.empty()) {
        std::cerr << "Bad args\n";
        usage(argv[0]);
        return 2;
    }

    if (endpoint.empty()) endpoint = default_endpoint();

    std::string resp;
#ifdef _WIN32
    bool ok = send_recv_win(endpoint, json, resp);
#else
    bool ok = send_recv_unix(endpoint, json, resp);
#endif
    if (!ok) return 1;
    std::cout << resp << std::endl;
    return 0;
}

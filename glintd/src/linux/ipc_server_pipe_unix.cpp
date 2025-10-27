#include "../common/ipc_server_pipe.h"
#include "../common/logger.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <cstring>
#include <vector>

static bool readLineFD(int fd, std::string& out) {
    out.clear();
    std::string acc;
    char buf[256];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) return false;
        acc.append(buf, buf + n);
        auto p = acc.find('\n');
        if (p != std::string::npos) {
            out.assign(acc.data(), p);
            return true;
        }
    }
}

static bool writeLineFD(int fd, const std::string& s) {
    std::string line = s;
    line.push_back('\n');
    const char* data = line.data();
    size_t left = line.size();
    while (left) {
        ssize_t n = ::write(fd, data, left);
        if (n <= 0) return false;
        data += n;
        left -= n;
    }
    return true;
}

bool IpcServerPipe::start(IpcHandler handler) {
    if (running_) return true;
    running_ = true;
    worker_ = std::thread(&IpcServerPipe::run, this, handler);
    return true;
}

void IpcServerPipe::stop() {
    if (!running_) return;
    running_ = false;

    if (worker_.joinable()) worker_.join();
}

void IpcServerPipe::run(IpcHandler handler) {
    auto& log = Logger::instance();
    const std::string path = endpoint_.empty() ?
        ("/run/user/" + std::to_string(getuid()) + "/glintd.sock") : endpoint_;

    ::unlink(path.c_str());
    int sfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { log.error("socket() failed"); return; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());

    if (::bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        log.error("bind() failed"); ::close(sfd); return;
    }
    ::chmod(path.c_str(), 0600);

    if (::listen(sfd, 1) < 0) {
        log.error("listen() failed"); ::close(sfd); return;
    }
    log.info(std::string("IPC: listening on ") + path);

    while (running_) {
        int cfd = ::accept(sfd, nullptr, nullptr);
        if (cfd < 0) { if (!running_) break; continue;}
        log.info("IPC: client connected");

        while (running_) {
            std::string req;
            if (!readLineFD(cfd, req)) break;
            std::string rsp;
            try { rsp = handler(req); }
            catch (...) { rsp = "{\"ok\":false,\"error\":\"exception\"}"; }
            if (!writeLineFD(cfd, rsp)) break;
        }

        ::close(cfd);
        log.info("IPC: client disconnected");
    }

    ::close(sfd);
    ::unlink(path.c_str());
    log.info("IPC: server stopped");
}

#include "../common/ipc_server_pipe.h"
#include "../common/logger.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <string>

static bool readLine(HANDLE h, std::string& out) {
    out.clear();
    std::string acc;
    char buf[256];
    DWORD read = 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof(buf), &read, nullptr)) {
            return false;
        }
        if (read == 0) return false;
                acc.append(buf, read);
        auto pos = acc.find('\n');
        if (pos != std::string::npos) {
            out.assign(acc.data(), pos);

            return true;
        }
    }
}

static bool writeLine(HANDLE h, const std::string& s) {
    std::string line = s;
    line.push_back('\n');
    DWORD written = 0;
    return WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr) != 0;
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

    std::wstring endpointW(endpoint_.begin(), endpoint_.end());
    std::wstring pipeName;
    if (endpoint_.rfind(R"(\\.\pipe\)", 0) == 0) {
        pipeName.assign(endpoint_.begin(), endpoint_.end());
    } else {
        std::wstring endpointW(endpoint_.begin(), endpoint_.end());
        pipeName = L"\\\\.\\pipe\\" + endpointW;
    }

    while (running_) {
        HANDLE hPipe = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096,
            0,
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            log.error("CreateNamedPipe failed: " + std::to_string(GetLastError()));
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        log.info("IPC: waiting client...");

        BOOL ok = ConnectNamedPipe(hPipe, nullptr) ? TRUE :
                  (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!ok) {
            CloseHandle(hPipe);
            if (!running_) break;
            continue;
        }

        log.info("IPC: client connected");

        while (running_) {
            std::string req;
            if (!readLine(hPipe, req))
                break;

            std::string rsp;
            try {
                rsp = handler(req);
            } catch (...) {
                rsp = "{\"ok\":false,\"error\":\"exception\"}";
            }

            if (!writeLine(hPipe, rsp))
                break;
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        log.info("IPC: client disconnected");
    }

    log.info("IPC: server stopped");
}

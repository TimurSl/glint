#include "common/transport.h"
#include "common/constants.h"
#include <windows.h>
#include <iostream>

namespace glintctl {

    bool send_recv(const std::string& pipe, const std::string& req, std::string& out) {
        HANDLE h = CreateFileA(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            if (!WaitNamedPipeA(pipe.c_str(), consts::PIPE_TIMEOUT_MS)) {
                std::cerr << "WaitNamedPipe failed " << GetLastError() << "\n";
                return false;
            }
            h = CreateFileA(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                std::cerr << "CreateFile failed " << GetLastError() << "\n";
                return false;
            }
        }

        std::string payload = req;
        if (payload.empty() || payload.back() != '\n') payload.push_back('\n');

        DWORD written = 0;
        if (!WriteFile(h, payload.data(), (DWORD)payload.size(), &written, NULL)) {
            std::cerr << "WriteFile failed " << GetLastError() << "\n";
            CloseHandle(h);
            return false;
        }

        out.clear();
        char buf[4096];
        while (true) {
            DWORD rd = 0;
            if (!ReadFile(h, buf, sizeof(buf), &rd, NULL)) break;
            if (rd == 0) break;
            out.append(buf, buf + rd);
            if (out.find('\n') != std::string::npos) break;
        }

        if (auto p = out.find('\n'); p != std::string::npos)
            out.resize(p);

        CloseHandle(h);
        return true;
    }

}

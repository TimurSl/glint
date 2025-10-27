#include "common/endpoint.h"
#include "common/json_builder.h"
#include "common/transport.h"
#include <iostream>
#include <vector>

static void usage(const char* prog) {
    std::cout <<
        "Usage:\n"
        "  " << prog << " [--socket <path_or_pipe>] <command> [args]\n"
        "  Commands: status, start, stop, marker, export, raw\n";
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage(argv[0]);
        return 2;
    }

    std::string endpoint;
    for (size_t i = 0; i + 1 < args.size();) {
        if (args[i] == "--socket") {
            endpoint = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
        } else ++i;
    }

    if (endpoint.empty()) endpoint = glintctl::default_endpoint();

    std::string json = glintctl::build_json(args);
    if (json.empty()) {
        std::cerr << "Bad args\n";
        usage(argv[0]);
        return 2;
    }

    std::string resp;
    if (!glintctl::send_recv(endpoint, json, resp)) return 1;
    std::cout << resp << std::endl;
    return 0;
}

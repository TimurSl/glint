#include "common/logger.h"
#include "common/config.h"
#include "common/capture_base.h"
#include "common/replay_buffer.h"
#include "common/marker_manager.h"
#include "common/ipc_server_stdin.h"
#include "common/detector.h"
#include "common/ipc_server_pipe.h"

#include <chrono>
#include <thread>
#include <sstream>

#include "constants.h"
#include "rpc/handlers.h"

#if defined(GLINT_WINDOWS)
extern "C" CaptureBase* create_capture();
#elif defined(GLINT_LINUX)
extern "C" CaptureBase* create_capture();
#endif

int main(int argc, char** argv) {
    auto& log = Logger::instance();
    Config cfg = load_default_config();

    std::string socket_path;
    bool force_reset = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc)
            socket_path = argv[++i];
        else if (arg == "--reset")
            force_reset = true;
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: glintd [--socket <path>] [--reset]\n";
            return 0;
        }
    }

#ifdef _WIN32
    if (socket_path.empty())
        socket_path = glintd::consts::DEFAULT_PIPE_PATH;
#else
    if (socket_path.empty())
        socket_path = glintd::consts::default_socket_path();
#endif

    Logger::instance().info("Using socket: " + socket_path);
    log.info("Glint Daemon starting...");

    std::unique_ptr<CaptureBase> capture(create_capture());
    ReplayBuffer replay;
    MarkerManager markers;
    Detector detector;
    IpcServerPipe ipc(socket_path);

    if (!capture->init()) {
        log.error("Capture init failed");
        return 1;
    }


    detector.start(
        [&](const std::string& game){
            replay.start_session(game);
            capture->start();
        },
        [&]{
            capture->stop();
            replay.stop_session();

            replay.export_last_clip(glintd::consts::EXPORT_LAST_CLIP);
        }
    );


    auto handler = [&](const std::string& line) -> std::string {
        return glintd::rpc::handle_command(line);
    };

    ipc.start(handler);

    log.info("Glint Daemon started with PID " + std::to_string(::getpid()));
    log.info("IPC server is running on " + socket_path);

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ipc.stop();
    detector.stop();
    return 0;
}

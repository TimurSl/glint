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

#include "rpc/handlers.h"

#if defined(GLINT_WINDOWS)
extern "C" CaptureBase* create_capture();
#elif defined(GLINT_LINUX)
extern "C" CaptureBase* create_capture();
#endif

int main() {
    auto& log = Logger::instance();
    Config cfg = load_default_config();

    std::unique_ptr<CaptureBase> capture(create_capture());
    ReplayBuffer replay;
    MarkerManager markers;
    Detector detector;
#if defined(GLINT_WINDOWS)
    IpcServerPipe ipc("\\\\.\\pipe\\glintd");
#else
    IpcServerPipe ipc("/run/user/" + std::to_string(getuid()) + "/glintd.sock");
#endif

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

            replay.export_last_clip("export_last_clip.txt");
        }
    );


    auto handler = [&](const std::string& line) -> std::string {
        return glintd::rpc::handle_command(line);
    };

    ipc.start(handler);


    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ipc.stop();
    detector.stop();
    return 0;
}

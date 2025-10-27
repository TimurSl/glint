#include "common/logger.h"
#include "common/config.h"
#include "common/capture_base.h"
#include "common/replay_buffer.h"
#include "common/marker_manager.h"
#include "common/ipc_server_stdin.h"
#include "common/detector.h"

#include <chrono>
#include <thread>
#include <sstream>

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
    StdinIpcServer ipc;

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

    auto handler = [&](const std::string& line)->std::string {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "status") {
            return replay.is_running() ? "status: recording" : "status: idle";
        }
        if (cmd == "start") {
            capture->start();
            replay.start_session("Manual");
            return "ok";
        }
        if (cmd == "stop") {
            capture->stop();
            replay.stop_session();
            return "ok";
        }
        if (cmd == "marker") {
            double pre = cfg.pre_seconds, post = cfg.post_seconds;
            iss >> pre >> post;
            Marker m{ static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()
            ), pre, post };
            markers.add(m);
            return "marker: ok";
        }
        if (cmd == "export") {
            replay.export_last_clip("export_manual.txt");
            return "export: ok";
        }
        return "unknown command";
    };

    ipc.start(handler);


    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ipc.stop();
    detector.stop();
    return 0;
}

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
#include "db.h"
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

    log.to_file(glintd::consts::LOG_FILE);
    log.info("Logging to file: " + std::string(glintd::consts::LOG_FILE));
    log.info("Glint Daemon starting...");

    DB::instance().open();
    std::unique_ptr<CaptureBase> capture(create_capture());

    RecorderConfig recorderCfg;
    recorderCfg.width = cfg.video.width == 0 ? recorderCfg.width : cfg.video.width;
    recorderCfg.height = cfg.video.height == 0 ? recorderCfg.height : cfg.video.height;
    recorderCfg.fps = cfg.video.fps;
    recorderCfg.video_bitrate_kbps = cfg.video.bitrate_kbps;
    recorderCfg.video_codec = cfg.video.use_nvenc ? "h264_nvenc" : cfg.video.codec;
    recorderCfg.audio_sample_rate = cfg.audio.sample_rate;
    recorderCfg.audio_channels = cfg.audio.channels;
    recorderCfg.audio_bitrate_kbps = cfg.audio.bitrate_kbps;
    recorderCfg.audio_codec = cfg.audio.codec;
    recorderCfg.rolling_directory = cfg.buffer.dir;
    recorderCfg.recordings_directory = cfg.record.out_dir;
    recorderCfg.segment_length = std::chrono::milliseconds(cfg.buffer.segment_ms);
    recorderCfg.retention = std::chrono::milliseconds(cfg.buffer.retention.max_total_ms);
    recorderCfg.max_total_size_bytes = cfg.buffer.retention.max_total_bytes;

    capture->setRecorderConfig(recorderCfg);

    ReplayBuffer replay;
    replay.setRollingBufferEnabled(cfg.buffer.enabled);

    MarkerManager markers;
    Detector detector;
    IpcServerPipe ipc(socket_path);

    CaptureRuntimeOptions runtimeOpts;
    runtimeOpts.rolling_buffer_enabled = cfg.buffer.enabled;
    capture->applyRuntimeOptions(runtimeOpts);

    if (!capture->init()) {
        log.error("Capture init failed");
        return 1;
    }


    replay.attachRecorder(&capture->recorder());

    detector.start(
        [&](const std::string& game){
            replay.start_session(game);
            capture->start();
        },
        [&]{
            capture->stop();
            replay.stop_session();
            const std::filesystem::path export_last_clip_path = glintd::consts::EXPORT_LAST_CLIP;
            replay.export_last_clip(export_last_clip_path);
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
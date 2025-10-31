#include "common/logger.h"
#include "common/config.h"
#include "common/capture_base.h"
#include "common/replay_buffer.h"
#include "common/marker_manager.h"
#include "common/ipc_server_stdin.h"
#include "common/detector.h"
#include "common/ipc_server_pipe.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <format>
#include <filesystem>
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

namespace {
std::string select_video_codec(const VideoSettings& video) {
    std::string codec = video.codec;
    std::string encoder = video.encoder;
    std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    std::transform(encoder.begin(), encoder.end(), encoder.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (encoder == "nvenc" || encoder == "vaapi") {
        return codec + "_" + encoder;
    }
    return codec;
}
}

int main(int argc, char** argv) {
    auto& log = Logger::instance();
    const std::filesystem::path configPath{"glintd/config.toml"};
    AppConfig appConfig = load_config(configPath);

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

    if (appConfig.general.file_logging) {
        auto logDir = appConfig.general.log_path.parent_path();
        if (!logDir.empty()) {
            std::filesystem::create_directories(logDir);
        }
        log.to_file(appConfig.general.log_path.string());
    }
    log.info("Logging to file: " + appConfig.general.log_path.string());
    log.info("Glint Daemon starting...");

    DB::instance().setCustomPath(appConfig.general.db_path);
    if (auto dbOpen = DB::instance().open(); !dbOpen) {
        log.error(std::format("Failed to open database: {}", dbOpen.error()));
        return 1;
    }
    std::unique_ptr<CaptureBase> capture(create_capture());
    ReplayBuffer replay;

    auto applyConfig = [&](const AppConfig& cfg) {
        const ProfileConfig& profile = cfg.activeProfile();
        RecorderConfig recorderCfg;
        recorderCfg.width = profile.video.width;
        recorderCfg.height = profile.video.height;
        recorderCfg.fps = profile.video.fps;
        recorderCfg.video_bitrate_kbps = profile.video.bitrate_kbps;
        recorderCfg.video_codec = select_video_codec(profile.video);
        recorderCfg.audio_sample_rate = profile.audio.sample_rate;
        recorderCfg.audio_channels = profile.audio.channels;
        recorderCfg.audio_bitrate_kbps = profile.audio.bitrate_kbps;
        recorderCfg.audio_codec = profile.audio.codec;
        recorderCfg.enable_system_audio = profile.audio.enable_system;
        recorderCfg.enable_microphone_audio = profile.audio.enable_microphone;
        recorderCfg.buffer_directory = profile.buffer.segment_directory;
        recorderCfg.recordings_directory = profile.buffer.output_directory;
        recorderCfg.segment_prefix = profile.buffer.segment_prefix;
        recorderCfg.segment_extension = profile.buffer.segment_extension;
        recorderCfg.container = profile.buffer.container;
        recorderCfg.rolling_size_limit_bytes = profile.buffer.size_limit_bytes;

        capture->setRecorderConfig(recorderCfg);

        ReplayBuffer::Options bufferOptions;
        bufferOptions.buffer_enabled = profile.buffer.enabled;
        bufferOptions.rolling_mode = profile.buffer.rolling_mode;
        bufferOptions.rolling_size_limit_bytes = profile.buffer.size_limit_bytes;
        bufferOptions.segment_root = profile.buffer.segment_directory;
        bufferOptions.output_directory = profile.buffer.output_directory;
        bufferOptions.temp_directory = cfg.general.temp_path;
        bufferOptions.container = profile.buffer.container;
        bufferOptions.segment_prefix = profile.buffer.segment_prefix;
        bufferOptions.segment_extension = profile.buffer.segment_extension;
        replay.applyOptions(bufferOptions);

        CaptureRuntimeOptions runtimeOpts;
        runtimeOpts.rolling_buffer_enabled = profile.buffer.rolling_mode;
        capture->applyRuntimeOptions(runtimeOpts);
    };

    applyConfig(appConfig);

    MarkerManager markers;
    Detector detector;
    IpcServerPipe ipc(socket_path);

    CaptureRuntimeOptions runtimeOpts;
    runtimeOpts.rolling_buffer_enabled = appConfig.activeProfile().buffer.rolling_mode;
    capture->applyRuntimeOptions(runtimeOpts);

    if (!capture->init()) {
        log.error("Capture init failed");
        return 1;
    }

    replay.attachRecorder(&capture->recorder());

    ConfigHotReloader reloader(configPath, appConfig, applyConfig);
    reloader.start();

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
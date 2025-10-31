#pragma once
#include <cstdint>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavdevice/avdevice.h>
}
#include <string>
#include <stdexcept>

inline std::string ff_errstr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

struct FfInitOnce {
    FfInitOnce() {
        av_log_set_level(AV_LOG_WARNING);

        // Эти функции удалены в FFmpeg 5+, не вызываем их:
        // av_register_all();
        // avcodec_register_all();

        // Но avdevice_register_all() все еще нужна, чтобы активировать устройства (dshow, wasapi, etc.)
#if LIBAVDEVICE_VERSION_INT >= AV_VERSION_INT(59, 0, 0)
        avdevice_register_all();
#endif
    }
};

inline void ff_init() {
    static FfInitOnce once;
}

struct TimeBase {
    AVRational tb{1, 1000}; // миллисекунды по умолчанию
};

inline int64_t now_us() {
    return av_gettime_relative();
}

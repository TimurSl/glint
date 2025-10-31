#include "buffer_merger.h"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

#include "logger.h"

BufferMerger::BufferMerger(std::filesystem::path tempDirectory)
    : temp_directory_(std::move(tempDirectory)) {}

namespace {
std::string escapePath(const std::filesystem::path& path) {
    std::string raw = path.string();
    std::string escaped;
    escaped.reserve(raw.size());
    for (char ch : raw) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped += ch;
        }
    }
    return escaped;
}
}

bool BufferMerger::writeConcatFile(const std::vector<SegmentInfo>& segments, const std::filesystem::path& listPath) const {
    std::ofstream list(listPath, std::ios::trunc);
    if (!list.is_open()) {
        Logger::instance().error(std::format("BufferMerger: failed to open concat list {}", listPath.string()));
        return false;
    }
    for (const auto& seg : segments) {
        auto absPath = std::filesystem::absolute(seg.path);
        list << "file '" << escapePath(absPath) << "'\n";
    }
    return true;
}

bool BufferMerger::merge(int sessionId, const std::vector<SegmentInfo>& segments, const std::filesystem::path& outputPath) const {
    if (segments.empty()) {
        Logger::instance().warn("BufferMerger: no segments to merge for session " + std::to_string(sessionId));
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(temp_directory_, ec);
    if (ec) {
        Logger::instance().error(std::format("BufferMerger: failed to create temp directory: {}", ec.message()));
        return false;
    }

    auto listPath = temp_directory_ / ("session_" + std::to_string(sessionId) + "_concat.txt");
    if (!writeConcatFile(segments, listPath)) {
        return false;
    }

    std::filesystem::create_directories(outputPath.parent_path(), ec);
    if (ec) {
        Logger::instance().error(std::format("BufferMerger: failed to create output directory: {}", ec.message()));
        std::filesystem::remove(listPath);
        return false;
    }

    std::ostringstream cmd;
    cmd << "ffmpeg -y -f concat -safe 0 -i "
        << '"' << listPath.string() << '"'
        << " -map 0 -c copy "
        << '"' << outputPath.string() << '"';

    Logger::instance().info(std::format("BufferMerger: executing {}", cmd.str()));
    int result = std::system(cmd.str().c_str());
    std::filesystem::remove(listPath, ec);
    if (result != 0) {
        Logger::instance().error(std::format("BufferMerger: ffmpeg returned code {}", result));
        return false;
    }
    return true;
}
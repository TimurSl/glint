#pragma once

#include <filesystem>
#include <vector>

#include "recorder.h"

class BufferMerger {
public:
    explicit BufferMerger(std::filesystem::path tempDirectory);

    bool merge(int sessionId, const std::vector<SegmentInfo>& segments, const std::filesystem::path& outputPath) const;

private:
    bool writeConcatFile(const std::vector<SegmentInfo>& segments, const std::filesystem::path& listPath) const;

    std::filesystem::path temp_directory_;
};
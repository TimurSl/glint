#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

#include "expected.h"

struct ChunkRecord {
    int64_t id{0};
    int64_t session_id{0};
    std::string path;
    int64_t start_ms{0};
    int64_t end_ms{0};
    std::optional<int64_t> keyframe_ms;
};

class DB {
public:
    static DB& instance();

    void setCustomPath(const std::filesystem::path& path);
    glint::Expected<void, std::string> open();

    glint::Expected<int64_t, std::string> createSession(const std::string& game, int64_t startedAt, const std::string& container);
    glint::Expected<void, std::string> finalizeSession(int64_t sessionId, int64_t stoppedAt, const std::string& outputMp4);
    glint::Expected<int64_t, std::string> insertChunk(int sessionId, const std::string& path, int64_t startMs, int64_t endMs, std::optional<int64_t> keyframeMs);
    std::vector<ChunkRecord> chunksForSession(int sessionId) const;
    glint::Expected<void, std::string> removeChunk(int64_t chunkId);
    glint::Expected<void, std::string> removeChunksForSession(int sessionId);

    bool columnExists(const char* table, const char* column) const;

    glint::Expected<void, std::string> initSchema();
    sqlite3* handle();

private:
    DB();
    ~DB();

    struct SqliteDeleter {
        void operator()(sqlite3* handle) const noexcept { if (handle) sqlite3_close(handle); }
    };

    std::unique_ptr<sqlite3, SqliteDeleter> db_{};
    std::optional<std::filesystem::path> custom_path_{};
    std::filesystem::path getPath() const;
};
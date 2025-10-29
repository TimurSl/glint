#pragma once
#include <filesystem>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

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
    bool open();

    int64_t createSession(const std::string& game, int64_t startedAt, const std::string& container);
    void finalizeSession(int64_t sessionId, int64_t stoppedAt, const std::string& outputMp4);
    int64_t insertChunk(int sessionId, const std::string& path, int64_t startMs, int64_t endMs, std::optional<int64_t> keyframeMs);
    std::vector<ChunkRecord> chunksForSession(int sessionId) const;
    void removeChunk(int64_t chunkId);
    void removeChunksForSession(int sessionId);

    bool columnExists(const char* table, const char* column) const;

    void initSchema();
    sqlite3* handle();

private:
    DB();
    ~DB();

    sqlite3* db{};
    std::optional<std::filesystem::path> custom_path_{};
    std::filesystem::path getPath() const;
};
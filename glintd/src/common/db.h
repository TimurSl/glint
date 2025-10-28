#pragma once
#include <filesystem>
#include <sqlite3.h>
#include <string>

class DB {
public:
    static DB& instance();

    bool open();

    void insertChunk(int sessionId, const std::string &path, int64_t startMs, int64_t endMs, int64_t keyframeMs,
                     uint64_t sizeBytes);

    bool columnExists(const char *table, const char *column) const;

    void initSchema();
    sqlite3* handle();

private:
    DB();
    ~DB();

    sqlite3* db{};
    std::filesystem::path getPath() const;
};

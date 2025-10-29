#include "db.h"

#include <cstring>
#include <iostream>

#include "logger.h"

#ifdef _WIN32
#include <ShlObj.h>
#endif

DB::DB() {}
DB::~DB() {
    if (db) sqlite3_close(db);
}

DB& DB::instance() {
    static DB inst;
    return inst;
}

void DB::setCustomPath(const std::filesystem::path& path) {
    custom_path_ = path;
}

std::filesystem::path DB::getPath() const {
    if (custom_path_) {
        auto resolved = *custom_path_;
        if (resolved.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(resolved.parent_path(), ec);
            if (ec) {
                Logger::instance().error("Failed to create DB directory: " + ec.message());
            }
        }
        return resolved;
    }
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t* appData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) {
        wchar_t buf[MAX_PATH];
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        appData = _wcsdup(buf);
    }

    std::filesystem::path path(appData);
    CoTaskMemFree(appData);
    CoUninitialize();

    path /= "glint";
#else
    const char* home = getenv("HOME");
    std::filesystem::path path(home ? home : ".");
    path /= ".local/share/glint";
#endif
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        Logger::instance().error("Failed to create DB directory: " + ec.message());
    }

    std::filesystem::create_directories(path);
    Logger::instance().info("DB directory created: " + path.string());
    return path / "glintd.db";
}

bool DB::open() {
    if (db) return true;
    auto path = getPath();
    if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
        std::cerr << "SQLite open failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    initSchema();
    return true;
}

int64_t DB::createSession(const std::string& game, int64_t startedAt, const std::string& container) {
    const char* sql = "INSERT INTO sessions(game, started_at, container) VALUES(?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, game.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, startedAt);
    sqlite3_bind_text(stmt, 3, container.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(db);
}

void DB::finalizeSession(int64_t sessionId, int64_t stoppedAt, const std::string& outputMp4) {
    const char* sql = "UPDATE sessions SET stopped_at=?, output_mp4=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, stoppedAt);
    sqlite3_bind_text(stmt, 2, outputMp4.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, sessionId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int64_t DB::insertChunk(int sessionId, const std::string& path, int64_t startMs, int64_t endMs, std::optional<int64_t> keyframeMs) {
    const char* sql = "INSERT INTO chunks(session_id, path, start_ms, end_ms, keyframe_ms) VALUES(?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, sessionId);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, startMs);
    sqlite3_bind_int64(stmt, 4, endMs);
    if (keyframeMs.has_value()) {
        sqlite3_bind_int64(stmt, 5, *keyframeMs);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return sqlite3_last_insert_rowid(db);
}

std::vector<ChunkRecord> DB::chunksForSession(int sessionId) const {
    std::vector<ChunkRecord> records;
    const char* sql = "SELECT id, session_id, path, start_ms, end_ms, keyframe_ms FROM chunks WHERE session_id=? ORDER BY start_ms ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return records;
    sqlite3_bind_int(stmt, 1, sessionId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChunkRecord rec;
        rec.id = sqlite3_column_int64(stmt, 0);
        rec.session_id = sqlite3_column_int64(stmt, 1);
        rec.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.start_ms = sqlite3_column_int64(stmt, 3);
        rec.end_ms = sqlite3_column_int64(stmt, 4);
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            rec.keyframe_ms = sqlite3_column_int64(stmt, 5);
        }
        records.push_back(rec);
    }
    sqlite3_finalize(stmt);
    return records;
}

void DB::removeChunk(int64_t chunkId) {
    const char* sql = "DELETE FROM chunks WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, chunkId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void DB::removeChunksForSession(int sessionId) {
    const char* sql = "DELETE FROM chunks WHERE session_id=?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, sessionId);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool DB::columnExists(const char* table, const char* column) const {
    sqlite3_stmt* stmt;
    std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (strcmp(name, column) == 0) { found = true; break; }
    }
    sqlite3_finalize(stmt);
    return found;
}

void DB::initSchema() {
    const char* stmts[] = {
        R"SQL(
CREATE TABLE IF NOT EXISTS sessions(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    game TEXT NOT NULL,
    started_at INTEGER NOT NULL,
    stopped_at INTEGER,
    container TEXT,
    output_mp4 TEXT
    ts_ms INTEGER NOT NULL,
    pre INTEGER NOT NULL,
    post INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
)SQL",
        R"SQL(
CREATE TABLE IF NOT EXISTS chunks(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    path TEXT NOT NULL,
    start_ms INTEGER NOT NULL,
    end_ms INTEGER NOT NULL,
    keyframe_ms INTEGER,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
)SQL"
    };
    char* err = nullptr;
    for (auto* sql : stmts) {
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            Logger::instance().error(std::string("Schema init failed: ") + err);
            sqlite3_free(err);
        }
    }
}

sqlite3* DB::handle() { return db; }
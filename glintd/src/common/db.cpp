#include "db.h"
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

std::filesystem::path DB::getPath() const {
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

void DB::insertChunk(int sessionId, const std::string& path, int64_t startMs, int64_t endMs,
                     int64_t keyframeMs, uint64_t sizeBytes) {
    const char* sql = "INSERT INTO chunks(session_id, path, start_ms, end_ms, keyframe_ms, size_bytes) VALUES(?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int(stmt, 1, sessionId);
    sqlite3_bind_text(stmt, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, startMs);
    sqlite3_bind_int64(stmt, 4, endMs);
    sqlite3_bind_int64(stmt, 5, keyframeMs);
    sqlite3_bind_int64(stmt, 6, sizeBytes);
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
);
)SQL",
        R"SQL(
CREATE TABLE IF NOT EXISTS markers(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
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

    if (!columnExists("chunks", "size_bytes")) {
        sqlite3_exec(db, "ALTER TABLE chunks ADD COLUMN size_bytes INTEGER DEFAULT 0;", nullptr, nullptr, nullptr);
    }
}

sqlite3* DB::handle() { return db; }

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

void DB::initSchema() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS sessions(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    game TEXT NOT NULL,
    started_at INTEGER NOT NULL,
    stopped_at INTEGER,
    container TEXT,
    output_mp4 TEXT
);
CREATE TABLE IF NOT EXISTS markers(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    ts_ms INTEGER NOT NULL,
    pre INTEGER NOT NULL,
    post INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS chunks(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    path TEXT NOT NULL,
    start_ms INTEGER NOT NULL,
    end_ms INTEGER NOT NULL,
    keyframe_ms INTEGER,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
)SQL";
    char* err = nullptr;
    if (sqlite3_exec(db, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "Schema init error: " << err << std::endl;
        sqlite3_free(err);
    }
}

sqlite3* DB::handle() { return db; }

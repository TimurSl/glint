#include "db.h"

#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <system_error>

#include "logger.h"

#ifdef _WIN32
#include <ShlObj.h>
#include <Windows.h>
#endif

namespace {

struct StatementDeleter {
    void operator()(sqlite3_stmt* stmt) const noexcept {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
};

using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

[[nodiscard]] std::string sqliteMessage(sqlite3* handle, std::string_view context) {
    const char* raw = handle ? sqlite3_errmsg(handle) : nullptr;
    return std::format("{}: {}", context, raw ? raw : "unknown error");
}

glint::Expected<StatementPtr, std::string> prepare(sqlite3* handle, const char* sql, std::string_view context) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(handle, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return glint::unexpected(sqliteMessage(handle, context));
    }
    return StatementPtr(stmt);
}

glint::Expected<void, std::string> bindChecked(sqlite3* handle, int rc, std::string_view context) {
    if (rc != SQLITE_OK) {
        return glint::unexpected(sqliteMessage(handle, context));
    }
    return {};
}

} // namespace

DB::DB() = default;
DB::~DB() = default;

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
                Logger::instance().error(std::format("DB: failed to create custom directory {}: {}",
                                                     resolved.parent_path().string(), ec.message()));
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
    const char* home = std::getenv("HOME");
    std::filesystem::path path(home ? home : ".");
    path /= ".local/share/glint";
#endif

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        Logger::instance().error(std::format("DB: failed to create directory {}: {}", path.string(), ec.message()));
    }

    return path / "glintd.db";
}

glint::Expected<void, std::string> DB::open() {
    if (db_) {
        return {};
    }

    const auto path = getPath();
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            const auto message = std::format("DB: cannot create directory {}: {}",
                                             path.parent_path().string(), ec.message());
            Logger::instance().error(message);
            return glint::unexpected(message);
        }
    }

    sqlite3* handle = nullptr;
    const int rc = sqlite3_open(path.string().c_str(), &handle);
    if (rc != SQLITE_OK) {
        const std::string message = sqliteMessage(handle, std::format("open {}", path.string()));
        if (handle) {
            sqlite3_close(handle);
        }
        Logger::instance().error(std::format("DB: {}", message));
        return glint::unexpected(message);
    }

    db_.reset(handle);
    sqlite3_exec(db_.get(), "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);

    if (auto schema = initSchema(); !schema) {
        Logger::instance().error(std::format("DB: {}", schema.error()));
        db_.reset();
        return glint::unexpected(schema.error());
    }

    return {};
}

glint::Expected<void, std::string> DB::initSchema() {
    if (!db_) {
        return glint::unexpected(std::string{"database not open"});
    }

    static constexpr const char* statements[] = {
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
CREATE TABLE IF NOT EXISTS chunks(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL,
    path TEXT NOT NULL,
    start_ms INTEGER NOT NULL,
    end_ms INTEGER NOT NULL,
    keyframe_ms INTEGER,
    FOREIGN KEY(session_id) REFERENCES sessions(id) ON DELETE CASCADE
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
)SQL"
    };

    for (const char* sql : statements) {
        char* err = nullptr;
        if (const int rc = sqlite3_exec(db_.get(), sql, nullptr, nullptr, &err); rc != SQLITE_OK) {
            std::string message = err ? err : "unknown error";
            sqlite3_free(err);
            return glint::unexpected(std::format("schema: {}", message));
        }
    }

    return {};
}

glint::Expected<int64_t, std::string> DB::createSession(const std::string& game,
                                                        int64_t startedAt,
                                                        const std::string& container) {
    if (auto openRes = open(); !openRes) {
        return glint::unexpected(openRes.error());
    }

    constexpr auto sql = "INSERT INTO sessions(game, started_at, container) VALUES(?,?,?);";
    auto stmtRes = prepare(db_.get(), sql, "createSession.prepare");
    if (!stmtRes) {
        Logger::instance().error(std::format("DB: {}", stmtRes.error()));
        return glint::unexpected(stmtRes.error());
    }

    StatementPtr stmt = std::move(stmtRes.value());
    if (auto rc = bindChecked(db_.get(), sqlite3_bind_text(stmt.get(), 1, game.c_str(), -1, SQLITE_TRANSIENT),
                              "createSession.bind(game)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }
    if (auto rc = bindChecked(db_.get(), sqlite3_bind_int64(stmt.get(), 2, startedAt),
                              "createSession.bind(started_at)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }
    if (auto rc = bindChecked(db_.get(), sqlite3_bind_text(stmt.get(), 3, container.c_str(), -1, SQLITE_TRANSIENT),
                              "createSession.bind(container)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        const std::string message = sqliteMessage(db_.get(), "createSession.step");
        Logger::instance().error(std::format("DB: {}", message));
        return glint::unexpected(message);
    }

    return sqlite3_last_insert_rowid(db_.get());
}

glint::Expected<void, std::string> DB::finalizeSession(int64_t sessionId,
                                                       int64_t stoppedAt,
                                                       const std::string& outputMp4) {
    if (!db_) {
        return glint::unexpected(std::string{"database not open"});
    }

    constexpr auto sql = "UPDATE sessions SET stopped_at=?, output_mp4=? WHERE id=?;";
    auto stmtRes = prepare(db_.get(), sql, "finalizeSession.prepare");
    if (!stmtRes) {
        Logger::instance().error(std::format("DB: {}", stmtRes.error()));
        return glint::unexpected(stmtRes.error());
    }
    StatementPtr stmt = std::move(stmtRes.value());

    if (auto rc = bindChecked(db_.get(), sqlite3_bind_int64(stmt.get(), 1, stoppedAt),
                              "finalizeSession.bind(stopped_at)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }
    if (auto rc = bindChecked(db_.get(), sqlite3_bind_text(stmt.get(), 2, outputMp4.c_str(), -1, SQLITE_TRANSIENT),
                              "finalizeSession.bind(output)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }
    if (auto rc = bindChecked(db_.get(), sqlite3_bind_int64(stmt.get(), 3, sessionId),
                              "finalizeSession.bind(id)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        const std::string message = sqliteMessage(db_.get(), "finalizeSession.step");
        Logger::instance().error(std::format("DB: {}", message));
        return glint::unexpected(message);
    }
    return {};
}

glint::Expected<int64_t, std::string> DB::insertChunk(int sessionId,
                                                      const std::string& path,
                                                      int64_t startMs,
                                                      int64_t endMs,
                                                      std::optional<int64_t> keyframeMs) {
    if (!db_) {
        return glint::unexpected(std::string{"database not open"});
    }

    constexpr auto sql = "INSERT INTO chunks(session_id, path, start_ms, end_ms, keyframe_ms) VALUES(?,?,?,?,?);";
    auto stmtRes = prepare(db_.get(), sql, "insertChunk.prepare");
    if (!stmtRes) {
        Logger::instance().error(std::format("DB: {}", stmtRes.error()));
        return glint::unexpected(stmtRes.error());
    }
    StatementPtr stmt = std::move(stmtRes.value());

    if (auto rc = bindChecked(db_.get(), sqlite3_bind_int(stmt.get(), 1, sessionId),
                              "insertChunk.bind(session_id)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }
    if (auto rc = bindChecked(db_.get(), sqlite3_bind_text(stmt.get(), 2, path.c_str(), -1, SQLITE_TRANSIENT),
                              "insertChunk.bind(path)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }
    if (auto rc = bindChecked(db_.get(), sqlite3_bind_int64(stmt.get(), 3, startMs),
                              "insertChunk.bind(start_ms)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }
    if (auto rc = bindChecked(db_.get(), sqlite3_bind_int64(stmt.get(), 4, endMs),
                              "insertChunk.bind(end_ms)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }
    if (keyframeMs) {
        if (auto rc = bindChecked(db_.get(), sqlite3_bind_int64(stmt.get(), 5, *keyframeMs),
                                  "insertChunk.bind(keyframe)"); !rc) {
            Logger::instance().error(std::format("DB: {}", rc.error()));
            return glint::unexpected(rc.error());
        }
    } else {
        if (auto rc = bindChecked(db_.get(), sqlite3_bind_null(stmt.get(), 5),
                                  "insertChunk.bind(keyframe=null)"); !rc) {
            Logger::instance().error(std::format("DB: {}", rc.error()));
            return glint::unexpected(rc.error());
        }
    }

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        const std::string message = sqliteMessage(db_.get(), "insertChunk.step");
        Logger::instance().error(std::format("DB: {}", message));
        return glint::unexpected(message);
    }

    return sqlite3_last_insert_rowid(db_.get());
}

std::vector<ChunkRecord> DB::chunksForSession(int sessionId) const {
    std::vector<ChunkRecord> records;
    if (!db_) {
        return records;
    }

    constexpr auto sql = "SELECT id, session_id, path, start_ms, end_ms, keyframe_ms FROM chunks WHERE session_id=? ORDER BY start_ms ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Logger::instance().error(sqliteMessage(db_.get(), "chunksForSession.prepare"));
        return records;
    }

    StatementPtr guard(stmt);
    if (sqlite3_bind_int(stmt, 1, sessionId) != SQLITE_OK) {
        Logger::instance().error(sqliteMessage(db_.get(), "chunksForSession.bind"));
        return records;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChunkRecord rec{};
        rec.id = sqlite3_column_int64(stmt, 0);
        rec.session_id = sqlite3_column_int64(stmt, 1);
        const unsigned char* text = sqlite3_column_text(stmt, 2);
        rec.path = text ? reinterpret_cast<const char*>(text) : "";
        rec.start_ms = sqlite3_column_int64(stmt, 3);
        rec.end_ms = sqlite3_column_int64(stmt, 4);
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            rec.keyframe_ms = sqlite3_column_int64(stmt, 5);
        }
        records.push_back(std::move(rec));
    }

    return records;
}

glint::Expected<void, std::string> DB::removeChunk(int64_t chunkId) {
    if (!db_) {
        return glint::unexpected(std::string{"database not open"});
    }

    constexpr auto sql = "DELETE FROM chunks WHERE id=?;";
    auto stmtRes = prepare(db_.get(), sql, "removeChunk.prepare");
    if (!stmtRes) {
        Logger::instance().error(std::format("DB: {}", stmtRes.error()));
        return glint::unexpected(stmtRes.error());
    }
    StatementPtr stmt = std::move(stmtRes.value());

    if (auto rc = bindChecked(db_.get(), sqlite3_bind_int64(stmt.get(), 1, chunkId),
                              "removeChunk.bind(id)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        const std::string message = sqliteMessage(db_.get(), "removeChunk.step");
        Logger::instance().error(std::format("DB: {}", message));
        return glint::unexpected(message);
    }
    return {};
}

glint::Expected<void, std::string> DB::removeChunksForSession(int sessionId) {
    if (!db_) {
        return glint::unexpected(std::string{"database not open"});
    }

    constexpr auto sql = "DELETE FROM chunks WHERE session_id=?;";
    auto stmtRes = prepare(db_.get(), sql, "removeChunksForSession.prepare");
    if (!stmtRes) {
        Logger::instance().error(std::format("DB: {}", stmtRes.error()));
        return glint::unexpected(stmtRes.error());
    }
    StatementPtr stmt = std::move(stmtRes.value());

    if (auto rc = bindChecked(db_.get(), sqlite3_bind_int(stmt.get(), 1, sessionId),
                              "removeChunksForSession.bind(session_id)"); !rc) {
        Logger::instance().error(std::format("DB: {}", rc.error()));
        return glint::unexpected(rc.error());
    }

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        const std::string message = sqliteMessage(db_.get(), "removeChunksForSession.step");
        Logger::instance().error(std::format("DB: {}", message));
        return glint::unexpected(message);
    }
    return {};
}

bool DB::columnExists(const char* table, const char* column) const {
    if (!db_) {
        return false;
    }

    std::string sql = std::format("PRAGMA table_info({});", table);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        Logger::instance().error(sqliteMessage(db_.get(), "columnExists.prepare"));
        return false;
    }

    StatementPtr guard(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (name && std::strcmp(name, column) == 0) {
            return true;
        }
    }
    return false;
}

sqlite3* DB::handle() {
    return db_.get();
}

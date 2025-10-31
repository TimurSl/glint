#include "marker_manager.h"
#include "db.h"
#include "logger.h"

#include <format>
#include <memory>
#include <sqlite3.h>

namespace {
struct StatementDeleter {
    void operator()(sqlite3_stmt* stmt) const noexcept {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
};
using StatementPtr = std::unique_ptr<sqlite3_stmt, StatementDeleter>;
}

int MarkerManager::addSession(const std::string& game, const std::string& container, const std::string& output) {
    if (auto openRes = DB::instance().open(); !openRes) {
        Logger::instance().error(std::format("MarkerManager: {}", openRes.error()));
        return -1;
    }

    sqlite3* db = DB::instance().handle();
    if (!db) {
        Logger::instance().error("MarkerManager: database handle unavailable");
        return -1;
    }

    const char* sql = "INSERT INTO sessions (game, started_at, container, output_mp4) VALUES (?, strftime('%s','now')*1000, ?, ?)";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        Logger::instance().error(std::format("MarkerManager: prepare failed: {}", sqlite3_errmsg(db)));
        return -1;
    }

    StatementPtr stmt(raw);
    if (sqlite3_bind_text(stmt.get(), 1, game.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt.get(), 2, container.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt.get(), 3, output.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        Logger::instance().error(std::format("MarkerManager: bind failed: {}", sqlite3_errmsg(db)));
        return -1;
    }

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        Logger::instance().error(std::format("MarkerManager: step failed: {}", sqlite3_errmsg(db)));
        return -1;
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db));
}

void MarkerManager::stopSession(int id) {
    if (auto openRes = DB::instance().open(); !openRes) {
        Logger::instance().error(std::format("MarkerManager: {}", openRes.error()));
        return;
    }

    sqlite3* db = DB::instance().handle();
    if (!db) {
        Logger::instance().error("MarkerManager: database handle unavailable");
        return;
    }

    const char* sql = "UPDATE sessions SET stopped_at = strftime('%s','now')*1000 WHERE id=?";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        Logger::instance().error(std::format("MarkerManager: prepare failed: {}", sqlite3_errmsg(db)));
        return;
    }

    StatementPtr stmt(raw);
    if (sqlite3_bind_int(stmt.get(), 1, id) != SQLITE_OK) {
        Logger::instance().error(std::format("MarkerManager: bind failed: {}", sqlite3_errmsg(db)));
        return;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        Logger::instance().error(std::format("MarkerManager: step failed: {}", sqlite3_errmsg(db)));
    }
}

void MarkerManager::addMarker(int sid, int ts, int pre, int post) {
    if (auto openRes = DB::instance().open(); !openRes) {
        Logger::instance().error(std::format("MarkerManager: {}", openRes.error()));
        return;
    }

    sqlite3* db = DB::instance().handle();
    if (!db) {
        Logger::instance().error("MarkerManager: database handle unavailable");
        return;
    }

    const char* sql = "INSERT INTO markers (session_id, ts_ms, pre, post) VALUES (?, ?, ?, ?)";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        Logger::instance().error(std::format("MarkerManager: prepare failed: {}", sqlite3_errmsg(db)));
        return;
    }

    StatementPtr stmt(raw);
    if (sqlite3_bind_int(stmt.get(), 1, sid) != SQLITE_OK ||
        sqlite3_bind_int(stmt.get(), 2, ts) != SQLITE_OK ||
        sqlite3_bind_int(stmt.get(), 3, pre) != SQLITE_OK ||
        sqlite3_bind_int(stmt.get(), 4, post) != SQLITE_OK) {
        Logger::instance().error(std::format("MarkerManager: bind failed: {}", sqlite3_errmsg(db)));
        return;
    }
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        Logger::instance().error(std::format("MarkerManager: step failed: {}", sqlite3_errmsg(db)));
    }
}

std::vector<Marker> MarkerManager::listMarkers(int sid) {
    std::vector<Marker> res;
    if (auto openRes = DB::instance().open(); !openRes) {
        Logger::instance().error(std::format("MarkerManager: {}", openRes.error()));
        return res;
    }

    sqlite3* db = DB::instance().handle();
    if (!db) {
        Logger::instance().error("MarkerManager: database handle unavailable");
        return res;
    }

    const char* sql = "SELECT id, ts_ms, pre, post FROM markers WHERE session_id=?";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        Logger::instance().error(std::format("MarkerManager: prepare failed: {}", sqlite3_errmsg(db)));
        return res;
    }

    StatementPtr stmt(raw);
    if (sqlite3_bind_int(stmt.get(), 1, sid) != SQLITE_OK) {
        Logger::instance().error(std::format("MarkerManager: bind failed: {}", sqlite3_errmsg(db)));
        return res;
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Marker m;
        m.id = sqlite3_column_int(stmt.get(), 0);
        m.ts_ms = sqlite3_column_int(stmt.get(), 1);
        m.pre = sqlite3_column_int(stmt.get(), 2);
        m.post = sqlite3_column_int(stmt.get(), 3);
        res.push_back(m);
    }
    return res;
}
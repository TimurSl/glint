#include "marker_manager.h"
#include "db.h"
#include <sqlite3.h>
#include <iostream>

int MarkerManager::addSession(const std::string& game, const std::string& container, const std::string& output) {
    DB::instance().open();
    sqlite3* db = DB::instance().handle();
    const char* sql = "INSERT INTO sessions (game, started_at, container, output_mp4) VALUES (?, strftime('%s','now')*1000, ?, ?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, game.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, container.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, output.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    int id = (int)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

void MarkerManager::stopSession(int id) {
    DB::instance().open();
    sqlite3* db = DB::instance().handle();
    const char* sql = "UPDATE sessions SET stopped_at = strftime('%s','now')*1000 WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void MarkerManager::addMarker(int sid, int ts, int pre, int post) {
    DB::instance().open();
    sqlite3* db = DB::instance().handle();
    const char* sql = "INSERT INTO markers (session_id, ts_ms, pre, post) VALUES (?, ?, ?, ?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, sid);
    sqlite3_bind_int(stmt, 2, ts);
    sqlite3_bind_int(stmt, 3, pre);
    sqlite3_bind_int(stmt, 4, post);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<Marker> MarkerManager::listMarkers(int sid) {
    std::vector<Marker> res;
    DB::instance().open();
    sqlite3* db = DB::instance().handle();
    const char* sql = "SELECT id, ts_ms, pre, post FROM markers WHERE session_id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, sid);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Marker m;
        m.id = sqlite3_column_int(stmt, 0);
        m.ts_ms = sqlite3_column_int(stmt, 1);
        m.pre = sqlite3_column_int(stmt, 2);
        m.post = sqlite3_column_int(stmt, 3);
        res.push_back(m);
    }
    sqlite3_finalize(stmt);
    return res;
}

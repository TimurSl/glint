#pragma once
#include <filesystem>
#include <sqlite3.h>
#include <string>

class DB {
public:
    static DB& instance();

    bool open();
    void initSchema();
    sqlite3* handle();

private:
    DB();
    ~DB();

    sqlite3* db{};
    std::filesystem::path getPath() const;
};

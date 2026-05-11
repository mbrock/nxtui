#pragma once

#include <duckdb.h>

#include <string>
#include <string_view>

namespace nxt::io::duckdb {

std::string sql_string(std::string_view text);

class session
{
public:
    session();
    ~session();

    session(const session &) = delete;
    session & operator=(const session &) = delete;

    [[nodiscard]] duckdb_connection connection() const noexcept;

private:
    duckdb_database database_{};
    duckdb_connection connection_{};
};

void checked_append(duckdb_state state, duckdb_appender appender);

class appender_handle
{
public:
    appender_handle(
        duckdb_connection connection, std::string_view table_name);
    ~appender_handle();

    appender_handle(const appender_handle &) = delete;
    appender_handle & operator=(const appender_handle &) = delete;

    [[nodiscard]] duckdb_appender get() const noexcept;

    void close();

private:
    duckdb_appender appender_{};
};

void checked_query(duckdb_connection connection, const std::string & sql);
void checked_query_result(
    duckdb_connection connection,
    const std::string & sql,
    duckdb_result & result);
std::string string_value(duckdb_result & result, idx_t col, idx_t row);

} // namespace nxt::io::duckdb

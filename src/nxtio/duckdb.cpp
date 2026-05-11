#include <nxtio/duckdb.hpp>

#include <stdexcept>
#include <string>

namespace nxt::io::duckdb {

std::string sql_string(std::string_view text)
{
    auto out = std::string{};
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(c);
        if (c == '\'')
            out.push_back('\'');
    }
    return out;
}

session::session()
{
    if (duckdb_open(nullptr, &database_) == DuckDBError)
        throw std::runtime_error{"failed to open DuckDB database"};
    if (duckdb_connect(database_, &connection_) == DuckDBError) {
        duckdb_close(&database_);
        throw std::runtime_error{"failed to connect to DuckDB database"};
    }
}

session::~session()
{
    if (connection_)
        duckdb_disconnect(&connection_);
    if (database_)
        duckdb_close(&database_);
}

duckdb_connection session::connection() const noexcept
{
    return connection_;
}

void checked_append(duckdb_state state, duckdb_appender appender)
{
    if (state != DuckDBError)
        return;
    auto error =
        appender != nullptr ? duckdb_appender_error(appender) : nullptr;
    throw std::runtime_error{
        error != nullptr ? std::string{error}
                         : std::string{"DuckDB append failed"}};
}

appender_handle::appender_handle(
    duckdb_connection connection, std::string_view table_name)
{
    auto table_name_text = std::string{table_name};
    checked_append(
        duckdb_appender_create(
            connection, nullptr, table_name_text.c_str(), &appender_),
        appender_);
}

appender_handle::~appender_handle()
{
    if (appender_ != nullptr)
        duckdb_appender_destroy(&appender_);
}

duckdb_appender appender_handle::get() const noexcept
{
    return appender_;
}

void appender_handle::close()
{
    checked_append(duckdb_appender_close(appender_), appender_);
    if (duckdb_appender_destroy(&appender_) == DuckDBError)
        throw std::runtime_error{"failed to destroy DuckDB appender"};
}

void checked_query_result(
    duckdb_connection connection,
    const std::string & sql,
    duckdb_result & result)
{
    if (duckdb_query(connection, sql.c_str(), &result) == DuckDBError) {
        auto error = duckdb_result_error(&result);
        auto message = error != nullptr
                           ? std::string{error}
                           : std::string{"DuckDB query failed"};
        duckdb_destroy_result(&result);
        throw std::runtime_error{message};
    }
}

void checked_query(duckdb_connection connection, const std::string & sql)
{
    duckdb_result result{};
    checked_query_result(connection, sql, result);
    duckdb_destroy_result(&result);
}

std::string string_value(duckdb_result & result, idx_t col, idx_t row)
{
    char * value = duckdb_value_varchar(&result, col, row);
    if (value == nullptr)
        return {};
    std::string out{value};
    duckdb_free(value);
    return out;
}

} // namespace nxt::io::duckdb

#include <nxtio/nxtllm-trace-store.hpp>

#include <nxtio/duckdb.hpp>

#include <cstddef>
#include <string>

namespace nxt::io::nxtllm {

void write_trace_parquet(
    std::string_view path,
    std::string_view run_id,
    const std::vector<trace_row> & rows)
{
    auto session = duckdb::session{};
    auto connection = session.connection();
    duckdb::checked_query(
        connection,
        "create table trace ("
        "run_id varchar,"
        "seq bigint,"
        "elapsed_ms bigint,"
        "phase varchar,"
        "event_type varchar,"
        "data varchar,"
        "payload_json varchar)");

    auto appender_handle = duckdb::appender_handle{connection, "trace"};
    auto appender = appender_handle.get();
    auto run_id_text = std::string{run_id};
    for (const auto & row : rows) {
        duckdb::checked_append(
            duckdb_appender_begin_row(appender), appender);
        duckdb::checked_append(
            duckdb_append_varchar(appender, run_id_text.c_str()), appender);
        duckdb::checked_append(
            duckdb_append_int64(appender, row.seq), appender);
        duckdb::checked_append(
            duckdb_append_int64(appender, row.elapsed_ms), appender);
        duckdb::checked_append(
            duckdb_append_varchar(appender, row.phase.c_str()), appender);
        duckdb::checked_append(
            duckdb_append_varchar(appender, row.event_type.c_str()),
            appender);
        duckdb::checked_append(
            duckdb_append_varchar(appender, row.data.c_str()), appender);
        duckdb::checked_append(
            duckdb_append_varchar(appender, row.payload_json.c_str()),
            appender);
        duckdb::checked_append(duckdb_appender_end_row(appender), appender);
    }
    appender_handle.close();

    duckdb::checked_query(
        connection,
        "copy trace to '" + duckdb::sql_string(path)
            + "' (format parquet)");
}

std::vector<trace_row> read_trace_parquet(std::string_view path)
{
    auto session = duckdb::session{};
    duckdb_result result{};
    auto query = std::string{
        "select seq, elapsed_ms, phase, event_type, data, payload_json "
        "from read_parquet('" + duckdb::sql_string(path) + "') "
        "order by seq"};
    duckdb::checked_query_result(session.connection(), query, result);

    auto rows = std::vector<trace_row>{};
    rows.reserve(static_cast<std::size_t>(duckdb_row_count(&result)));
    for (idx_t i = 0; i < duckdb_row_count(&result); ++i) {
        rows.push_back(
            trace_row{
                .seq = duckdb_value_int64(&result, 0, i),
                .elapsed_ms = duckdb_value_int64(&result, 1, i),
                .phase = duckdb::string_value(result, 2, i),
                .event_type = duckdb::string_value(result, 3, i),
                .data = duckdb::string_value(result, 4, i),
                .payload_json = duckdb::string_value(result, 5, i),
            });
    }
    duckdb_destroy_result(&result);
    return rows;
}

} // namespace nxt::io::nxtllm

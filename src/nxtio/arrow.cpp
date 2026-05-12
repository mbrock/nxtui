#include <nxtio/arrow.hpp>

#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace nxt::io::arrow {
namespace {

constexpr int trace_field_count = 13;

struct nanoarrow_error
{
    ArrowError value{};

    nanoarrow_error()
    {
        ArrowErrorInit(&value);
    }
};

[[nodiscard]] std::string nanoarrow_message(
    std::string_view operation,
    ArrowErrorCode code,
    const ArrowError & error)
{
    auto message = std::string{operation};
    message += " failed";
    if (const char * detail =
            ArrowErrorMessage(const_cast<ArrowError *>(&error));
        detail != nullptr
        && detail[0] != '\0') {
        message += ": ";
        message += detail;
    } else if (code != NANOARROW_OK) {
        message += ": ";
        message += std::strerror(code);
    }
    return message;
}

void check(
    ArrowErrorCode code,
    std::string_view operation,
    const ArrowError & error)
{
    if (code != NANOARROW_OK)
        throw std::runtime_error{nanoarrow_message(operation, code, error)};
}

void check(ArrowErrorCode code, std::string_view operation)
{
    auto error = nanoarrow_error{};
    check(code, operation, error.value);
}

[[nodiscard]] ArrowStringView string_view(const std::string & value)
{
    return ArrowStringView{
        .data = value.data(),
        .size_bytes = static_cast<int64_t>(value.size()),
    };
}

[[nodiscard]] ArrowBufferView buffer_view(const std::vector<std::uint8_t> & value)
{
    return ArrowBufferView{
        .data = {.as_uint8 = value.data()},
        .size_bytes = static_cast<int64_t>(value.size()),
    };
}

[[nodiscard]] std::string to_string(ArrowStringView value)
{
    if (value.size_bytes <= 0)
        return {};
    return std::string{value.data, static_cast<std::size_t>(value.size_bytes)};
}

class schema_handle
{
public:
    schema_handle() = default;
    schema_handle(const schema_handle &) = delete;
    schema_handle & operator=(const schema_handle &) = delete;

    schema_handle(schema_handle && other) noexcept
        : schema_(other.schema_)
    {
        other.schema_.release = nullptr;
    }

    schema_handle & operator=(schema_handle && other) noexcept
    {
        if (this != &other) {
            reset();
            schema_ = other.schema_;
            other.schema_.release = nullptr;
        }
        return *this;
    }

    ~schema_handle()
    {
        reset();
    }

    [[nodiscard]] ArrowSchema * get() noexcept
    {
        return &schema_;
    }

    [[nodiscard]] const ArrowSchema * get() const noexcept
    {
        return &schema_;
    }

private:
    void reset() noexcept
    {
        if (schema_.release != nullptr)
            ArrowSchemaRelease(&schema_);
    }

    ArrowSchema schema_{};
};

class array_handle
{
public:
    array_handle() = default;
    array_handle(const array_handle &) = delete;
    array_handle & operator=(const array_handle &) = delete;

    array_handle(array_handle && other) noexcept
        : array_(other.array_)
    {
        other.array_.release = nullptr;
    }

    array_handle & operator=(array_handle && other) noexcept
    {
        if (this != &other) {
            if (array_.release != nullptr)
                ArrowArrayRelease(&array_);
            array_ = other.array_;
            other.array_.release = nullptr;
        }
        return *this;
    }

    ~array_handle()
    {
        if (array_.release != nullptr)
            ArrowArrayRelease(&array_);
    }

    [[nodiscard]] ArrowArray * get() noexcept
    {
        return &array_;
    }

private:
    ArrowArray array_{};
};

class array_view_handle
{
public:
    array_view_handle() = default;
    array_view_handle(const array_view_handle &) = delete;
    array_view_handle & operator=(const array_view_handle &) = delete;

    ~array_view_handle()
    {
        ArrowArrayViewReset(&view_);
    }

    [[nodiscard]] ArrowArrayView * get() noexcept
    {
        return &view_;
    }

private:
    ArrowArrayView view_{};
};

class array_stream_handle
{
public:
    array_stream_handle() = default;
    array_stream_handle(const array_stream_handle &) = delete;
    array_stream_handle & operator=(const array_stream_handle &) = delete;

    ~array_stream_handle()
    {
        if (stream_.release != nullptr)
            ArrowArrayStreamRelease(&stream_);
    }

    [[nodiscard]] ArrowArrayStream * get() noexcept
    {
        return &stream_;
    }

private:
    ArrowArrayStream stream_{};
};

class file_handle
{
public:
    file_handle(std::string_view path, const char * mode)
        : path_(path)
        , file_(std::fopen(path_.c_str(), mode))
    {
        if (file_ == nullptr)
            throw std::runtime_error{
                "failed to open trace file '" + path_ + "': "
                + std::strerror(errno)};
    }

    file_handle(const file_handle &) = delete;
    file_handle & operator=(const file_handle &) = delete;

    ~file_handle()
    {
        if (file_ != nullptr)
            std::fclose(file_);
    }

    [[nodiscard]] FILE * get() const noexcept
    {
        return file_;
    }

    void flush()
    {
        if (file_ != nullptr && std::fflush(file_) != 0)
            throw std::runtime_error{
                "failed to flush trace file '" + path_ + "': "
                + std::strerror(errno)};
    }

    void close()
    {
        if (file_ == nullptr)
            return;
        auto * file = std::exchange(file_, nullptr);
        if (std::fclose(file) != 0)
            throw std::runtime_error{
                "failed to close trace file '" + path_ + "': "
                + std::strerror(errno)};
    }

private:
    std::string path_;
    FILE * file_ = nullptr;
};

void set_child(
    ArrowSchema * schema,
    int index,
    const char * name,
    ArrowType type)
{
    check(ArrowSchemaSetType(schema->children[index], type), "set Arrow type");
    check(ArrowSchemaSetName(schema->children[index], name), "set Arrow name");
}

[[nodiscard]] schema_handle make_trace_schema()
{
    auto schema = schema_handle{};
    ArrowSchemaInit(schema.get());
    check(
        ArrowSchemaSetTypeStruct(schema.get(), trace_field_count),
        "create trace schema");
    set_child(schema.get(), 0, "run_id", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 1, "seq", NANOARROW_TYPE_INT64);
    set_child(schema.get(), 2, "elapsed_ms", NANOARROW_TYPE_INT64);
    set_child(schema.get(), 3, "phase", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 4, "event_type", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 5, "data", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 6, "payload_json", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 7, "span_id", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 8, "parent_span_id", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 9, "span_name", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 10, "frame_seq", NANOARROW_TYPE_INT64);
    set_child(schema.get(), 11, "payload_kind", NANOARROW_TYPE_STRING);
    set_child(schema.get(), 12, "payload_bin", NANOARROW_TYPE_BINARY);
    return schema;
}

[[nodiscard]] array_handle make_trace_batch(
    const ArrowSchema * schema,
    const trace_row & row)
{
    auto error = nanoarrow_error{};
    auto batch = array_handle{};
    check(
        ArrowArrayInitFromSchema(batch.get(), schema, &error.value),
        "create trace batch",
        error.value);
    check(ArrowArrayStartAppending(batch.get()), "start trace batch");

    check(
        ArrowArrayAppendString(batch.get()->children[0], string_view(row.run_id)),
        "append run_id");
    check(ArrowArrayAppendInt(batch.get()->children[1], row.seq), "append seq");
    check(
        ArrowArrayAppendInt(batch.get()->children[2], row.elapsed_ms),
        "append elapsed_ms");
    check(
        ArrowArrayAppendString(batch.get()->children[3], string_view(row.phase)),
        "append phase");
    check(
        ArrowArrayAppendString(
            batch.get()->children[4],
            string_view(row.event_type)),
        "append event_type");
    check(
        ArrowArrayAppendString(batch.get()->children[5], string_view(row.data)),
        "append data");
    check(
        ArrowArrayAppendString(
            batch.get()->children[6],
            string_view(row.payload_json)),
        "append payload_json");
    check(
        ArrowArrayAppendString(
            batch.get()->children[7],
            string_view(row.span_id)),
        "append span_id");
    check(
        ArrowArrayAppendString(
            batch.get()->children[8],
            string_view(row.parent_span_id)),
        "append parent_span_id");
    check(
        ArrowArrayAppendString(
            batch.get()->children[9],
            string_view(row.span_name)),
        "append span_name");
    check(
        ArrowArrayAppendInt(batch.get()->children[10], row.frame_seq),
        "append frame_seq");
    check(
        ArrowArrayAppendString(
            batch.get()->children[11],
            string_view(row.payload_kind)),
        "append payload_kind");
    check(
        ArrowArrayAppendBytes(
            batch.get()->children[12],
            buffer_view(row.payload_bin)),
        "append payload_bin");
    check(ArrowArrayFinishElement(batch.get()), "finish trace row");
    check(
        ArrowArrayFinishBuildingDefault(batch.get(), &error.value),
        "finish trace batch",
        error.value);
    return batch;
}

[[nodiscard]] trace_row row_from_view(const ArrowArrayView * view, int64_t index)
{
    auto bin_view = ArrowArrayViewGetBytesUnsafe(view->children[12], index);
    auto payload_bin = std::vector<std::uint8_t>{};
    if (bin_view.size_bytes > 0 && bin_view.data.as_uint8 != nullptr)
        payload_bin.assign(
            bin_view.data.as_uint8,
            bin_view.data.as_uint8
                + static_cast<std::size_t>(bin_view.size_bytes));
    return trace_row{
        .run_id = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[0], index)),
        .seq = ArrowArrayViewGetIntUnsafe(view->children[1], index),
        .elapsed_ms = ArrowArrayViewGetIntUnsafe(view->children[2], index),
        .phase = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[3], index)),
        .event_type = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[4], index)),
        .data = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[5], index)),
        .payload_json = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[6], index)),
        .span_id = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[7], index)),
        .parent_span_id = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[8], index)),
        .span_name = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[9], index)),
        .frame_seq = ArrowArrayViewGetIntUnsafe(view->children[10], index),
        .payload_kind = to_string(
            ArrowArrayViewGetStringUnsafe(view->children[11], index)),
        .payload_bin = std::move(payload_bin),
    };
}

} // namespace

class ipc_trace::writer
{
public:
    writer(std::string_view path, std::string_view run_id)
        : run_id_(run_id)
        , file_(path, "wb")
        , schema_(make_trace_schema())
    {
        auto error = nanoarrow_error{};
        auto output = ArrowIpcOutputStream{};
        check(
            ArrowIpcOutputStreamInitFile(
                &output,
                file_.get(),
                /*close_on_release=*/0),
            "open Arrow IPC output stream",
            error.value);

        auto writer_ready = false;
        try {
            check(
                ArrowIpcWriterInit(&writer_, &output),
                "create Arrow IPC writer",
                error.value);
            writer_ready = true;
            writer_initialized_ = true;

            check(
                ArrowArrayViewInitFromSchema(
                    &array_view_,
                    schema_.get(),
                    &error.value),
                "create trace array view",
                error.value);
            array_view_initialized_ = true;

            check(
                ArrowIpcWriterWriteSchema(
                    &writer_,
                    schema_.get(),
                    &error.value),
                "write Arrow IPC schema",
                error.value);
            file_.flush();
        } catch (...) {
            if (array_view_initialized_) {
                ArrowArrayViewReset(&array_view_);
                array_view_initialized_ = false;
            }
            if (writer_ready)
                ArrowIpcWriterReset(&writer_);
            else if (output.release != nullptr)
                output.release(&output);
            throw;
        }
    }

    writer(const writer &) = delete;
    writer & operator=(const writer &) = delete;

    ~writer()
    {
        try {
            close();
        } catch (...) {
        }
    }

    void append(const trace_row & row)
    {
        if (closed_)
            throw std::runtime_error{"cannot append to closed trace"};

        auto error = nanoarrow_error{};
        auto batch = make_trace_batch(schema_.get(), row);
        check(
            ArrowArrayViewSetArray(&array_view_, batch.get(), &error.value),
            "prepare trace batch",
            error.value);
        check(
            ArrowIpcWriterWriteArrayView(
                &writer_,
                &array_view_,
                &error.value),
            "write Arrow IPC trace batch",
            error.value);
        file_.flush();
    }

    void close()
    {
        if (closed_)
            return;

        auto error = nanoarrow_error{};
        auto close_error = ArrowErrorCode{NANOARROW_OK};
        if (writer_initialized_) {
            close_error = ArrowIpcWriterWriteArrayView(
                &writer_,
                nullptr,
                &error.value);
            ArrowIpcWriterReset(&writer_);
            writer_initialized_ = false;
        }
        if (array_view_initialized_) {
            ArrowArrayViewReset(&array_view_);
            array_view_initialized_ = false;
        }
        closed_ = true;
        file_.close();
        check(close_error, "finish Arrow IPC trace", error.value);
    }

private:
    std::string run_id_;
    file_handle file_;
    schema_handle schema_;
    ArrowIpcWriter writer_{};
    ArrowArrayView array_view_{};
    bool writer_initialized_ = false;
    bool array_view_initialized_ = false;
    bool closed_ = false;
};

ipc_trace::ipc_trace(
    std::optional<std::string> output_path,
    std::string run_id)
    : output_path_(std::move(output_path))
    , run_id_(std::move(run_id))
    , start_(std::chrono::steady_clock::now())
{
    if (output_path_) {
        // Don't synthesize a run id here: callers pass one in so the
        // same id can be used as a tag for sibling artifacts (PNGs,
        // logs, etc.) before the trace is even opened.
        if (run_id_.empty())
            run_id_ = "nxt-trace";
        writer_ = std::make_unique<writer>(*output_path_, run_id_);
    }
}

ipc_trace::~ipc_trace() = default;

bool ipc_trace::enabled() const noexcept
{
    return output_path_.has_value();
}

const std::optional<std::string> & ipc_trace::output_path() const
{
    return output_path_;
}

std::string_view ipc_trace::run_id() const noexcept
{
    return run_id_;
}

void ipc_trace::write()
{
    auto lock = std::scoped_lock{mutex_};
    if (writer_ != nullptr)
        writer_->close();
}

void ipc_trace::add(
    std::string phase,
    std::string event_type,
    std::string data,
    std::string payload_json)
{
    trace_row row;
    row.phase = std::move(phase);
    row.event_type = std::move(event_type);
    row.data = std::move(data);
    row.payload_json = std::move(payload_json);
    add(std::move(row));
}

void ipc_trace::add(trace_row row)
{
    auto lock = std::scoped_lock{mutex_};
    if (writer_ == nullptr)
        return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start_);
    row.run_id = run_id_;
    row.seq = next_seq_++;
    row.elapsed_ms = elapsed.count();
    writer_->append(row);
}

std::vector<trace_row> read_trace_ipc(std::string_view path)
{
    auto file = file_handle{path, "rb"};
    auto error = nanoarrow_error{};
    auto input = ArrowIpcInputStream{};
    check(
        ArrowIpcInputStreamInitFile(
            &input,
            file.get(),
            /*close_on_release=*/0),
        "open Arrow IPC input stream",
        error.value);

    auto input_owned = true;
    auto stream = array_stream_handle{};
    try {
        check(
            ArrowIpcArrayStreamReaderInit(stream.get(), &input, nullptr),
            "open Arrow IPC trace reader",
            error.value);
        input_owned = false;
    } catch (...) {
        if (input_owned && input.release != nullptr)
            input.release(&input);
        throw;
    }

    auto schema = schema_handle{};
    check(
        ArrowArrayStreamGetSchema(stream.get(), schema.get(), &error.value),
        "read trace schema",
        error.value);
    if (schema.get()->n_children != trace_field_count)
        throw std::runtime_error{"trace IPC schema has unexpected field count"};

    auto view = array_view_handle{};
    check(
        ArrowArrayViewInitFromSchema(view.get(), schema.get(), &error.value),
        "create trace reader array view",
        error.value);

    auto rows = std::vector<trace_row>{};
    while (true) {
        auto batch = array_handle{};
        check(
            ArrowArrayStreamGetNext(stream.get(), batch.get(), &error.value),
            "read trace batch",
            error.value);
        if (batch.get()->release == nullptr)
            break;

        check(
            ArrowArrayViewSetArray(view.get(), batch.get(), &error.value),
            "inspect trace batch",
            error.value);
        for (int64_t i = 0; i < view.get()->length; ++i)
            rows.push_back(row_from_view(view.get(), i));
    }

    return rows;
}

} // namespace nxt::io::arrow

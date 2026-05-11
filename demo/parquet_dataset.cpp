#include <arrow/api.h>
#include <arrow/compute/initialize.h>
#include <arrow/dataset/api.h>
#include <arrow/dataset/plan.h>
#include <arrow/filesystem/api.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace ds = arrow::dataset;
namespace fs = arrow::fs;

namespace {

arrow::Result<std::shared_ptr<ds::Dataset>> open_parquet_dataset(
    const std::string & path)
{
    auto local_fs = std::make_shared<fs::LocalFileSystem>();
    auto format = std::make_shared<ds::ParquetFileFormat>();
    auto options = ds::FileSystemFactoryOptions{};
    options.partitioning = ds::HivePartitioning::MakeFactory();

    if (std::filesystem::is_regular_file(path)) {
        ARROW_ASSIGN_OR_RAISE(auto info, local_fs->GetFileInfo(path));
        ARROW_ASSIGN_OR_RAISE(
            auto factory,
            ds::FileSystemDatasetFactory::Make(
                local_fs,
                std::vector<fs::FileInfo>{info},
                format,
                options));
        return factory->Finish();
    }

    auto selector = fs::FileSelector{};
    selector.base_dir = path;
    selector.recursive = true;

    ARROW_ASSIGN_OR_RAISE(
        auto factory,
        ds::FileSystemDatasetFactory::Make(
            local_fs,
            selector,
            format,
            options));
    return factory->Finish();
}

arrow::Status run(const std::string & path)
{
    ARROW_RETURN_NOT_OK(arrow::compute::Initialize());
    ds::internal::Initialize();

    ARROW_ASSIGN_OR_RAISE(auto dataset, open_parquet_dataset(path));

    std::cout << "Schema:\n" << dataset->schema()->ToString() << "\n";

    auto scanner_builder = ds::ScannerBuilder{dataset};
    ARROW_RETURN_NOT_OK(scanner_builder.UseThreads(true));

    ARROW_ASSIGN_OR_RAISE(auto scanner, scanner_builder.Finish());
    ARROW_ASSIGN_OR_RAISE(auto table, scanner->ToTable());

    std::cout << "Rows: " << table->num_rows()
              << "\nColumns: " << table->num_columns() << "\n";
    return arrow::Status::OK();
}

} // namespace

int main(int argc, char ** argv)
{
    auto path = std::string{argc > 1 ? argv[1] : "out.parquet"};
    auto status = run(path);
    if (!status.ok()) {
        std::cerr << status.ToString() << "\n";
        return 1;
    }
    return 0;
}

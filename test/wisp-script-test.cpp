// SPDX-License-Identifier: AGPL-3.0-or-later
// Runs the Wisp test files in test/wisp/*-test.wisp as nested tests. The
// files are read from the source tree when the suite runs, so editing a
// Wisp test needs no C++ rebuild. Each file is a group and each DEFTEST a
// test; every test runs in its own fresh machine.
#include <wisp/load.hpp>
#include <wisp/printer.hpp>

#include "test.hpp"
#include "wisp-base.hpp"

#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#ifndef NXT_WISP_TEST_DIR
#error "the build defines NXT_WISP_TEST_DIR as test/wisp in the source tree"
#endif

namespace wisp::test {
namespace {

using namespace boost::ut;

const std::filesystem::path script_dir{NXT_WISP_TEST_DIR};

std::string read_file(const std::filesystem::path & path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

// Test names must outlive the declarations that refer to them.
std::string_view keep(std::string text)
{
    static std::deque<std::string> names;
    return names.emplace_back(std::move(text));
}

/// A machine with the base library, the compiler, the harness, and one
/// test file.
struct script_machine
{
    explicit script_machine(const std::filesystem::path & file)
        : owner(compiler_image())
    {
        load(read_file(script_dir / "harness.wisp"), "harness.wisp");
        load(read_file(file), file.filename().string());
    }

    /// A private copy of a machine saved after loading.
    explicit script_machine(std::span<const std::byte> tape)
        : owner(tape::decode(tape))
    {
    }

    std::unique_ptr<image> owner;
    heap & h = owner->storage;
    evaluator & vm = owner->machine;

    void load(std::string_view text, std::string_view path)
    {
        loader source{h, vm, text, path};
        for (;;) {
            const auto state = source.advance(4096);
            vm.collect();
            if (state == evaluation::failed)
                throw std::runtime_error(
                    source.location() + ": "
                    + print(h, h.get<tag::run, field::err>(source.run())));
            if (state == evaluation::done)
                return;
        }
    }

    /// Evaluate one form and pass its value to `use` while its run is
    /// still rooted.
    template<typename Use>
    void evaluate(std::string_view text, Use && use)
    {
        root run{h, vm.start(reader{h, vm, text}.next().value())};
        for (;;) {
            const auto state = vm.advance(run.get(), 4096);
            vm.collect();
            if (state == evaluation::failed)
                throw std::runtime_error(
                    "runner form failed: "
                    + print(h, h.get<tag::run, field::err>(run.get())));
            if (state == evaluation::done)
                break;
        }
        use(h.get<tag::run, field::val>(run.get()));
    }

    std::vector<word> list(word xs)
    {
        std::vector<word> out;
        for (; xs != nil; xs = h.get<tag::duo, field::cdr>(xs))
            out.push_back(h.get<tag::duo, field::car>(xs));
        return out;
    }
};

std::vector<std::filesystem::path> script_files()
{
    std::vector<std::filesystem::path> files;
    for (const auto & entry : std::filesystem::directory_iterator{script_dir})
        if (entry.path().filename().string().ends_with("-test.wisp"))
            files.push_back(entry.path());
    std::ranges::sort(files);
    return files;
}

void declare_file(const std::filesystem::path & file)
{
    // Interpret the file once; every test decodes a fresh copy of the
    // loaded machine, so tests stay independent without reloading.
    script_machine m{file};
    const auto loaded = tape::encode(m.vm);
    std::vector<std::pair<std::string_view, bool>> tests;
    m.evaluate("(%test-names)", [&](word names) {
        for (auto name : m.list(names))
            tests.emplace_back(keep(std::string{m.h.v08slice(name)}), false);
    });
    m.evaluate("(%test-slow-flags)", [&](word flags) {
        auto i = tests.begin();
        for (auto flag : m.list(flags))
            (i++)->second = flag != nil;
    });
    for (std::size_t i = 0; i < tests.size(); ++i) {
        const auto [name, slow] = tests[i];
        auto run = [&loaded, i] {
            script_machine fresh{loaded};
            fresh.evaluate(
                "(%run-test " + std::to_string(i) + ")", [&](word failures) {
                    for (auto failure : fresh.list(failures))
                        expect(false) << print(fresh.h, failure);
                });
        };
        if (slow)
            test_case{name}.slow() = run;
        else
            test_case{name} = run;
    }
}

static suite script_tests{
    "WISP TEST FILES", [] {
        for (const auto & file : script_files())
            test_case{.name = keep(file.stem().string()), .is_group = true} =
                [&file] { declare_file(file); };
    }};

} // namespace
} // namespace wisp::test

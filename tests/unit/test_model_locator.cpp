// SPDX-License-Identifier: Apache-2.0
// Model discovery must not depend on the working directory or source tree.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "osie/inference/model_locator.hpp"

using namespace osie::inference;
namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path root;
    TempTree() {
        root = fs::temp_directory_path() / ("osie_locator_" + std::to_string(::getpid()) + "_" +
                                            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(root);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path touch(const fs::path& rel) const {
        const auto p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
        return p;
    }
};

}  // namespace

TEST(ModelLocator, ExplicitPathWinsAndIsReportedEvenIfMissing) {
    ModelSearch s;
    s.explicit_path = "/does/not/exist.osiem";
    s.env_path = "/also/missing.osiem";
    const auto loc = locate_model(s);
    EXPECT_EQ(loc.source, ModelSource::explicit_path);
    EXPECT_EQ(loc.path, "/does/not/exist.osiem");
    EXPECT_TRUE(loc.user_requested()) << "a missing explicit model must be an error, not a silent fallback";
}

TEST(ModelLocator, EnvironmentBeforeDataDirAndBundle) {
    TempTree t;
    ModelSearch s;
    s.env_path = (t.root / "env.osiem").string();
    s.data_dir = (t.root / "data").string();
    t.touch("data/models/intent.osiem");
    const auto loc = locate_model(s);
    EXPECT_EQ(loc.source, ModelSource::environment);
    EXPECT_TRUE(loc.user_requested());
}

TEST(ModelLocator, DataDirThenInstalledLayoutThenNone) {
    TempTree t;
    ModelSearch s;
    s.data_dir = (t.root / "data").string();
    s.executable = (t.root / "prefix" / "bin" / "osie").string();

    auto loc = locate_model(s);
    EXPECT_EQ(loc.source, ModelSource::none);
    EXPECT_TRUE(loc.path.empty());
    EXPECT_EQ(loc.searched.size(), 3U) << "data dir, <prefix>/share, <bindir>/share";

    const auto bundled = t.touch("prefix/share/osie/models/intent.osiem");
    loc = locate_model(s);
    EXPECT_EQ(loc.source, ModelSource::bundled);
    EXPECT_EQ(fs::path(loc.path), bundled.lexically_normal());
    EXPECT_FALSE(loc.user_requested());

    t.touch("data/models/intent.osiem");
    loc = locate_model(s);
    EXPECT_EQ(loc.source, ModelSource::user_data_dir) << "a user-installed model overrides the bundled one";
}

TEST(ModelLocator, NeverSearchesTheWorkingDirectory) {
    TempTree t;
    t.touch("models/intent.osiem");
    const auto old = fs::current_path();
    fs::current_path(t.root);
    ModelSearch s;  // no explicit path, env, data dir or executable
    const auto loc = locate_model(s);
    fs::current_path(old);
    EXPECT_EQ(loc.source, ModelSource::none);
    EXPECT_TRUE(loc.searched.empty());
}

TEST(ModelLocator, DirectoriesAreNotModels) {
    TempTree t;
    fs::create_directories(t.root / "data" / "models" / "intent.osiem");
    ModelSearch s;
    s.data_dir = (t.root / "data").string();
    EXPECT_EQ(locate_model(s).source, ModelSource::none);
}

TEST(ModelLocator, ExecutablePathIsAbsolute) {
    const auto p = executable_path();
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(fs::path(*p).is_absolute()) << *p;
    EXPECT_TRUE(fs::exists(*p));
}

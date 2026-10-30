#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <itch/mapped_file.hpp>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;

// A file in the temp directory that removes itself.
class TempFile {
public:
    TempFile(const std::string& name, const std::string& content)
        : path_(fs::temp_directory_path() / ("itch_mapped_" + name)) {
        std::ofstream(path_, std::ios::binary) << content;
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempFile(const TempFile&)            = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    fs::path path_;
};

std::string message_of(const std::string& path) {
    try {
        (void)itch::MappedFile::open(path);
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    return {};
}

TEST(MappedFile, MissingPathNamesThePath) {
    const std::string path = (fs::temp_directory_path() / "itch_no_such_file.bin").string();
    const std::string msg  = message_of(path);
    EXPECT_NE(msg.find(path), std::string::npos) << msg;
}

TEST(MappedFile, EmptyFileRejected) {
    const TempFile    f("empty.bin", "");
    const std::string msg = message_of(f.path());
    EXPECT_NE(msg.find(f.path()), std::string::npos) << msg;
    EXPECT_NE(msg.find("empty"), std::string::npos) << msg;
}

TEST(MappedFile, GzipMagicSaysDecompressFirst) {
    const TempFile    f("gz.bin", std::string("\x1f\x8b\x08\x00 payload", 12));
    const std::string msg = message_of(f.path());
    EXPECT_NE(msg.find("fetch_itch.py"), std::string::npos) << msg;
}

TEST(MappedFile, DirectoryRejected) {
    const std::string msg = message_of(fs::temp_directory_path().string());
    EXPECT_FALSE(msg.empty());
}

TEST(MappedFile, ExposesBytesAndPrefaults) {
    const std::string content(10000, 'x');
    const TempFile    f("ok.bin", content);
    const auto        mf = itch::MappedFile::open(f.path());
    ASSERT_EQ(mf.bytes().size(), content.size());
    EXPECT_EQ(std::to_integer<unsigned char>(mf.bytes()[0]), 'x');
    EXPECT_EQ(std::to_integer<unsigned char>(mf.bytes()[content.size() - 1]), 'x');
    mf.prefault();
}

TEST(MappedFile, MoveTransfersOwnership) {
    const TempFile f("move.bin", "abcd");
    auto           a = itch::MappedFile::open(f.path());
    auto           b = std::move(a);
    EXPECT_EQ(b.bytes().size(), 4U);
    EXPECT_TRUE(a.bytes().empty());  // NOLINT(bugprone-use-after-move)
}

}  // namespace

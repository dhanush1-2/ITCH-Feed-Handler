#include <gtest/gtest.h>

#include <cctype>
#include <itch/provenance.hpp>
#include <string>

namespace {

bool is_hex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

TEST(Provenance, CommitIsRealHashWithOptionalDirtySuffix) {
    std::string c = itch::make_provenance("x").commit;
    ASSERT_NE(c, "unknown") << "build was not configured through CMake inside a git checkout";
    const std::string suffix = "-dirty";
    if (c.size() > suffix.size() &&
        c.compare(c.size() - suffix.size(), suffix.size(), suffix) == 0) {
        c.resize(c.size() - suffix.size());
    }
    ASSERT_EQ(c.size(), 40U) << c;
    for (const char ch : c) {
        EXPECT_TRUE(is_hex(ch)) << c;
    }
}

TEST(Provenance, CompilerFlagsAndCpuArePopulated) {
    const auto p = itch::make_provenance("12302019.NASDAQ_ITCH50");
    EXPECT_FALSE(p.compiler.empty());
    EXPECT_NE(p.compiler, "unknown");
    EXPECT_FALSE(p.flags.empty());
    EXPECT_NE(p.flags, "unknown");
    EXPECT_FALSE(p.cpu.empty());
    EXPECT_EQ(p.data_file, "12302019.NASDAQ_ITCH50");
}

TEST(Provenance, JsonHasEveryKeyAndEscapes) {
    itch::Provenance p{"abc", "f\"ile", "cpu", "cc 1", "-O3 -DNDEBUG"};
    const auto       j = itch::provenance_json(p);
    for (const char* k : {"\"commit\"", "\"data_file\"", "\"cpu\"", "\"compiler\"", "\"flags\""}) {
        EXPECT_NE(j.find(k), std::string::npos) << k;
    }
    EXPECT_NE(j.find("f\\\"ile"), std::string::npos) << j;
}

}  // namespace

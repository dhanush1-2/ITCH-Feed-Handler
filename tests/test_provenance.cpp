#include <gtest/gtest.h>

#include <cctype>
#include <itch/provenance.hpp>
#include <string>

namespace {

bool is_hex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

TEST(Provenance, CommitStartsWithRealHash) {
    const std::string c = itch::make_provenance("x").commit;
    ASSERT_NE(c, "unknown") << "build was not configured through CMake inside a git checkout";
    ASSERT_GE(c.size(), 40U) << c;
    for (std::size_t i = 0; i < 40; ++i) {
        EXPECT_TRUE(is_hex(c[i])) << c;
    }
}

TEST(Provenance, ClassifyCommit) {
    const std::string a(40, 'a');
    const std::string b(40, 'b');
    EXPECT_EQ(itch::classify_commit(a, a, false), a);
    EXPECT_EQ(itch::classify_commit(a, a, true), a + "-dirty");
    EXPECT_EQ(itch::classify_commit(a, b, false), a + "-stale-build");
    EXPECT_EQ(itch::classify_commit(a, b, true), a + "-stale-build-dirty");
    // A binary built from a dirty tree can never match a hash, so it is never "clean".
    EXPECT_EQ(itch::classify_commit(a + "-dirty", a, false), a + "-dirty-stale-build");
    EXPECT_EQ(itch::classify_commit(a, "", false), a + "-unverified");
    EXPECT_EQ(itch::classify_commit("unknown", b, false), "unknown");
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

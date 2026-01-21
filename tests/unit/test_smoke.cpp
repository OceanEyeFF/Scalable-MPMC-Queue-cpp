#include <gtest/gtest.h>

#include <lscq/lscq.hpp>

static_assert(LSCQ_ENABLE_CAS2 == 0 || LSCQ_ENABLE_CAS2 == 1, "LSCQ_ENABLE_CAS2 must be 0/1");
static_assert(LSCQ_ENABLE_SANITIZERS == 0 || LSCQ_ENABLE_SANITIZERS == 1,
              "LSCQ_ENABLE_SANITIZERS must be 0/1");
static_assert(LSCQ_COMPILER_CLANG == 0 || LSCQ_COMPILER_CLANG == 1,
              "LSCQ_COMPILER_CLANG must be 0/1");

TEST(Smoke, HeaderIncludesAndMacrosAreCoherent) {
    EXPECT_EQ(lscq::kEnableCas2, LSCQ_ENABLE_CAS2 != 0);
    EXPECT_EQ(lscq::kEnableSanitizers, LSCQ_ENABLE_SANITIZERS != 0);
    EXPECT_EQ(lscq::kCompilerIsClang, LSCQ_COMPILER_CLANG != 0);
}

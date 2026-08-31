#include "SalsaCore/Application/ApplicationInfo.h"

#include <gtest/gtest.h>

#include <string_view>

namespace {

TEST(ApplicationInfo, ReportsApplicationIdentity) {
    EXPECT_EQ(salsa::core::applicationName(), std::string_view{ "SALSA" });
    EXPECT_FALSE(salsa::core::applicationVersion().empty());
}

}  // namespace

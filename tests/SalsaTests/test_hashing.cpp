#include "SalsaCore/Foundation/Hashing.h"

#include <gtest/gtest.h>

#include <span>
#include <string_view>

namespace salsa::core {
namespace {

[[nodiscard]] std::span<const std::byte> bytes(const std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

TEST(HashingTest, MatchesPublishedSha256Vectors) {
    const auto empty = sha256(bytes(""));
    ASSERT_TRUE(empty);
    EXPECT_EQ(
        empty.value().toHex(),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const auto abc = sha256(bytes("abc"));
    ASSERT_TRUE(abc);
    EXPECT_EQ(
        abc.value().toHex(),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HashingTest, StreamingMatchesOneShotHashing) {
    auto created = Sha256Hasher::create();
    ASSERT_TRUE(created);
    auto hasher = std::move(created).takeValue();
    ASSERT_TRUE(hasher.update(bytes("a")));
    ASSERT_TRUE(hasher.update(bytes("bc")));
    auto streamed = hasher.finish();
    ASSERT_TRUE(streamed);

    const auto oneShot = sha256(bytes("abc"));
    ASSERT_TRUE(oneShot);
    EXPECT_EQ(streamed.value(), oneShot.value());
    EXPECT_FALSE(hasher.finish());
    EXPECT_FALSE(hasher.update(bytes("later")));
}

}  // namespace
}  // namespace salsa::core

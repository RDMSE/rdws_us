// SPDX-FileCopyrightText: 2026 Rafael Dias
// SPDX-License-Identifier: Apache-2.0

#include "firmware/file_image_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using rdws::firmware::FileImageStore;

namespace fs = std::filesystem;

class FileImageStoreTest : public ::testing::Test {
protected:
  fs::path root = fs::temp_directory_path() / ("rdws_store_" + std::to_string(::getpid()));
  fs::path dir = root / "images";
  const std::string sha = std::string(64, 'a');

  void SetUp() override { fs::create_directories(dir); }
  void TearDown() override { fs::remove_all(root); }

  static void put(const fs::path& path, const std::string& content) {
    std::ofstream(path, std::ios::binary) << content;
  }
};

TEST_F(FileImageStoreTest, ReadsTheWholeFile) {
  put(dir / (sha + ".bin"), std::string("a\0b\n", 4));
  const auto data = FileImageStore(dir).read(sha, 100);
  ASSERT_TRUE(data.has_value());
  EXPECT_EQ(std::string(data->begin(), data->end()), std::string("a\0b\n", 4));
}

TEST_F(FileImageStoreTest, StopsOneByteOverTheLimit) {
  put(dir / (sha + ".bin"), std::string(50, 'x'));
  EXPECT_EQ(FileImageStore(dir).read(sha, 50)->size(), 50U);
  EXPECT_EQ(FileImageStore(dir).read(sha, 10)->size(), 11U);
}

TEST_F(FileImageStoreTest, MissingFileOrDirectory) {
  EXPECT_FALSE(FileImageStore(dir).read(sha, 100).has_value());
  EXPECT_FALSE(FileImageStore(dir / "nope").read(sha, 100).has_value());
}

TEST_F(FileImageStoreTest, OnlyHexNames) {
  put(root / "secret.bin", "s");
  for (const std::string& name : {std::string("../secret"), std::string("secret"), std::string()}) {
    EXPECT_FALSE(FileImageStore(dir).read(name, 100).has_value()) << name;
  }
}

TEST_F(FileImageStoreTest, NotALinkNorADirectory) {
  put(root / "secret.bin", "s");
  fs::create_symlink(root / "secret.bin", dir / (sha + ".bin"));
  EXPECT_FALSE(FileImageStore(dir).read(sha, 100).has_value());
  fs::remove(dir / (sha + ".bin"));
  fs::create_directory(dir / (sha + ".bin"));
  EXPECT_FALSE(FileImageStore(dir).read(sha, 100).has_value());
}

TEST_F(FileImageStoreTest, ReadsABlock) {
  put(dir / (sha + ".bin"), "0123456789");
  FileImageStore store(dir);
  const auto text = [](const auto& d) { return d ? std::string(d->begin(), d->end()) : "none"; };
  EXPECT_EQ(text(store.readAt(sha, 10, 0, 4)), "0123");
  EXPECT_EQ(text(store.readAt(sha, 10, 8, 2)), "89");
  EXPECT_EQ(text(store.readAt(sha, 10, 0, 10)), "0123456789");
  EXPECT_EQ(text(store.readAt(sha, 10, 10, 0)), "");
  EXPECT_EQ(text(store.readAt(sha, 10, 8, 3)), "none");
  EXPECT_EQ(text(store.readAt(sha, 10, 11, 0)), "none");
  EXPECT_EQ(text(store.readAt(sha, 10, 1, SIZE_MAX)), "none");
}

TEST_F(FileImageStoreTest, ABlockOnlyFromAFileOfTheRegisteredSize) {
  put(dir / (sha + ".bin"), "0123456789");
  FileImageStore store(dir);
  EXPECT_FALSE(store.readAt(sha, 9, 0, 4));
  EXPECT_FALSE(store.readAt(sha, 11, 0, 4));
  EXPECT_FALSE(store.readAt(std::string(64, 'b'), 10, 0, 4));
}

TEST_F(FileImageStoreTest, ABlockNeverFromElsewhere) {
  put(root / "secret.bin", "0123456789");
  fs::create_symlink(root / "secret.bin", dir / (sha + ".bin"));
  fs::create_directory(dir / (std::string(64, 'c') + ".bin"));
  FileImageStore store(dir);
  EXPECT_FALSE(store.readAt(sha, 10, 0, 4));
  EXPECT_FALSE(store.readAt("../secret", 10, 0, 4));
  EXPECT_FALSE(store.readAt(std::string(64, 'c'), 0, 0, 0));
}

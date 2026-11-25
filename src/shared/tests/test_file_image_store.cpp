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

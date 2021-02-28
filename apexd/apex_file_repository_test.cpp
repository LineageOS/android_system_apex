/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <filesystem>
#include <string>

#include <errno.h>
#include <sys/stat.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "apex_file.h"
#include "apex_file_repository.h"
#include "apexd_test_utils.h"
#include "apexd_verity.h"

namespace android {
namespace apex {

using namespace std::literals;

namespace fs = std::filesystem;

using android::apex::testing::IsOk;
using android::base::GetExecutableDirectory;
using android::base::RemoveFileIfExists;
using android::base::StringPrintf;

static std::string GetTestDataDir() { return GetExecutableDirectory(); }
static std::string GetTestFile(const std::string& name) {
  return GetTestDataDir() + "/" + name;
}

TEST(ApexFileRepositoryTest, InitializeSuccess) {
  // Prepare test data.
  TemporaryDir td;
  fs::copy(GetTestFile("apex.apexd_test.apex"), td.path);
  fs::copy(GetTestFile("apex.apexd_test_different_app.apex"), td.path);
  fs::copy(GetTestFile("com.android.apex.compressed.v1.capex"), td.path);

  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.Initialize({td.path})));

  // Now test that apexes were scanned correctly;
  auto test_fn = [&](const std::string& apex_name) {
    auto apex = ApexFile::Open(GetTestFile(apex_name));
    ASSERT_TRUE(IsOk(apex));

    {
      auto ret = instance.GetPublicKey(apex->GetManifest().name());
      ASSERT_TRUE(IsOk(ret));
      ASSERT_EQ(apex->GetBundledPublicKey(), *ret);
    }

    {
      auto ret = instance.GetPreinstalledPath(apex->GetManifest().name());
      ASSERT_TRUE(IsOk(ret));
      ASSERT_EQ(StringPrintf("%s/%s", td.path, apex_name.c_str()), *ret);
    }

    ASSERT_TRUE(instance.HasPreInstalledVersion(apex->GetManifest().name()));
  };

  test_fn("apex.apexd_test.apex");
  test_fn("apex.apexd_test_different_app.apex");
  test_fn("com.android.apex.compressed.v1.capex");

  // Check that second call will succeed as well.
  ASSERT_TRUE(IsOk(instance.Initialize({td.path})));

  test_fn("apex.apexd_test.apex");
  test_fn("apex.apexd_test_different_app.apex");
  test_fn("com.android.apex.compressed.v1.capex");
}

TEST(ApexFileRepositoryTest, InitializeFailureCorruptApex) {
  // Prepare test data.
  TemporaryDir td;
  fs::copy(GetTestFile("apex.apexd_test.apex"), td.path);
  fs::copy(GetTestFile("apex.apexd_test_corrupt_superblock_apex.apex"),
           td.path);

  ApexFileRepository instance;
  ASSERT_FALSE(IsOk(instance.Initialize({td.path})));
}

TEST(ApexFileRepositoryTest, InitializeCompressedApexWithoutApex) {
  // Prepare test data.
  TemporaryDir td;
  fs::copy(GetTestFile("com.android.apex.compressed.v1_without_apex.capex"),
           td.path);

  ApexFileRepository instance;
  // Compressed APEX without APEX cannot be opened
  ASSERT_FALSE(IsOk(instance.Initialize({td.path})));
}

TEST(ApexFileRepositoryTest, InitializeSameNameDifferentPathAborts) {
  // Prepare test data.
  TemporaryDir td;
  fs::copy(GetTestFile("apex.apexd_test.apex"), td.path);
  fs::copy(GetTestFile("apex.apexd_test.apex"),
           StringPrintf("%s/other.apex", td.path));

  ASSERT_DEATH(
      {
        ApexFileRepository instance;
        instance.Initialize({td.path});
      },
      "");
}

TEST(ApexFileRepositoryTest,
     InitializeSameNameDifferentPathAbortsCompressedApex) {
  // Prepare test data.
  TemporaryDir td;
  fs::copy(GetTestFile("com.android.apex.compressed.v1.capex"), td.path);
  fs::copy(GetTestFile("com.android.apex.compressed.v1.capex"),
           StringPrintf("%s/other.capex", td.path));

  ASSERT_DEATH(
      {
        ApexFileRepository instance;
        instance.Initialize({td.path});
      },
      "");
}

TEST(ApexFileRepositoryTest, InitializePublicKeyUnexpectdlyChangedAborts) {
  // Prepare test data.
  TemporaryDir td;
  fs::copy(GetTestFile("apex.apexd_test.apex"), td.path);

  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.Initialize({td.path})));

  // Check that apex was loaded.
  auto path = instance.GetPreinstalledPath("com.android.apex.test_package");
  ASSERT_TRUE(IsOk(path));
  ASSERT_EQ(StringPrintf("%s/apex.apexd_test.apex", td.path), *path);

  auto public_key = instance.GetPublicKey("com.android.apex.test_package");
  ASSERT_TRUE(IsOk(public_key));

  // Substitute it with another apex with the same name, but different public
  // key.
  fs::copy(GetTestFile("apex.apexd_test_different_key.apex"), *path,
           fs::copy_options::overwrite_existing);

  {
    auto apex = ApexFile::Open(*path);
    ASSERT_TRUE(IsOk(apex));
    // Check module name hasn't changed.
    ASSERT_EQ("com.android.apex.test_package", apex->GetManifest().name());
    // Check public key has changed.
    ASSERT_NE(*public_key, apex->GetBundledPublicKey());
  }

  ASSERT_DEATH({ instance.Initialize({td.path}); }, "");
}

TEST(ApexFileRepositoryTest,
     InitializePublicKeyUnexpectdlyChangedAbortsCompressedApex) {
  // Prepare test data.
  TemporaryDir td;
  fs::copy(GetTestFile("com.android.apex.compressed.v1.capex"), td.path);

  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.Initialize({td.path})));

  // Check that apex was loaded.
  auto path = instance.GetPreinstalledPath("com.android.apex.compressed");
  ASSERT_TRUE(IsOk(path));
  ASSERT_EQ(StringPrintf("%s/com.android.apex.compressed.v1.capex", td.path),
            *path);

  auto public_key = instance.GetPublicKey("com.android.apex.compressed");
  ASSERT_TRUE(IsOk(public_key));

  // Substitute it with another apex with the same name, but different public
  // key.
  fs::copy(GetTestFile("com.android.apex.compressed_different_key.capex"),
           *path, fs::copy_options::overwrite_existing);

  {
    auto apex = ApexFile::Open(*path);
    ASSERT_TRUE(IsOk(apex));
    // Check module name hasn't changed.
    ASSERT_EQ("com.android.apex.compressed", apex->GetManifest().name());
    // Check public key has changed.
    ASSERT_NE(*public_key, apex->GetBundledPublicKey());
  }

  ASSERT_DEATH({ instance.Initialize({td.path}); }, "");
}

TEST(ApexFileRepositoryTest, IsPreInstalledApex) {
  // Prepare test data.
  TemporaryDir td;
  fs::copy(GetTestFile("apex.apexd_test.apex"), td.path);
  fs::copy(GetTestFile("com.android.apex.compressed.v1.capex"), td.path);

  ApexFileRepository instance;
  ASSERT_TRUE(IsOk(instance.Initialize({td.path})));

  auto compressed_apex = ApexFile::Open(
      StringPrintf("%s/com.android.apex.compressed.v1.capex", td.path));
  ASSERT_TRUE(IsOk(compressed_apex));
  ASSERT_TRUE(instance.IsPreInstalledApex(*compressed_apex));

  auto apex1 = ApexFile::Open(StringPrintf("%s/apex.apexd_test.apex", td.path));
  ASSERT_TRUE(IsOk(apex1));
  ASSERT_TRUE(instance.IsPreInstalledApex(*apex1));

  // It's same apex, but path is different. Shouldn't be treated as
  // pre-installed.
  auto apex2 = ApexFile::Open(GetTestFile("apex.apexd_test.apex"));
  ASSERT_TRUE(IsOk(apex2));
  ASSERT_FALSE(instance.IsPreInstalledApex(*apex2));

  auto apex3 =
      ApexFile::Open(GetTestFile("apex.apexd_test_different_app.apex"));
  ASSERT_TRUE(IsOk(apex3));
  ASSERT_FALSE(instance.IsPreInstalledApex(*apex3));
}

TEST(ApexFileRepositoryTest, IsDecompressedApex) {
  // Prepare instance
  TemporaryDir decompression_dir;
  ApexFileRepository instance(decompression_dir.path);

  // Prepare decompressed apex
  std::string filename = "com.android.apex.compressed.v1_original.apex";
  fs::copy(GetTestFile(filename), decompression_dir.path);
  auto decompressed_path =
      StringPrintf("%s/%s", decompression_dir.path, filename.c_str());
  auto decompressed_apex = ApexFile::Open(decompressed_path);

  // Any file which is already located in |decompression_dir| should be
  // considered decompressed
  ASSERT_TRUE(instance.IsDecompressedApex(*decompressed_apex));

  // Hard links with same file name is considered decompressed
  TemporaryDir active_dir;
  auto active_path = StringPrintf("%s/%s", active_dir.path, filename.c_str());
  std::error_code ec;
  fs::create_hard_link(decompressed_path, active_path, ec);
  ASSERT_FALSE(ec) << "Failed to create hardlink";
  auto active_apex = ApexFile::Open(active_path);
  ASSERT_TRUE(instance.IsDecompressedApex(*active_apex));

  // Hard links with different filename is not considered decompressed
  auto different_name_path =
      StringPrintf("%s/different.name.apex", active_dir.path);
  fs::create_hard_link(decompressed_path, different_name_path, ec);
  ASSERT_FALSE(ec) << "Failed to create hardlink";
  auto different_name_apex = ApexFile::Open(different_name_path);
  ASSERT_FALSE(instance.IsDecompressedApex(*different_name_apex));

  // Same file name but not hard link -> not considered decompressed
  RemoveFileIfExists(active_path);
  fs::copy(GetTestFile(filename), active_dir.path);
  active_apex = ApexFile::Open(active_path);
  ASSERT_FALSE(instance.IsDecompressedApex(*active_apex));
}

}  // namespace apex
}  // namespace android
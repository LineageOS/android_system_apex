/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "apex_preinstalled_data.h"
#include "apexd.h"
#include "apexd_test_utils.h"

namespace android {
namespace apex {

namespace fs = std::filesystem;

using android::apex::testing::ApexFileEq;
using android::apex::testing::IsOk;
using android::base::GetExecutableDirectory;
using android::base::StringPrintf;
using ::testing::ByRef;
using ::testing::UnorderedElementsAre;

static std::string GetTestDataDir() { return GetExecutableDirectory(); }
static std::string GetTestFile(const std::string& name) {
  return GetTestDataDir() + "/" + name;
}

TEST(ApexdUnitTest, ScanAndGroupApexFiles) {
  TemporaryDir mock_built_in;
  fs::copy(GetTestFile("apex.apexd_test.apex"), mock_built_in.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), mock_built_in.path);

  TemporaryDir mock_data;
  fs::copy(GetTestFile("com.android.apex.cts.shim.v2.apex"), mock_data.path);

  std::vector<std::string> dirs_to_scan{mock_built_in.path, mock_data.path};
  auto result = ScanAndGroupApexFiles(dirs_to_scan);

  // Verify the contents of result
  auto apexd_test_file = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test.apex", mock_built_in.path));
  auto shim_v1 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.apex", mock_built_in.path));
  auto shim_v2 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.v2.apex", mock_data.path));

  ASSERT_EQ(result.size(), 2u);
  ASSERT_THAT(result[apexd_test_file->GetManifest().name()],
              UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file))));
  ASSERT_THAT(result[shim_v1->GetManifest().name()],
              UnorderedElementsAre(ApexFileEq(ByRef(*shim_v1)),
                                   ApexFileEq(ByRef(*shim_v2))));
}

// Apex that does not have pre-installed version, does not get selected
TEST(ApexdUnitTest, ApexMustHavePreInstalledVersionForSelection) {
  TemporaryDir mock_built_in;
  fs::copy(GetTestFile("apex.apexd_test.apex"), mock_built_in.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), mock_built_in.path);
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v1.libvX.apex"),
      mock_built_in.path);

  // Pre-installed information is not initialized
  ApexPreinstalledData instance;
  std::vector<std::string> dirs_to_scan{mock_built_in.path};
  auto all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  auto result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 0u);

  // Once initialized, pre-installed APEX should get selected
  ASSERT_TRUE(IsOk(instance.Initialize({mock_built_in.path})));
  all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 3u);
  auto apexd_test_file = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test.apex", mock_built_in.path));
  auto shim_v1 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.apex", mock_built_in.path));
  auto shared_lib = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
      mock_built_in.path));
  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file)),
                                           ApexFileEq(ByRef(*shim_v1)),
                                           ApexFileEq(ByRef(*shared_lib))));
}

// Higher version gets priority when selecting for activation
TEST(ApexdUnitTest, HigherVersionOfApexIsSelected) {
  TemporaryDir mock_built_in;
  fs::copy(GetTestFile("apex.apexd_test_v2.apex"), mock_built_in.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), mock_built_in.path);

  // Initialize pre-installed APEX information
  ApexPreinstalledData instance;
  ASSERT_TRUE(IsOk(instance.Initialize({mock_built_in.path})));

  TemporaryDir mock_data;
  fs::copy(GetTestFile("apex.apexd_test.apex"), mock_data.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.v2.apex"), mock_data.path);

  std::vector<std::string> dirs_to_scan{mock_built_in.path, mock_data.path};
  auto all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  auto result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 2u);

  auto apexd_test_file_v2 = ApexFile::Open(
      StringPrintf("%s/apex.apexd_test_v2.apex", mock_built_in.path));
  auto shim_v2 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.v2.apex", mock_data.path));
  ASSERT_THAT(result,
              UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file_v2)),
                                   ApexFileEq(ByRef(*shim_v2))));
}

// When versions are equal, non-pre-installed version gets priority
TEST(ApexdUnitTest, DataApexGetsPriorityForSameVersions) {
  TemporaryDir mock_built_in;
  fs::copy(GetTestFile("apex.apexd_test.apex"), mock_built_in.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), mock_built_in.path);

  // Initialize pre-installed APEX information
  ApexPreinstalledData instance;
  ASSERT_TRUE(IsOk(instance.Initialize({mock_built_in.path})));

  TemporaryDir mock_data;
  fs::copy(GetTestFile("apex.apexd_test.apex"), mock_data.path);
  fs::copy(GetTestFile("com.android.apex.cts.shim.apex"), mock_data.path);

  std::vector<std::string> dirs_to_scan{mock_built_in.path, mock_data.path};
  auto all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  auto result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 2u);

  auto apexd_test_file =
      ApexFile::Open(StringPrintf("%s/apex.apexd_test.apex", mock_data.path));
  auto shim_v1 = ApexFile::Open(
      StringPrintf("%s/com.android.apex.cts.shim.apex", mock_data.path));
  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*apexd_test_file)),
                                           ApexFileEq(ByRef(*shim_v1))));
}

// Both versions of shared libs can be selected
TEST(ApexdUnitTest, SharedLibsCanHaveBothVersionSelected) {
  TemporaryDir mock_built_in;
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v1.libvX.apex"),
      mock_built_in.path);

  // Initialize pre-installed APEX information
  ApexPreinstalledData instance;
  ASSERT_TRUE(IsOk(instance.Initialize({mock_built_in.path})));

  TemporaryDir mock_data;
  fs::copy(
      GetTestFile("com.android.apex.test.sharedlibs_generated.v2.libvY.apex"),
      mock_data.path);

  std::vector<std::string> dirs_to_scan{mock_built_in.path, mock_data.path};
  auto all_apex = ScanAndGroupApexFiles(dirs_to_scan);
  auto result = SelectApexForActivation(std::move(all_apex), instance);
  ASSERT_EQ(result.size(), 2u);

  auto shared_lib_v1 = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.test.sharedlibs_generated.v1.libvX.apex",
      mock_built_in.path));
  auto shared_lib_v2 = ApexFile::Open(StringPrintf(
      "%s/com.android.apex.test.sharedlibs_generated.v2.libvY.apex",
      mock_data.path));
  ASSERT_THAT(result, UnorderedElementsAre(ApexFileEq(ByRef(*shared_lib_v1)),
                                           ApexFileEq(ByRef(*shared_lib_v2))));
}

}  // namespace apex
}  // namespace android

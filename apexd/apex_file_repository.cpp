/*
 * Copyright (C) 2019 The Android Open Source Project
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

#define LOG_TAG "apexd"

#include "apex_file_repository.h"

#include <unordered_map>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/result.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include "apex_constants.h"
#include "apex_file.h"
#include "apexd_utils.h"

using android::base::Error;
using android::base::GetProperty;
using android::base::Result;
using android::base::StringPrintf;

namespace android {
namespace apex {

Result<void> ApexFileRepository::ScanBuiltInDir(const std::string& dir) {
  LOG(INFO) << "Scanning " << dir << " for pre-installed ApexFiles";
  if (access(dir.c_str(), F_OK) != 0 && errno == ENOENT) {
    LOG(WARNING) << dir << " does not exist. Skipping";
    return {};
  }

  Result<std::vector<std::string>> all_apex_files = FindFilesBySuffix(
      dir, {kApexPackageSuffix, kCompressedApexPackageSuffix});
  if (!all_apex_files.ok()) {
    return all_apex_files.error();
  }

  for (const auto& file : *all_apex_files) {
    Result<ApexFile> apex_file = ApexFile::Open(file);
    if (!apex_file.ok()) {
      return Error() << "Failed to open " << file << " : " << apex_file.error();
    }

    const std::string& name = apex_file->GetManifest().name();
    auto it = pre_installed_store_.find(name);
    if (it == pre_installed_store_.end()) {
      pre_installed_store_.emplace(name, std::move(*apex_file));
    } else if (it->second.GetPath() != apex_file->GetPath()) {
      // Currently, on some -eng builds there are two art apexes on /system
      // partition. While this issue is not fixed, exempt art apex from the
      // duplicate check on -eng builds.
      // TODO(b/176497601): remove this exemption once issue with duplicate art
      // apex is resolved.
      std::string build_type = GetProperty("ro.build.type", "");
      auto level = build_type == "eng" && name == "com.android.art"
                       ? base::ERROR
                       : base::FATAL;
      LOG(level) << "Found two apex packages " << it->second.GetPath()
                 << " and " << apex_file->GetPath()
                 << " with the same module name " << name;
    } else if (it->second.GetBundledPublicKey() !=
               apex_file->GetBundledPublicKey()) {
      LOG(FATAL) << "Public key of apex package " << it->second.GetPath()
                 << " (" << name << ") has unexpectedly changed";
    }
  }
  return {};
}

ApexFileRepository& ApexFileRepository::GetInstance() {
  static ApexFileRepository instance;
  return instance;
}

android::base::Result<void> ApexFileRepository::AddPreInstalledApex(
    const std::vector<std::string>& prebuilt_dirs) {
  for (const auto& dir : prebuilt_dirs) {
    if (auto result = ScanBuiltInDir(dir); !result.ok()) {
      return result.error();
    }
  }
  return {};
}

// TODO(b/179497746): AddDataApex should not concern with filtering out invalid
//   apex.
Result<void> ApexFileRepository::AddDataApex(const std::string& data_dir) {
  LOG(INFO) << "Scanning " << data_dir << " for data ApexFiles";
  if (access(data_dir.c_str(), F_OK) != 0 && errno == ENOENT) {
    LOG(WARNING) << data_dir << " does not exist. Skipping";
    return {};
  }

  Result<std::vector<std::string>> all_apex_files =
      FindFilesBySuffix(data_dir, {kApexPackageSuffix});
  if (!all_apex_files.ok()) {
    return all_apex_files.error();
  }

  for (const auto& file : *all_apex_files) {
    Result<ApexFile> apex_file = ApexFile::Open(file);
    if (!apex_file.ok()) {
      return Error() << "Failed to open " << file << " : " << apex_file.error();
    }

    const std::string& name = apex_file->GetManifest().name();
    if (!HasPreInstalledVersion(name)) {
      // Ignore data apex without corresponding pre-installed apex
      continue;
    }
    auto pre_installed_public_key = GetPublicKey(name);
    if (!pre_installed_public_key.ok() ||
        apex_file->GetBundledPublicKey() != *pre_installed_public_key) {
      // Ignore data apex if public key doesn't match with pre-installed apex
      continue;
    }

    auto it = data_store_.find(name);
    if (it == data_store_.end()) {
      data_store_.emplace(name, std::move(*apex_file));
      continue;
    }

    const auto& existing_version = it->second.GetManifest().version();
    const auto new_version = apex_file->GetManifest().version();
    // If multiple data apexs are preset, select the one with highest version
    bool prioritize_higher_version = new_version > existing_version;
    // For same version, non-decompressed apex gets priority
    bool prioritize_non_decompressed =
        (new_version == existing_version) && !IsDecompressedApex(*apex_file);
    if (prioritize_higher_version || prioritize_non_decompressed) {
      it->second = std::move(*apex_file);
    }
  }
  return {};
}

// TODO(b/179497746): remove this method when we add api for fetching ApexFile
//  by name
Result<const std::string> ApexFileRepository::GetPublicKey(
    const std::string& name) const {
  auto it = pre_installed_store_.find(name);
  if (it == pre_installed_store_.end()) {
    return Error() << "No preinstalled data found for package " << name;
  }
  return it->second.GetBundledPublicKey();
}

// TODO(b/179497746): remove this method when we add api for fetching ApexFile
//  by name
Result<const std::string> ApexFileRepository::GetPreinstalledPath(
    const std::string& name) const {
  auto it = pre_installed_store_.find(name);
  if (it == pre_installed_store_.end()) {
    return Error() << "No preinstalled data found for package " << name;
  }
  return it->second.GetPath();
}

bool ApexFileRepository::HasPreInstalledVersion(const std::string& name) const {
  return pre_installed_store_.find(name) != pre_installed_store_.end();
}

// ApexFile is considered a decompressed APEX if it is a hard link of file in
// |decompression_dir_| with same filename
bool ApexFileRepository::IsDecompressedApex(const ApexFile& apex) const {
  namespace fs = std::filesystem;
  const std::string filename = fs::path(apex.GetPath()).filename();
  const std::string decompressed_path =
      StringPrintf("%s/%s", decompression_dir_.c_str(), filename.c_str());

  std::error_code ec;
  bool hard_link_exists = fs::equivalent(decompressed_path, apex.GetPath(), ec);
  return !ec && hard_link_exists;
}

bool ApexFileRepository::IsPreInstalledApex(const ApexFile& apex) const {
  auto it = pre_installed_store_.find(apex.GetManifest().name());
  if (it == pre_installed_store_.end()) {
    return false;
  }
  return it->second.GetPath() == apex.GetPath() || IsDecompressedApex(apex);
}

std::vector<std::reference_wrapper<const ApexFile>>
ApexFileRepository::GetPreInstalledApexFiles() {
  std::vector<std::reference_wrapper<const ApexFile>> result;
  for (const auto& it : pre_installed_store_) {
    result.emplace_back(std::cref(it.second));
  }
  return std::move(result);
}

std::vector<std::reference_wrapper<const ApexFile>>
ApexFileRepository::GetDataApexFiles() {
  std::vector<std::reference_wrapper<const ApexFile>> result;
  for (const auto& it : data_store_) {
    result.emplace_back(std::cref(it.second));
  }
  return std::move(result);
}

}  // namespace apex
}  // namespace android

/*
 * Copyright 2020 The Android Open Source Project
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

#include "../apex_file.h"

#include <android-base/file.h>

using android::apex::ApexFile;
using android::base::WriteFully;

// A very basic fuzzer that just feeds random bytes to ApexFile::Open.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  TemporaryFile file;
  // Ignore failure to write to the temporary file, that's not what we are
  // fuzzing.
  if (!WriteFully(file.fd, data, size)) {
    return 0;
  }
  auto apex_file = ApexFile::Open(file.path);
  return 0;
}

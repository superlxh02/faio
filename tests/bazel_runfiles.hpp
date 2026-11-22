#pragma once

#include "rules_cc/cc/runfiles/runfiles.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace faio_test {

inline std::string bazel_runfile(const std::string& relative_path) {
  using rules_cc::cc::runfiles::Runfiles;
  std::string error;
  const std::unique_ptr<Runfiles> files{Runfiles::CreateForTest(BAZEL_CURRENT_REPOSITORY, &error)};
  if (!files)
    throw std::runtime_error("Cannot locate Bazel test runfiles: " + error);
  return files->Rlocation("faio/" + relative_path);
}

}  // namespace faio_test

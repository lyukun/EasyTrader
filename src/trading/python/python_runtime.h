#pragma once

#include <filesystem>
#include <memory>

namespace trading {

// Owns one embedded interpreter on the core thread
class PythonRuntime final {
 public:
  explicit PythonRuntime(const std::filesystem::path& module_directory);
  ~PythonRuntime();

  PythonRuntime(const PythonRuntime&) = delete;
  PythonRuntime& operator=(const PythonRuntime&) = delete;

  void assertOwner() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace trading

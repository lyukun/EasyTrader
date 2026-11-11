#include "trading/python/python_runtime.h"

#include <pybind11/embed.h>

#include <exception>
#include <stdexcept>
#include <thread>

namespace trading {
namespace py = pybind11;

// Keeps the embedded module registration in the linked static library
void registerPythonBindings();

struct PythonRuntime::Impl {
  std::thread::id owner{std::this_thread::get_id()};
  std::unique_ptr<py::scoped_interpreter> interpreter;
  std::unique_ptr<py::gil_scoped_release> release;
};

PythonRuntime::PythonRuntime(const std::filesystem::path& module_directory)
    : impl_(std::make_unique<Impl>()) {
  if (Py_IsInitialized()) {
    throw std::logic_error("an embedded Python interpreter is already running");
  }
  if (!std::filesystem::is_directory(module_directory)) {
    throw std::invalid_argument("Python module directory does not exist");
  }
  registerPythonBindings();
  impl_->interpreter = std::make_unique<py::scoped_interpreter>();
  try {
    py::module_::import("sys").attr("path").attr("insert")(
        0, std::filesystem::absolute(module_directory).string());
    py::module_::import("easytrader");
  } catch (const py::error_already_set& e) {
    throw std::runtime_error(std::string("Python initialization failed:\n") +
                             e.what());
  }
  impl_->release = std::make_unique<py::gil_scoped_release>();
}

PythonRuntime::~PythonRuntime() {
  if (impl_->owner != std::this_thread::get_id()) {
    std::terminate();
  }
  // Reacquire the initializing thread's GIL before interpreter finalization
  impl_->release.reset();
  impl_->interpreter.reset();
}

void PythonRuntime::assertOwner() const {
  if (impl_->owner != std::this_thread::get_id()) {
    throw std::logic_error("Python runtime accessed outside the core thread");
  }
}

}  // namespace trading

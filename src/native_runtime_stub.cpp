#include "nexodion/native_runtime.hpp"
namespace nexodion {
std::unique_ptr<NativeRuntime> create_native_runtime(std::string& error) {
    error = "Native SDL display support was not enabled. Build with -DNEXODION_NATIVE_UI=ON or use --headless.";
    return nullptr;
}
}
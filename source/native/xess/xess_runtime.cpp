#include "xess_runtime.h"
#include <stdexcept>

namespace xess_runtime
{
std::filesystem::path ModulePath(HMODULE module)
{
    std::wstring text(32768, L'\0');
    const DWORD count = GetModuleFileNameW(module, text.data(), static_cast<DWORD>(text.size()));
    if (!count || count >= text.size()) throw std::runtime_error("module_path_unavailable");
    text.resize(count);
    return std::filesystem::path(text);
}
}

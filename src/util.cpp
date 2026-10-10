#include "util.h"

#include <windows.h>

#include <cstdio>
#include <iterator>

namespace lumen {

std::string toUtf8(const std::wstring& value)
{
    if (value.empty())
        return { };

    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return { };

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring fromUtf8(const std::string& value)
{
    if (value.empty())
        return { };

    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return { };

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring executableDirectory()
{
    wchar_t path[MAX_PATH * 2] { };
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path))
        return L".";

    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        result.erase(slash);
    return result;
}

void log(const std::wstring& message)
{
    const std::wstring path = executableDirectory() + L"\\lumen.log";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0 || !file)
        return;

    SYSTEMTIME now { };
    GetLocalTime(&now);

    char prefix[64] { };
    std::snprintf(prefix, sizeof(prefix), "[%04d-%02d-%02d %02d:%02d:%02d] ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    const std::string line = std::string(prefix) + toUtf8(message) + "\r\n";
    std::fwrite(line.data(), 1, line.size(), file);
    std::fclose(file);
}

} // namespace lumen

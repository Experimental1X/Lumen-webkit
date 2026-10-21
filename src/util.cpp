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

std::wstring percentEncode(const std::wstring& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    const std::string utf8 = toUtf8(value);

    std::string encoded;
    encoded.reserve(utf8.size() * 3);
    for (unsigned char byte : utf8) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hex[(byte >> 4) & 0x0F]);
        encoded.push_back(hex[byte & 0x0F]);
    }
    return fromUtf8(encoded);
}

std::string percentDecodeBytes(const std::wstring& value)
{
    auto hexDigit = [](wchar_t c, int& out) {
        if (c >= L'0' && c <= L'9') { out = c - L'0'; return true; }
        if (c >= L'a' && c <= L'f') { out = c - L'a' + 10; return true; }
        if (c >= L'A' && c <= L'F') { out = c - L'A' + 10; return true; }
        return false;
    };

    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        int high = 0, low = 0;
        if (value[i] == L'%' && i + 2 < value.size()
            && hexDigit(value[i + 1], high) && hexDigit(value[i + 2], low)) {
            decoded.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else {
            decoded.push_back(static_cast<char>(value[i]));
        }
    }
    return decoded;
}

std::wstring percentDecode(const std::wstring& value)
{
    return fromUtf8(percentDecodeBytes(value));
}

std::wstring fileUrlFromPath(const std::wstring& path)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    const std::string utf8 = toUtf8(path);

    std::string encoded = "file:///";
    for (unsigned char byte : utf8) {
        if (byte == '\\') {
            encoded.push_back('/');
        } else if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '_' || byte == '.' || byte == '~' || byte == '/' || byte == ':') {
            encoded.push_back(static_cast<char>(byte));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(byte >> 4) & 0x0F]);
            encoded.push_back(hex[byte & 0x0F]);
        }
    }
    return fromUtf8(encoded);
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

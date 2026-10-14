#pragma once

#include <string>

namespace lumen {

std::string toUtf8(const std::wstring& value);
std::wstring fromUtf8(const std::string& value);

// Percent-encodes a string for use in a URL query.
std::wstring percentEncode(const std::wstring& value);

// "C:\\Users\\me\\My File.txt" becomes "file:///C:/Users/me/My%20File.txt".
std::wstring fileUrlFromPath(const std::wstring& path);

// Escape `<`, `>`, `&`, `"`, `'` for safe inclusion in generated HTML.
// Shared between downloads_page.h and settings_page.h so we don't end up
// with two anonymous-namespace definitions of the same function in a single
// translation unit.
std::string escapeHtml(const std::string& s);

// Appends a timestamped line to lumen.log next to the executable. Used to
// diagnose problems on machines without a debugger.
void log(const std::wstring& message);

// Directory containing Lumen.exe, without a trailing slash.
std::wstring executableDirectory();

} // namespace lumen

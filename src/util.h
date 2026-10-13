#pragma once

#include <string>

namespace lumen {

std::string toUtf8(const std::wstring& value);
std::wstring fromUtf8(const std::string& value);

// Percent-encodes a string for use in a URL query.
std::wstring percentEncode(const std::wstring& value);

// "C:\\Users\\me\\My File.txt" becomes "file:///C:/Users/me/My%20File.txt".
std::wstring fileUrlFromPath(const std::wstring& path);

// Appends a timestamped line to lumen.log next to the executable. Used to
// diagnose problems on machines without a debugger.
void log(const std::wstring& message);

// Directory containing Lumen.exe, without a trailing slash.
std::wstring executableDirectory();

} // namespace lumen

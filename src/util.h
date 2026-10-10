#pragma once

#include <string>

namespace lumen {

std::string toUtf8(const std::wstring& value);
std::wstring fromUtf8(const std::string& value);

// Appends a timestamped line to lumen.log next to the executable. Used to
// diagnose problems on machines without a debugger.
void log(const std::wstring& message);

// Directory containing Lumen.exe, without a trailing slash.
std::wstring executableDirectory();

} // namespace lumen

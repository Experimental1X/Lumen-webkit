#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace lumen {

// Dialogs for the things web pages can ask for.
void showAlert(HWND parent, const std::wstring& message);
bool showConfirm(HWND parent, const std::wstring& message);
bool showPrompt(HWND parent, const std::wstring& message, const std::wstring& defaultValue, std::wstring& result);

// File picker for <input type=file>. Empty when cancelled.
std::vector<std::wstring> chooseFiles(HWND parent, bool allowMultiple);

} // namespace lumen

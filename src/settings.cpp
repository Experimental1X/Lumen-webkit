#include "settings.h"
#include "util.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace lumen {

namespace {

std::wstring settingsPath()
{
    wchar_t buffer[MAX_PATH * 2] { };
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));

    std::wstring directory = (length > 0 && length < std::size(buffer))
        ? std::wstring(buffer, length)
        : executableDirectory();
    directory += L"\\Lumen";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\settings.tsv";
}

std::string sanitize(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        if (ch == '\t' || ch == '\r' || ch == '\n')
            result.push_back(' ');
        else
            result.push_back(ch);
    }
    return result;
}

} // namespace

Settings& Settings::instance()
{
    static Settings settings;
    return settings;
}

void Settings::load()
{
    m_entries.clear();

    FILE* file = nullptr;
    if (_wfopen_s(&file, settingsPath().c_str(), L"rb") != 0 || !file)
        return;

    std::string data;
    char buffer[4096];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        data.append(buffer, count);
    std::fclose(file);

    size_t start = 0;
    while (start < data.size()) {
        size_t end = data.find('\n', start);
        if (end == std::string::npos)
            end = data.size();

        std::string line = data.substr(start, end - start);
        start = end + 1;

        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        const size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;

        Entry entry;
        entry.key = fromUtf8(line.substr(0, tab));
        entry.value = fromUtf8(line.substr(tab + 1));
        if (!entry.key.empty())
            m_entries.push_back(std::move(entry));
    }
}

void Settings::save() const
{
    std::string data;
    for (const auto& entry : m_entries) {
        data += sanitize(toUtf8(entry.key));
        data += '\t';
        data += sanitize(toUtf8(entry.value));
        data += '\n';
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, settingsPath().c_str(), L"wb") != 0 || !file)
        return;
    std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
}

std::wstring Settings::get(const std::wstring& key, const std::wstring& defaultValue) const
{
    for (const auto& entry : m_entries) {
        if (entry.key == key)
            return entry.value;
    }
    return defaultValue;
}

int Settings::getInt(const std::wstring& key, int defaultValue) const
{
    for (const auto& entry : m_entries) {
        if (entry.key == key) {
            try {
                return std::stoi(toUtf8(entry.value));
            } catch (...) {
                return defaultValue;
            }
        }
    }
    return defaultValue;
}

bool Settings::getBool(const std::wstring& key, bool defaultValue) const
{
    for (const auto& entry : m_entries) {
        if (entry.key == key)
            return entry.value == L"1" || entry.value == L"true";
    }
    return defaultValue;
}

void Settings::set(const std::wstring& key, const std::wstring& value)
{
    for (auto& entry : m_entries) {
        if (entry.key == key) {
            entry.value = value;
            save();
            return;
        }
    }
    m_entries.push_back(Entry { key, value });
    save();
}

void Settings::setBool(const std::wstring& key, bool value)
{
    set(key, std::wstring(value ? L"1" : L"0"));
}

void Settings::setInt(const std::wstring& key, int value)
{
    set(key, std::to_wstring(value));
}

} // namespace lumen

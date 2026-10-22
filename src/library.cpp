#include "library.h"

#include "util.h"

#include <windows.h>

#include <cstdio>

namespace lumen {

namespace {

constexpr size_t kMaximumHistory = 500;

std::wstring dataDirectory()
{
    wchar_t buffer[MAX_PATH * 2] { };
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));

    std::wstring directory = (length > 0 && length < std::size(buffer))
        ? std::wstring(buffer, length)
        : executableDirectory();
    directory += L"\\Lumen";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

std::wstring sanitize(const std::wstring& value)
{
    std::wstring result = value;
    for (auto& ch : result) {
        if (ch == L'\t' || ch == L'\r' || ch == L'\n')
            ch = L' ';
    }
    return result;
}

} // namespace

std::wstring Library::pathFor(Kind kind)
{
    const wchar_t* name = L"history.tsv";
    if (kind == Kind::Bookmarks)
        name = L"bookmarks.tsv";
    else if (kind == Kind::Reading)
        name = L"reading-list.tsv";
    return dataDirectory() + L"\\" + name;
}

std::vector<LibraryEntry> Library::read(Kind kind)
{
    std::vector<LibraryEntry> entries;

    FILE* file = nullptr;
    if (_wfopen_s(&file, pathFor(kind).c_str(), L"rb") != 0 || !file)
        return entries;

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

        LibraryEntry entry;
        entry.title = fromUtf8(line.substr(0, tab));
        entry.url = fromUtf8(line.substr(tab + 1));
        if (!entry.url.empty())
            entries.push_back(std::move(entry));
    }

    return entries;
}

const std::vector<LibraryEntry>& Library::listFor(Kind kind) const
{
    if (kind == Kind::Bookmarks)
        return m_bookmarks;
    if (kind == Kind::Reading)
        return m_reading;
    return m_history;
}

void Library::write(Kind kind) const
{
    std::string data;
    for (const auto& entry : listFor(kind)) {
        data += toUtf8(sanitize(entry.title));
        data += '\t';
        data += toUtf8(sanitize(entry.url));
        data += '\n';
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, pathFor(kind).c_str(), L"wb") != 0 || !file)
        return;
    std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
}

void Library::load()
{
    m_bookmarks = read(Kind::Bookmarks);
    m_reading = read(Kind::Reading);
    m_history = read(Kind::History);
}

bool Library::visit(const std::wstring& url, const std::wstring& title)
{
    if (url.empty() || url == L"about:blank")
        return false;

    if (!m_history.empty() && m_history.front().url == url) {
        if (title.empty() || m_history.front().title == title)
            return false;
        m_history.front().title = title;
        write(Kind::History);
        return true;
    }

    m_history.insert(m_history.begin(), LibraryEntry { title, url });
    if (m_history.size() > kMaximumHistory)
        m_history.resize(kMaximumHistory);
    write(Kind::History);
    return true;
}

bool Library::addBookmark(const std::wstring& url, const std::wstring& title)
{
    for (const auto& entry : m_bookmarks) {
        if (entry.url == url)
            return false;
    }
    m_bookmarks.insert(m_bookmarks.begin(), LibraryEntry { title, url });
    write(Kind::Bookmarks);
    return true;
}

bool Library::addToReadingList(const std::wstring& url, const std::wstring& title)
{
    for (const auto& entry : m_reading) {
        if (entry.url == url)
            return false;
    }
    m_reading.insert(m_reading.begin(), LibraryEntry { title, url });
    write(Kind::Reading);
    return true;
}

void Library::removeBookmark(size_t index)
{
    if (index >= m_bookmarks.size())
        return;
    m_bookmarks.erase(m_bookmarks.begin() + static_cast<std::ptrdiff_t>(index));
    write(Kind::Bookmarks);
}

void Library::removeFromReadingList(size_t index)
{
    if (index >= m_reading.size())
        return;
    m_reading.erase(m_reading.begin() + static_cast<std::ptrdiff_t>(index));
    write(Kind::Reading);
}

void Library::removeFromHistory(size_t index)
{
    if (index >= m_history.size())
        return;
    m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(index));
    write(Kind::History);
}

void Library::clearHistory()
{
    m_history.clear();
    write(Kind::History);
}

} // namespace lumen

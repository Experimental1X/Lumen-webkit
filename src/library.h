#pragma once

#include <string>
#include <vector>

namespace lumen {

struct LibraryEntry {
    std::wstring title;
    std::wstring url;
};

// Bookmarks, Reading List and History, stored as small UTF-8 text files in
// %APPDATA%\Lumen so they survive restarts.
class Library {
public:
    void load();

    const std::vector<LibraryEntry>& bookmarks() const { return m_bookmarks; }
    const std::vector<LibraryEntry>& readingList() const { return m_reading; }
    const std::vector<LibraryEntry>& history() const { return m_history; }

    // Returns true when something changed (the caller may need to repaint).
    bool visit(const std::wstring& url, const std::wstring& title);
    bool addBookmark(const std::wstring& url, const std::wstring& title);
    bool addToReadingList(const std::wstring& url, const std::wstring& title);
    void removeBookmark(size_t index);
    void removeFromReadingList(size_t index);
    void removeFromHistory(size_t index);
    void clearHistory();

private:
    enum class Kind { Bookmarks, Reading, History };

    static std::wstring pathFor(Kind kind);
    static std::vector<LibraryEntry> read(Kind kind);
    void write(Kind kind) const;
    const std::vector<LibraryEntry>& listFor(Kind kind) const;

    std::vector<LibraryEntry> m_bookmarks;
    std::vector<LibraryEntry> m_reading;
    std::vector<LibraryEntry> m_history;
};

} // namespace lumen

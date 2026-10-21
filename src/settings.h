#pragma once

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace lumen {

// Custom message posted to the Browser main window when a setting changes
// via the Settings UI (lumen:settings URL handler). The Browser re-renders
// the active Settings tab so the new value shows up immediately.
#define WM_LUMEN_SETTINGS_CHANGED (WM_APP + 8)

// Lightweight key/value store, persisted at %APPDATA%\Lumen\settings.tsv
// so the browser survives restarts. Values are stored as wide strings; use
// the typed accessors (`getBool`, `getInt`) when a value isn't plain text.
//
// The setters write through to disk immediately, so any change in the
// Settings UI is durable without the caller having to remember to save.
class Settings {
public:
    static Settings& instance();

    void load();
    void save() const;

    std::wstring get(const std::wstring& key, const std::wstring& defaultValue) const;
    int getInt(const std::wstring& key, int defaultValue) const;
    bool getBool(const std::wstring& key, bool defaultValue) const;

    void set(const std::wstring& key, const std::wstring& value);
    void setBool(const std::wstring& key, bool value);
    void setInt(const std::wstring& key, int value);

    // Iterate over (key, value) pairs for the Settings UI.
    struct Entry { std::wstring key; std::wstring value; };
    const std::vector<Entry>& entries() const { return m_entries; }

private:
    std::vector<Entry> m_entries;
};

// Known setting keys. Use these constants instead of literal strings so
// renames stay mechanical.
namespace settingKey {
    constexpr auto kHomepage              = L"homepage";
    constexpr auto kSearchEngine          = L"search_engine";
    constexpr auto kDownloadsFolder       = L"downloads_folder";
    constexpr auto kPageZoom              = L"page_zoom";            // percent
    constexpr auto kTheme                 = L"theme";                // system | light | dark
    constexpr auto kBlockTrackers         = L"block_trackers";       // 0/1
    constexpr auto kBlockThirdPartyCookies= L"block_3p_cookies";     // 0/1
    constexpr auto kJavaScript            = L"javascript";           // 0/1
    constexpr auto kWebGL                 = L"webgl";                // 0/1
    constexpr auto kHardwareAccel         = L"hardware_accel";       // 0/1 (LUMEN_GPU)
    constexpr auto kShowFullUrl           = L"show_full_url";        // 0/1
    constexpr auto kDefaultEncoding       = L"default_encoding";     // utf-8 / iso-8859-1 / windows-1252 / shift_jis
    constexpr auto kSearchHighlights      = L"search_highlights";    // 0/1
    constexpr auto kShowSidebarOnStartup  = L"sidebar_on_startup";   // 0/1
    constexpr auto kHomepageInNewTab      = L"newtab_homepage";      // 0/1
}

} // namespace lumen

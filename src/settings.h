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

// Posted by the "Clear History and Website Data" button. The Library is owned
// by Browser, so the work has to be marshalled onto the UI thread that owns it.
#define WM_LUMEN_SETTINGS_CLEAR_HISTORY (WM_APP + 9)

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

    // Transient UI state: which pane of the Settings window is on screen.
    // Deliberately not persisted - it exists only so a `lumen:settings?view=x`
    // link clicked inside the page can be carried back to the browser, which
    // re-renders the page on the other side.
    const std::wstring& view() const { return m_view; }
    void setView(std::wstring value) { m_view = std::move(value); }

private:
    std::vector<Entry> m_entries;
    std::wstring m_view;
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
    constexpr auto kSearchSuggestions     = L"search_suggestions";   // 0/1
    constexpr auto kShowSidebarOnStartup  = L"sidebar_on_startup";   // 0/1
    constexpr auto kHomepageInNewTab      = L"newtab_homepage";      // 0/1

    // --- Safari's General / Tabs panes ---
    constexpr auto kRestoreTabs           = L"restore_tabs";         // 0/1
    constexpr auto kOpenLinksInTabs       = L"open_links_in_tabs";   // 0/1
    constexpr auto kNewTabsOpenWith       = L"newtab_open_with";     // homepage | blank
    constexpr auto kAskWhereToSave        = L"ask_where_to_save";    // 0/1
    constexpr auto kOpenSafeFiles         = L"open_safe_files";      // 0/1
    constexpr auto kSeparateTabWindows    = L"tabs_separate_window"; // 0/1
    constexpr auto kConfirmClosingTabs    = L"confirm_closing_tabs"; // 0/1
    constexpr auto kWarnMultipleTabs      = L"warn_multiple_tabs";   // 0/1

    // --- Safari's Privacy & Security pane ---
    constexpr auto kHideIpFromTrackers    = L"hide_ip_trackers";     // 0/1
    constexpr auto kFraudulentWarning     = L"warn_fraudulent";     // 0/1
    constexpr auto kClearOnExit           = L"clear_on_exit";        // 0/1

    // --- Safari's Advanced pane ---
    constexpr auto kShowDevelopMenu       = L"dev_menu";             // 0/1
    constexpr auto kWebDeveloperFeatures = L"dev_features";         // 0/1
    constexpr auto kCheckSpelling         = L"check_spelling";       // 0/1
}

} // namespace lumen

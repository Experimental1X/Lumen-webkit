#pragma once

#include "settings.h"
#include "util.h"

#include <algorithm>
#include <string>
#include <vector>

namespace lumen {

namespace {

inline std::string escapeSettingHtml(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '<':  result += "&lt;";  break;
        case '>':  result += "&gt;";  break;
        case '&':  result += "&amp;"; break;
        case '"':  result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default:   result += c;      break;
        }
    }
    return result;
}

} // namespace

// Safari Settings window reimplemented as a single HTML page (left category
// sidebar + right detail pane). Each category links to a
// `lumen:settings?view=<id>` URL so a click triggers a re-render with the
// other pane shown. Within a pane, every interactive control links to
// `lumen:settings?key=<key>&value=<value>` (or `?key=__action__&value=...`
// for one-shot buttons); `WebView::decideNavigationAction` catches those and
// routes them through the Settings store, with a custom message marshalling
// the re-render back onto the UI thread.
//
// Apple-only rows (iCloud, Apple Pay, Apple ID, Screen Time, "Show in Dock",
// macOS-specific startup controls) are intentionally absent because their
// behaviour relies on macOS services. Windows has no equivalent integration
// at the taskbar/shell level, per the user's standing rule.
inline std::string settingsPageHtml(bool dark)
{
    Settings& store = Settings::instance();

    // Which pane to render on the right. Defaults to General the way Safari
    // opens its settings window.
    std::wstring view = store.view();
    if (view.empty())
        view = L"general";

    // ----- read all values up front, formatted for the page -----
    const std::wstring homepage        = store.get(settingKey::kHomepage,            L"lumen:start");
    const std::wstring downloadsFolder = store.get(settingKey::kDownloadsFolder,     L"");
    const int          pageZoom        = store.getInt(settingKey::kPageZoom,         100);
    const std::string  engine          = toUtf8(store.get(settingKey::kSearchEngine, L"google"));
    const std::string  encoding        = toUtf8(store.get(settingKey::kDefaultEncoding, L"utf-8"));
    const std::string  newTabsOpenWith = toUtf8(store.get(settingKey::kNewTabsOpenWith, L"homepage"));

    const bool restoreTabs        = store.getBool(settingKey::kRestoreTabs,            true);
    const bool openLinksInTabs    = store.getBool(settingKey::kOpenLinksInTabs,        true);
    const bool askWhereToSave     = store.getBool(settingKey::kAskWhereToSave,         false);
    const bool openSafeFiles      = store.getBool(settingKey::kOpenSafeFiles,          false);
    const bool separateTabWindows = store.getBool(settingKey::kSeparateTabWindows,     false);
    const bool confirmClosingTabs = store.getBool(settingKey::kConfirmClosingTabs,     true);
    const bool warnMultipleTabs   = store.getBool(settingKey::kWarnMultipleTabs,       false);

    const bool blockTrackers      = store.getBool(settingKey::kBlockTrackers,          true);
    const bool block3pCookies     = store.getBool(settingKey::kBlockThirdPartyCookies, false);
    const bool hideIp             = store.getBool(settingKey::kHideIpFromTrackers,     false);
    const bool warnFraud          = store.getBool(settingKey::kFraudulentWarning,      true);
    const bool clearOnExit        = store.getBool(settingKey::kClearOnExit,            false);

    const bool searchHl           = store.getBool(settingKey::kSearchHighlights,    true);
    const bool searchSugg         = store.getBool(settingKey::kSearchSuggestions,   true);
    const bool sidebarOnStart     = store.getBool(settingKey::kShowSidebarOnStartup,false);
    const bool homepageInTab      = store.getBool(settingKey::kHomepageInNewTab,    true);
    const bool javascript         = store.getBool(settingKey::kJavaScript,          true);
    const bool webgl              = store.getBool(settingKey::kWebGL,               true);
    const bool hardwareAccel      = store.getBool(settingKey::kHardwareAccel,       false);
    const bool showFullUrl        = store.getBool(settingKey::kShowFullUrl,         true);
    const bool devMenu            = store.getBool(settingKey::kShowDevelopMenu,     false);
    const bool devFeatures        = store.getBool(settingKey::kWebDeveloperFeatures,false);
    const bool checkSpelling      = store.getBool(settingKey::kCheckSpelling,        false);

    // Map any non-ASCII text up here so the lambdas can take std::string.
    const std::string viewUtf8          = toUtf8(view);
    const std::string homepageUtf8      = toUtf8(homepage);
    const std::string downloadsFolderUtf = toUtf8(downloadsFolder);
    const std::string pageZoomStr       = std::to_string(pageZoom);

    auto setValue = [](const wchar_t* key, const std::string& raw) -> std::string {
        return std::string("lumen:settings?key=") + toUtf8(key) + "&value=" + escapeSettingHtml(raw);
    };
    auto setView = [](const char* id) -> std::string {
        return std::string("lumen:settings?view=") + id;
    };

    // Every control navigates with location.href rather than submitting a form.
    // Form submission to a custom scheme is not guaranteed to reach the
    // navigation delegate on this WebKit build, but an href navigation always
    // does, so that is the path the lumen: handler is wired to.
    auto navScript = [](const wchar_t* key) {
        return "location.href='lumen:settings?key=" + toUtf8(key) + "&value='+encodeURIComponent(this.value)";
    };

    // ----- row/group/popup/checkbox helpers -----
    auto checkbox = [&](const wchar_t* key, bool on) -> std::string {
        const std::string url = setValue(key, on ? "0" : "1");
        return "<a class=\"chk " + std::string(on ? "on" : "off") + "\" href=\"" + url + "\"></a>";
    };

    auto popup = [&](const wchar_t* key, const std::string& current,
                     const std::vector<std::pair<std::string, std::string>>& opts) -> std::string {
        std::string out = "<div class=\"sel\"><select onchange=\"" + navScript(key) + "\">";
        for (const auto& [value, label] : opts) {
            out += "<option value=\"" + escapeSettingHtml(value) + "\"";
            if (value == current)
                out += " selected";
            out += ">" + escapeSettingHtml(label) + "</option>";
        }
        out += "</select></div>";
        return out;
    };

    auto textInput = [&](const wchar_t* key, const std::string& value, const char* placeholder) -> std::string {
        return "<div class=\"txt\"><input type=\"text\" value=\"" + escapeSettingHtml(value)
             + "\" placeholder=\"" + placeholder + "\" onchange=\"" + navScript(key)
             + "\" onkeydown=\"if(event.keyCode===13)this.blur()\"></div>";
    };

    auto button = [&](const std::string& label, const char* actionValue) -> std::string {
        const std::string url = std::string("lumen:settings?key=__action__&value=") + actionValue;
        return "<a class=\"btn\" href=\"" + url + "\">" + escapeSettingHtml(label) + "</a>";
    };

    auto groupTitle = [](const std::string& t) -> std::string {
        return "<div class=\"grp\">" + escapeSettingHtml(t) + "</div>";
    };

    // Build one row: <label> ... </label> ... <control ...>
    auto row = [&](const std::string& name, const std::string& desc, const std::string& control) -> std::string {
        std::string out = "<div class=\"row\">";
        out += "<div class=\"lbl\"><div class=\"nm\">" + escapeSettingHtml(name) + "</div>";
        if (!desc.empty())
            out += "<div class=\"ds\">" + escapeSettingHtml(desc) + "</div>";
        out += "</div>";
        out += "<div class=\"ctrl\">" + control + "</div>";
        out += "</div>";
        return out;
    };

    auto group = [&](const std::string& title, const std::string& body) -> std::string {
        std::string out = "<section class=\"panel\">";
        out += groupTitle(title);
        out += body;
        out += "</section>";
        return out;
    };

    // ----- sidebar (categories) -----
    // Safari 26 / macOS Tahoe dropped the coloured rounded-square emoji tiles
    // of the Ventura-Sonoma era for monochrome SF-Symbol-style glyphs tinted by
    // the row state. Emoji cannot be forced monochrome, so the icons are drawn
    // as inline SVG on a 16x16 grid: one stroke weight, round caps, no fills.
    auto icon = [](const char* body) {
        return std::string("<svg class='ic' viewBox='0 0 16 16' fill='none' stroke='currentColor'"
                           " stroke-width='1.45' stroke-linecap='round' stroke-linejoin='round'>")
             + body + "</svg>";
    };

    // (id, label, icon svg)
    struct Cat { const char* id; const char* label; std::string icon; };
    const Cat sidebar[] = {
        { "general", "General",
            icon("<circle cx='8' cy='8' r='4.85'/><circle cx='8' cy='8' r='2.15'/>"
                 "<path d='M8 1.15v1.9M8 12.95v1.9M1.15 8h1.9M12.95 8h1.9'/>"
                 "<path d='M3.24 3.24l1.35 1.35M11.41 11.41l1.35 1.35"
                 "M12.76 3.24l-1.35 1.35M4.59 11.41l-1.35 1.35'/>") },
        { "tabs", "Tabs",
            icon("<rect x='1.9' y='2.9' width='12.2' height='10.2' rx='1.9'/>"
                 "<path d='M1.9 6.3h12.2'/>") },
        { "privacy", "Privacy & Security",
            icon("<path d='M8 1.5 13.1 3.4v4.3c0 3.3-2.15 5.9-5.1 6.9'"
                     " -2.95-1-5.1-3.6-5.1-6.9V3.4z'/>") },
        { "search", "Search",
            icon("<circle cx='6.9' cy='6.9' r='4.45'/><path d='M10.2 10.2 14 14'/>") },
        { "passwords", "Passwords",
            icon("<circle cx='5.6' cy='5.6' r='2.9'/><path d='M7.75 7.75 13.4 13.4'/>"
                 "<path d='M10.4 10.4l1.5-1.5M12.1 12.1l1.4-1.4'/>") },
        { "autofill", "AutoFill",
            icon("<rect x='1.7' y='3.7' width='12.6' height='8.6' rx='1.9'/>"
                 "<path d='M1.7 6.7h12.6'/><path d='M4.2 9.6h3.2'/>") },
        { "extensions", "Extensions",
            icon("<path d='M6.6 2.2h2.8v1.9h2.2a1 1 0 0 1 1 1v2.2h1.9v2.8h-1.9v2.2'"
                     "a1 1 0 0 1-1 1H9.4v1.9H6.6v-1.9H4.4a1 1 0 0 1-1-1V10.1H1.5'"
                     "V7.3h1.9V5.1a1 1 0 0 1 1-1h2.2z'/>") },
        { "engines", "Search Engines",
            icon("<circle cx='11.3' cy='11.3' r='2.7'/><path d='M13.4 13.4 15 15'/>"
                 "<path d='M2.2 3.4h8M2.2 6.8h5.5M2.2 10.2h3.4'/>") },
        { "advanced", "Advanced",
            icon("<path d='M1.8 4.9h12.4M1.8 11.1h12.4'/>"
                 "<circle cx='5.7' cy='4.9' r='2'/><circle cx='10.4' cy='11.1' r='2'/>") },
        { "websites", "Websites",
            icon("<circle cx='8' cy='8' r='6.05'/><path d='M1.95 8h12.1'/>"
                 "<path d='M8 1.95c1.75 2.05 2.7 4 2.7 6.05S9.75 11.95 8 14.05'/>"
                 "<path d='M8 1.95C6.25 4 5.3 5.95 5.3 8S6.25 11.95 8 14.05'/>") },
        { "controllers", "Game Controllers",
            icon("<path d='M4.5 5.4h7a3.5 3.5 0 0 1 3.45 4.4l-.42 1.9'"
                     "a1.75 1.75 0 0 1-3.1.55L10.2 10.4H5.8l-1.23 1.85'"
                     "a1.75 1.75 0 0 1-3.1-.55l-.42-1.9A3.5 3.5 0 0 1 4.5 5.4z'/>"
                 "<path d='M4.7 7.9v1.8M3.8 8.8h1.8'/>"
                 "<circle cx='10.9' cy='8.4' r='.75'/><circle cx='12.5' cy='10' r='.75'/>") },
        { "reading", "Reading List",
            icon("<path d='M2.4 3.1h3.7a2.05 2.05 0 0 1 1.9 1.35 2.05 2.05 0 0 1 1.9-1.35'"
                     "h3.7v9.7h-3.7a1.65 1.65 0 0 0-1.9 1.25 1.65 1.65 0 0 0-1.9-1.25H2.4z'/>"
                 "<path d='M8 4.45v9.6'/>") },
        { "downloads", "Downloads",
            icon("<path d='M8 2.1v7.7M5.05 6.95 8 9.9l2.95-2.95'/>"
                 "<path d='M2.4 11.35v1.55a1.1 1.1 0 0 0 1.1 1.1h9a1.1 1.1 0 0 0 1.1-1.1v-1.55'/>") },
    };

    std::string sidebarHtml;
    sidebarHtml.reserve(4096);
    for (const auto& c : sidebar) {
        const bool sel = (c.id == viewUtf8);
        sidebarHtml += "<a class=\"cat" + std::string(sel ? " on" : "") + "\" href=\"" + setView(c.id) + "\" data-name=\"";
        sidebarHtml += c.label;
        sidebarHtml += "\">";
        sidebarHtml += c.icon;
        sidebarHtml += "<span class=\"lbl\">";
        sidebarHtml += c.label;
        sidebarHtml += "</span></a>";
    }

    // ----- pane bodies -----
    std::string body;
    body.reserve(8192);

    if (viewUtf8 == "general") {
        body += group("Settings",
            row("Homepage",
                "Opens when you launch the browser or click the home button.",
                textInput(settingKey::kHomepage, homepageUtf8, "lumen:start"))
            + row("Open homepage in new tabs",
                "New-tab windows open to your homepage instead of a blank page.",
                checkbox(settingKey::kHomepageInNewTab, homepageInTab))
            + row("New tabs open with",
                "Choose what loads every time you open a new tab.",
                popup(settingKey::kNewTabsOpenWith, newTabsOpenWith, {
                    { "homepage", "Homepage" },
                    { "blank",    "Blank page" },
                    { "last",     "Same as last tab" },
                }))
        );
        body += group("Downloads",
            row("Save files to",
                "Default download location. Leave empty to use your system Downloads folder.",
                textInput(settingKey::kDownloadsFolder, downloadsFolderUtf, "Default"))
            + row("Ask where to save each file",
                "Show a file picker before every download.",
                checkbox(settingKey::kAskWhereToSave, askWhereToSave))
            + row("Open safe files after downloading",
                "PDFs, images and other safe types open automatically when the download finishes.",
                checkbox(settingKey::kOpenSafeFiles, openSafeFiles))
        );
        body += group("Search with Lumen",
            row("Show Highlights",
                "Featured results and shortcuts at the top of the start page.",
                checkbox(settingKey::kSearchHighlights, searchHl))
            + row("Show Suggestions",
                "Suggest search queries and bookmarks as you type in the address bar.",
                checkbox(settingKey::kSearchSuggestions, searchSugg))
        );
    }

    else if (viewUtf8 == "tabs") {
        body += group("Tabs",
            row("Open tabs in a separate window",
                "New tabs take their own window instead of joining the active one.",
                checkbox(settingKey::kSeparateTabWindows, separateTabWindows))
            + row("Open links in tabs instead of windows",
                "Middle-click / Ctrl-click on a link opens a new tab.",
                checkbox(settingKey::kOpenLinksInTabs, openLinksInTabs))
            + row("Confirm before closing multiple tabs",
                "Asks you to confirm when more than one tab would close at once.",
                checkbox(settingKey::kConfirmClosingTabs, confirmClosingTabs))
            + row("Warn when opening multiple tabs at once",
                "Prompt when a page tries to open many tabs in rapid succession.",
                checkbox(settingKey::kWarnMultipleTabs, warnMultipleTabs))
        );
        body += group("Startup",
            row("Restore tabs when reopening the browser",
                "Reopen the tabs that were open the last time you closed the browser.",
                checkbox(settingKey::kRestoreTabs, restoreTabs))
        );
    }

    else if (viewUtf8 == "privacy") {
        body += group("Cross-Site Tracking",
            row("Prevent cross-site tracking",
                "Limits how third-party sites can follow you across the web.",
                checkbox(settingKey::kBlockTrackers, blockTrackers))
            + row("Hide IP address from known trackers",
                "Routes requests through a relay so trackers can't see your IP.",
                checkbox(settingKey::kHideIpFromTrackers, hideIp))
        );
        body += group("Cookies",
            row("Block all third-party cookies",
                "Cookies set by sites other than the one you're on.",
                checkbox(settingKey::kBlockThirdPartyCookies, block3pCookies))
            + row("Clear history and website data when Lumen closes",
                "Clears history, cookies and cached data automatically on exit.",
                checkbox(settingKey::kClearOnExit, clearOnExit))
        );
        body += group("Warnings",
            row("Warn about fraudulent websites",
                "Show a warning before loading a page suspected of phishing or malware.",
                checkbox(settingKey::kFraudulentWarning, warnFraud))
        );
        body += group("Browsing Data",
            row("Clear browsing history…",
                "Wipe the local history stored on this computer.",
                button("Clear History", "clear_history"))
        );
    }

    else if (viewUtf8 == "search") {
        body += group("Default Search Engine",
            row("Search engine",
                "Used by the address bar and the start page when you type plain words.",
                popup(settingKey::kSearchEngine, engine, {
                    { "google",     "Google" },
                    { "bing",       "Bing" },
                    { "duckduckgo", "DuckDuckGo" },
                    { "yahoo",      "Yahoo" },
                    { "ecosia",     "Ecosia" },
                    { "startpage",  "Startpage" },
                }))
        );
        body += group("Search Experience",
            row("Show Highlights",
                "Featured results and shortcuts at the top of the start page.",
                checkbox(settingKey::kSearchHighlights, searchHl))
            + row("Show Suggestions",
                "Suggest search queries and bookmarks as you type in the address bar.",
                checkbox(settingKey::kSearchSuggestions, searchSugg))
        );
    }

    else if (viewUtf8 == "passwords") {
        body += "<section class=\"panel\"><div class=\"grp\">Passwords</div>"
                "<div class=\"empty\"><div class=\"big\">Passwords are not stored in Lumen.</div>"
                "<div class=\"ds\">This build does not include a credential store or keychain. "
                "You will be asked to type credentials on every visit.</div></div></section>";
    }

    else if (viewUtf8 == "autofill") {
        body += "<section class=\"panel\"><div class=\"grp\">AutoFill</div>"
                "<div class=\"empty\"><div class=\"big\">AutoFill is not supported in Lumen.</div>"
                "<div class=\"ds\">Saved passwords, cards and addresses are not stored in this build.</div></div></section>";
    }

    else if (viewUtf8 == "extensions") {
        body += "<section class=\"panel\"><div class=\"grp\">Extensions</div>"
                "<div class=\"empty\"><div class=\"big\">No extensions installed</div>"
                "<div class=\"ds\">Lumen does not support extensions in this build. "
                "When support is added, installed extensions will appear here and you can grant "
                "them access per-site.</div></div></section>";
    }

    else if (viewUtf8 == "engines") {
        body += group("Website Search",
            row("Use website search shortcut suggestions",
                "Show a list of website-specific searches when the address bar matches.",
                checkbox(settingKey::kSearchSuggestions, searchSugg))
        );
        body += "<section class=\"panel\"><div class=\"grp\">Manage Search Engines</div>"
                "<div class=\"empty\"><div class=\"big\">Manage search engines…</div>"
                "<div class=\"ds\">The engine list is fixed to the six providers in Search this "
                "release. Custom engines will appear here in a future update.</div></div></section>";
    }

    else if (viewUtf8 == "advanced") {
        body += group("Accessibility",
            row("Check spelling while typing",
                "Underline misspelled words in the address bar and input fields.",
                checkbox(settingKey::kCheckSpelling, checkSpelling))
        );
        body += group("Smart Search Field",
            row("Show full website address",
                "Always show https:// and www. in the address bar.",
                checkbox(settingKey::kShowFullUrl, showFullUrl))
        );
        body += group("Web Content",
            row("Default page zoom",
                "Zoom level used by pages that don't ask for one.",
                popup(settingKey::kPageZoom, pageZoomStr, {
                    { "50",  "50%"  }, { "67",  "67%"  }, { "75", "75%" }, { "90", "90%" },
                    { "100", "100%" }, { "110", "110%" }, { "125", "125%" }, { "150", "150%" },
                    { "200", "200%" },
                }))
            + row("Default text encoding",
                "Used for pages that don't declare one.",
                popup(settingKey::kDefaultEncoding, encoding, {
                    { "utf-8",        "UTF-8" },
                    { "iso-8859-1",   "ISO 8859-1 (Latin 1)" },
                    { "windows-1252", "Windows-1252" },
                    { "shift_jis",    "Shift-JIS" },
                }))
            + row("JavaScript",
                "Required for almost every modern site.",
                checkbox(settingKey::kJavaScript, javascript))
            + row("WebGL",
                "Hardware-accelerated 3D rendering inside pages.",
                checkbox(settingKey::kWebGL, webgl))
            + row("Hardware acceleration",
                "Off uses CPU rendering. Slower, but avoids some GPU glitches.",
                checkbox(settingKey::kHardwareAccel, hardwareAccel))
        );
        body += group("Develop",
            row("Show Develop menu and toolbar",
                "Adds a Develop menu with web-developer tools (Inspect, etc.).",
                checkbox(settingKey::kShowDevelopMenu, devMenu))
            + row("Enable web-developer features",
                "Turns on DOM Inspector and other developer surfaces.",
                checkbox(settingKey::kWebDeveloperFeatures, devFeatures))
        );
    }

    else if (viewUtf8 == "websites") {
        body += group("Per-Site Permissions",
            row("Camera, microphone, location",
                "Default prompt used when a site asks for access.",
                "<span class=\"static\">Ask per site</span>")
            + row("Pop-ups",
                "Default behaviour when a page tries to open a new window.",
                "<span class=\"static\">Block pop-ups</span>")
            + row("Downloads",
                "Whether a site can trigger a download without confirmation.",
                "<span class=\"static\">Ask per site</span>")
            + row("Notifications",
                "Whether a site can show desktop notifications.",
                "<span class=\"static\">Ask per site</span>")
        );
    }

    else if (viewUtf8 == "controllers") {
        body += "<section class=\"panel\"><div class=\"grp\">Game Controllers</div>"
                "<div class=\"empty\"><div class=\"big\">Game controllers are not configured.</div>"
                "<div class=\"ds\">Lumen does not currently map gamepad buttons to page actions. "
                "Connect a controller and games will detect it via the standard WebHID API.</div></div></section>";
    }

    else if (viewUtf8 == "reading") {
        body += group("Reading List",
            row("Show Reading List in the sidebar",
                "The Reading List pane is reached from the sidebar (Ctrl+Shift+D).",
                "<span class=\"static\">Always available</span>")
        );
        body += "<section class=\"panel\"><div class=\"empty\">"
                "<div class=\"ds\">Reading list entries are stored locally in "
                "<code>%APPDATA%\\Lumen\\reading.tsv</code>.</div></div></section>";
    }

    else if (viewUtf8 == "downloads") {
        body += group("Downloads",
            row("Save files to",
                "Where downloads land by default.",
                textInput(settingKey::kDownloadsFolder, downloadsFolderUtf, "Default"))
            + row("Ask where to save each file",
                "Show a file picker before every download.",
                checkbox(settingKey::kAskWhereToSave, askWhereToSave))
            + row("Open safe files after downloading",
                "PDFs, images and other safe types open automatically when the download finishes.",
                checkbox(settingKey::kOpenSafeFiles, openSafeFiles))
        );
    }

    // ----- assemble the page -----
    std::string html;
    html.reserve(16384);

    html += "<!doctype html>\n<html class=\"";
    html += dark ? "dark" : "light";
    html += "\">\n<head>\n<meta charset=\"utf-8\">\n";
    html += "<title>Settings</title>\n<style>\n";

    // Liquid Glass (macOS Tahoe 26 / Safari 26): sidebars and content float
    // as separate translucent, generously rounded panels inset from the window
    // edge, rather than sitting flush against it. Controls are capsules with
    // a soft border. -webkit-backdrop-filter is prefixed because this is the
    // WinCairo WebKit build.
    html +=
        "html,body{margin:0;height:100%}\n"
        "html.light{--bg:#e9eaee;--side:rgba(250,250,252,.72);--card:rgba(255,255,255,.78);"
        "--text:#1d1d1f;--sub:#6e6e73;--border:rgba(0,0,0,.10);--hair:rgba(0,0,0,.07);"
        "--accent:#007aff;--accentText:#fff;--hover:rgba(0,0,0,.055);--field:rgba(255,255,255,.9);"
        "--shadow:0 10px 34px rgba(0,0,0,.13);color-scheme:light}\n"
        "html.dark{--bg:#101012;--side:rgba(42,42,47,.72);--card:rgba(38,38,43,.78);"
        "--text:#f5f5f7;--sub:#98989d;--border:rgba(255,255,255,.13);--hair:rgba(255,255,255,.08);"
        "--accent:#0a84ff;--accentText:#fff;--hover:rgba(255,255,255,.08);--field:rgba(30,30,34,.85);"
        "--shadow:0 10px 34px rgba(0,0,0,.5);color-scheme:dark}\n"
        "*{box-sizing:border-box}\n"
        "body{font:13px/1.45 'Segoe UI Variable Text','Segoe UI',system-ui,sans-serif;"
        "color:var(--text);background:var(--bg);display:flex;overflow:hidden;"
        "padding:10px;gap:10px}\n"
        "#side,#main{background:var(--side);border:1px solid var(--border);border-radius:18px;"
        "box-shadow:var(--shadow);-webkit-backdrop-filter:blur(26px) saturate(180%);"
        "backdrop-filter:blur(26px) saturate(180%)}\n"
        "#side{width:246px;flex:0 0 246px;display:flex;flex-direction:column;overflow:hidden}\n"
        "#filter{padding:12px 12px 8px}\n"
        "#filter input{width:100%;border:1px solid var(--border);border-radius:9px;"
        "background:var(--field);color:var(--text);padding:5px 10px;font:inherit;outline:none}\n"
        "#filter input:focus{border-color:var(--accent)}\n"
        "#list{flex:1 1 auto;overflow:auto;padding:2px 8px 14px}\n"
        ".cat{display:flex;align-items:center;gap:11px;padding:6px 9px;border-radius:9px;"
        "text-decoration:none;color:var(--text);font-size:13px;margin:0 0 1px;"
        "-webkit-user-drag:none}\n"
        ".cat:hover{background:var(--hover)}\n"
        ".cat.on{background:var(--accent);color:var(--accentText)}\n"
        ".cat .ic{width:17px;height:17px;flex:0 0 17px;display:block}\n"
        ".cat .lbl{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}\n"
        "#main{flex:1 1 auto;overflow:auto;background:var(--card)}\n"
        "#main .inner{max-width:640px;margin:0 auto;padding:30px 32px 64px}\n"
        ".heading{margin:0 0 22px;font-size:21px;font-weight:700;letter-spacing:-.01em}\n"
        ".panel{background:var(--field);border:1px solid var(--border);border-radius:14px;"
        "overflow:hidden;margin-bottom:20px}\n"
        ".grp{font-size:11px;font-weight:700;color:var(--sub);text-transform:uppercase;"
        "letter-spacing:.06em;padding:0 2px 7px}\n"
        ".row{display:flex;align-items:center;justify-content:space-between;gap:18px;"
        "padding:10px 14px;border-top:1px solid var(--hair)}\n"
        ".panel .row:first-of-type{border-top:none}\n"
        ".lbl{flex:1 1 auto;min-width:0}\n"
        ".lbl .nm{font-weight:500}\n"
        ".lbl .ds{display:block;color:var(--sub);font-size:12px;margin-top:2px;line-height:1.35}\n"
        ".ctrl{flex:0 0 auto;display:flex;align-items:center;justify-content:flex-end}\n"
        ".chk{width:16px;height:16px;border-radius:5px;display:inline-block;position:relative;"
        "flex:0 0 16px;text-decoration:none;border:1px solid var(--border);background:var(--field);"
        "transition:background-color .12s ease,border-color .12s ease}\n"
        ".chk:hover{border-color:var(--accent)}\n"
        ".chk.on{background:var(--accent);border-color:var(--accent)}\n"
        ".chk.on::after{content:\"\";position:absolute;left:4px;top:1px;width:5px;height:9px;"
        "border:solid #fff;border-width:0 2px 2px 0;transform:rotate(43deg)}\n"
        ".sel select{font:inherit;color:var(--text);background:var(--field);border:1px solid var(--border);"
        "border-radius:9px;padding:5px 28px 5px 12px;min-width:180px;outline:none;"
        "appearance:none;-webkit-appearance:none;"
        "background-image:url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' "
        "viewBox='0 0 12 12'><path d='M2 4.4l4 3.8 4-3.8' fill='none' stroke='%238e8e93' "
        "stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'/></svg>\");"
        "background-repeat:no-repeat;background-position:right 10px center;background-size:11px}\n"
        ".sel select:focus{border-color:var(--accent)}\n"
        ".txt input{font:inherit;color:var(--text);background:var(--field);border:1px solid var(--border);"
        "border-radius:9px;padding:5px 11px;width:240px;outline:none}\n"
        ".txt input:focus{border-color:var(--accent)}\n"
        ".btn{display:inline-block;font:inherit;color:var(--text);background:var(--field);"
        "border:1px solid var(--border);border-radius:9px;padding:5px 14px;text-decoration:none;"
        "white-space:nowrap}\n"
        ".btn:hover{border-color:var(--accent);color:var(--accent)}\n"
        ".empty{padding:18px 16px 16px}\n"
        ".empty .big{font-weight:600;margin-bottom:3px}\n"
        ".empty .ds{color:var(--sub);font-size:12px;line-height:1.45}\n"
        ".static{color:var(--sub);font-size:12px}\n"
        "code{font-family:'Cascadia Code',Consolas,monospace;font-size:12px;"
        "background:var(--field);padding:1px 5px;border-radius:4px;border:1px solid var(--border)}\n";

    html += "</style>\n</head>\n<body>\n";

    // Sidebar
    html += "<div id=\"side\"><div id=\"filter\"><input id=\"q\" placeholder=\"Search settings\" "
            "oninput=\"filterCats()\"></div><div id=\"list\">";
    html += sidebarHtml;
    html += "</div></div>";

    // Main pane (the selected detail panel is rendered into here)
    html += "<div id=\"main\"><div class=\"inner\">";
    html += "<h1 class=\"heading\">";

    if      (viewUtf8 == "general")     html += "General";
    else if (viewUtf8 == "tabs")        html += "Tabs";
    else if (viewUtf8 == "privacy")     html += "Privacy &amp; Security";
    else if (viewUtf8 == "search")      html += "Search";
    else if (viewUtf8 == "passwords")   html += "Passwords";
    else if (viewUtf8 == "autofill")    html += "AutoFill";
    else if (viewUtf8 == "extensions")  html += "Extensions";
    else if (viewUtf8 == "engines")     html += "Search Engines";
    else if (viewUtf8 == "advanced")    html += "Advanced";
    else if (viewUtf8 == "websites")    html += "Websites";
    else if (viewUtf8 == "controllers") html += "Game Controllers";
    else if (viewUtf8 == "reading")     html += "Reading List";
    else if (viewUtf8 == "downloads")   html += "Downloads";
    else                                html += "Settings";

    html += "</h1>";
    html += body;
    html += "</div></div>\n";

    // Tiny inline JS: filter the sidebar as the user types in the search box.
    html += "<script>\nfunction filterCats(){var q=document.getElementById('q').value.toLowerCase();"
            "var rows=document.querySelectorAll('#list .cat');"
            "for(var i=0;i<rows.length;i++){var n=rows[i].getAttribute('data-name').toLowerCase();"
            "rows[i].style.display=(!q||n.indexOf(q)>=0)?'flex':'none';}}\n</script>\n";

    html += "</body>\n</html>\n";
    return html;
}

} // namespace lumen

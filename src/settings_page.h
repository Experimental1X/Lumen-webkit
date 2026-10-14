#pragma once

#include "settings.h"
#include "util.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace lumen {

namespace {

inline std::string escapeHtml(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '&': result += "&amp;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::wstring settingValue(const wchar_t* key, const std::wstring& defaultValue)
{
    return Settings::instance().get(key, defaultValue);
}

std::string selectedEngine()
{
    const std::wstring v = settingValue(settingKey::kSearchEngine, L"google");
    return toUtf8(v);
}

std::string selectedTheme()
{
    const std::wstring v = settingValue(settingKey::kTheme, L"system");
    return toUtf8(v);
}

std::string selectedEncoding()
{
    return toUtf8(settingValue(settingKey::kDefaultEncoding, L"utf-8"));
}

} // namespace

// Self-contained Settings page in the same visual language as the start page
// and the downloads page. Each interactive control links to a
// `lumen:settings?key=X&value=Y` URL; `WebView::decideNavigationAction`
// catches those, calls `Settings::set`, and the browser re-renders the page.
inline std::string settingsPageHtml(bool dark)
{
    const Settings& s = Settings::instance();

    // Pull values once, formatted for the page.
    const std::wstring homepage       = s.get(settingKey::kHomepage, L"lumen:start");
    const std::wstring downloadsFolder= s.get(settingKey::kDownloadsFolder, L"");
    const int          pageZoom       = s.getInt(settingKey::kPageZoom, 100);
    const bool         blockTrackers  = s.getBool(settingKey::kBlockTrackers, true);
    const bool         block3pCookies = s.getBool(settingKey::kBlockThirdPartyCookies, false);
    const bool         javascript     = s.getBool(settingKey::kJavaScript, true);
    const bool         webgl          = s.getBool(settingKey::kWebGL, true);
    const bool         hardwareAccel  = s.getBool(settingKey::kHardwareAccel, false);
    const bool         showFullUrl    = s.getBool(settingKey::kShowFullUrl, true);
    const bool         searchHl       = s.getBool(settingKey::kSearchHighlights, true);
    const bool         sidebarOnStart = s.getBool(settingKey::kShowSidebarOnStartup, false);
    const bool         homepageInTab  = s.getBool(settingKey::kHomepageInNewTab, true);

    const std::string engine   = selectedEngine();
    const std::string theme    = selectedTheme();
    const std::string encoding = selectedEncoding();

    auto pillToggle = [&](const wchar_t* key, bool on) {
        return std::string("<a class=\"toggle ") + (on ? "on" : "off") +
               "\" href=\"lumen:settings?key=" + toUtf8(key) + "&value=" + (on ? "0" : "1") + "\">" +
               (on ? "On" : "Off") + "</a>";
    };

    auto radioRow = [&](const char* group, const std::string& value, const char* label, bool selected, const char* optValue) {
        std::string url = std::string("lumen:settings?key=") + group + "&value=" + optValue;
        std::string out = "<a class=\"radio";
        if (selected) out += " sel";
        out += "\" href=\"" + url + "\">";
        out += "<span class=\"dot\"></span><span>" + std::string(label) + "</span></a>";
        (void)value;
        return out;
    };

    auto selectRow = [&](const wchar_t* key, const std::string& current, const std::vector<std::pair<std::string, std::string>>& options) {
        std::string out = "<div class=\"select\"><form action=\"lumen:settings\" method=\"get\">";
        out += "<input type=\"hidden\" name=\"key\" value=\"" + toUtf8(key) + "\">";
        out += "<select name=\"value\" onchange=\"this.form.submit()\">";
        for (const auto& [value, label] : options) {
            out += "<option value=\"" + value + "\"";
            if (value == current) out += " selected";
            out += ">" + label + "</option>";
        }
        out += "</select></form></div>";
        return out;
    };

    auto textRow = [&](const wchar_t* key, const std::wstring& value, const char* placeholder) {
        std::string out = "<div class=\"text\"><form action=\"lumen:settings\" method=\"get\">";
        out += "<input type=\"hidden\" name=\"key\" value=\"" + toUtf8(key) + "\">";
        out += "<input type=\"text\" name=\"value\" value=\"" + escapeHtml(toUtf8(value)) +
               "\" placeholder=\"" + placeholder + "\" onchange=\"this.form.submit()\">";
        out += "</form></div>";
        return out;
    };

    auto folderRow = [&](const wchar_t* key, const std::wstring& value) {
        std::string out = "<div class=\"text\"><form action=\"lumen:settings\" method=\"get\">";
        out += "<input type=\"hidden\" name=\"key\" value=\"" + toUtf8(key) + "\">";
        out += "<input type=\"text\" name=\"value\" value=\"" + escapeHtml(toUtf8(value)) +
               "\" placeholder=\"Default (Windows Downloads folder)\" onchange=\"this.form.submit()\">";
        out += "</form></div>";
        return out;
    };

    std::string html;
    html.reserve(8192);
    html += "<!doctype html>\n<html class=\"";
    html += dark ? "dark" : "light";
    html += "\">\n<head>\n<meta charset=\"utf-8\">\n";
    html += "<title>Settings</title>\n<style>\n";

    html +=
        "html.light{--bg1:#f4f5f8;--bg2:#e4e7ee;--card:#fff;--text:#1d1d1f;"
        "--sub:#6e6e73;--border:#d6d6dc;--accent:#007aff;--danger:#ff3b30;"
        "--track:#e9e9ee;color-scheme:light}\n"
        "html.dark{--bg1:#2b2c32;--bg2:#16171b;--card:#3a3a3f;--text:#f5f5f7;"
        "--sub:#9a9aa1;--border:#54545a;--accent:#0a84ff;--danger:#ff453a;"
        "--track:#404045;color-scheme:dark}\n"
        "*{box-sizing:border-box}\n"
        "html,body{margin:0;min-height:100%}\n"
        "body{font:13px/1.4 'Segoe UI Variable Text','Segoe UI',system-ui,sans-serif;"
        "color:var(--text);background:linear-gradient(180deg,var(--bg1),var(--bg2));"
        "background-attachment:fixed}\n"
        ".page{max-width:780px;margin:0 auto;padding:32px 24px 80px}\n"
        "h1{margin:0 0 6px;font-size:24px;font-weight:700;letter-spacing:-.01em}\n"
        ".lede{margin:0 0 24px;color:var(--sub);font-size:13px}\n"
        ".section{background:var(--card);border:1px solid var(--border);border-radius:12px;"
        "padding:6px 18px;margin-bottom:18px}\n"
        ".section h2{margin:14px 0 4px;font-size:13px;font-weight:600;color:var(--sub);"
        "text-transform:uppercase;letter-spacing:.04em}\n"
        ".row{display:flex;align-items:center;justify-content:space-between;padding:11px 0;"
        "border-top:1px solid var(--border);gap:14px}\n"
        ".row:first-of-type{border-top:none}\n"
        ".label{flex:1 1 auto;min-width:0}\n"
        ".label .name{font-weight:500}\n"
        ".label .desc{display:block;color:var(--sub);font-size:12px;margin-top:2px}\n"
        ".toggle,.radio,a.action{display:inline-flex;align-items:center;justify-content:center;"
        "min-width:54px;height:26px;border-radius:999px;text-decoration:none;font-weight:600;"
        "font-size:12px;padding:0 14px;cursor:pointer;border:1px solid transparent}\n"
        ".toggle.on,.radio.sel{background:var(--accent);color:#fff}\n"
        ".toggle.off,.radio{background:var(--track);color:var(--text)}\n"
        ".radio{display:inline-flex;justify-content:flex-start;gap:8px;padding:0 12px;min-width:120px;height:30px}\n"
        ".radio .dot{width:8px;height:8px;border-radius:50%;background:var(--card);border:1px solid var(--border);margin-right:2px;flex:0 0 auto}\n"
        ".radio.sel .dot{background:var(--accent);border-color:var(--accent)}\n"
        ".select select,.text input{font:inherit;color:var(--text);background:var(--bg1);"
        "border:1px solid var(--border);border-radius:8px;padding:6px 10px;min-width:200px}\n"
        ".text input{width:260px}\n"
        ".danger{background:transparent;color:var(--danger);border-color:var(--danger)}\n"
        ".danger:hover{background:var(--danger);color:#fff}\n"
        ".action{background:var(--bg1);color:var(--text);border-color:var(--border)}\n"
        ".grid{display:grid;grid-template-columns:1fr 1fr;gap:6px 14px}\n";

    html += "</style>\n</head>\n<body>\n<div class=\"page\">\n";
    html += "<h1>Settings</h1>\n";
    html += "<p class=\"lede\">Configure Lumen. Changes save immediately and apply to new tabs.</p>\n";

    // General
    html += "<div class=\"section\"><h2>General</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Homepage</div>"
            "<span class=\"desc\">Opens when you launch the browser or click Home.</span></div>";
    html += textRow(settingKey::kHomepage, homepage, "lumen:start");
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Open homepage in new tabs</div>"
            "<span class=\"desc\">When you open a new tab.</span></div>";
    html += pillToggle(settingKey::kHomepageInNewTab, homepageInTab);
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Downloads folder</div>"
            "<span class=\"desc\">Where saved files go by default.</span></div>";
    html += folderRow(settingKey::kDownloadsFolder, downloadsFolder);
    html += "</div></div>";

    // Appearance
    html += "<div class=\"section\"><h2>Appearance</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Theme</div>"
            "<span class=\"desc\">Follow the system, or pin to light or dark.</span></div>";
    html += "<div class=\"grid\">";
    html += radioRow("theme", theme, "System", theme == "system", "system");
    html += radioRow("theme", theme, "Light",  theme == "light",  "light");
    html += radioRow("theme", theme, "Dark",   theme == "dark",   "dark");
    html += "</div></div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Default page zoom</div>"
            "<span class=\"desc\">%</span></div>";
    {
        std::vector<std::pair<std::string, std::string>> opts {
            {"50", "50%"}, {"67", "67%"}, {"75", "75%"}, {"90", "90%"},
            {"100", "100%"}, {"110", "110%"}, {"125", "125%"}, {"150", "150%"}, {"200", "200%"}
        };
        std::ostringstream zoomOss;
        zoomOss << pageZoom;
        for (auto& o : opts) {
            if (o.first == zoomOss.str()) o.second = o.first + "%  (current)";
        }
        html += selectRow(settingKey::kPageZoom, zoomOss.str(), opts);
    }
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Default text encoding</div>"
            "<span class=\"desc\">For pages that don't declare one.</span></div>";
    {
        std::vector<std::pair<std::string, std::string>> opts {
            {"utf-8", "UTF-8"}, {"iso-8859-1", "ISO 8859-1 (Latin 1)"},
            {"windows-1252", "Windows-1252"}, {"shift_jis", "Shift-JIS"}
        };
        html += selectRow(settingKey::kDefaultEncoding, encoding, opts);
    }
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Show sidebar on startup</div>"
            "<span class=\"desc\">Open the bookmarks/history panel when the browser launches.</span></div>";
    html += pillToggle(settingKey::kShowSidebarOnStartup, sidebarOnStart);
    html += "</div></div>";

    // Privacy & Security
    html += "<div class=\"section\"><h2>Privacy &amp; Security</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Prevent cross-site tracking</div>"
            "<span class=\"desc\">Limits how third-party sites can follow you across the web.</span></div>";
    html += pillToggle(settingKey::kBlockTrackers, blockTrackers);
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Block third-party cookies</div>"
            "<span class=\"desc\">Cookies set by sites other than the one you're on.</span></div>";
    html += pillToggle(settingKey::kBlockThirdPartyCookies, block3pCookies);
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Clear browsing history</div>"
            "<span class=\"desc\">Wipe the local history sidebar.</span></div>";
    html += "<a class=\"action danger\" href=\"lumen:settings?key=__action__&value=clear_history\">Clear</a>";
    html += "</div></div>";

    // Websites (placeholder)
    html += "<div class=\"section\"><h2>Websites</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Camera, microphone, location</div>"
            "<span class=\"desc\">Use the prompt defaults set here for every site that asks.</span></div>";
    html += "<span class=\"desc\" style=\"color:var(--sub)\">Ask per site</span></div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Pop-ups</div>"
            "<span class=\"desc\">Default behaviour when a page opens a new window.</span></div>";
    html += "<span class=\"desc\" style=\"color:var(--sub)\">Block pop-ups</span></div></div>";

    // Extensions (placeholder)
    html += "<div class=\"section\"><h2>Extensions</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">No extensions installed</div>"
            "<span class=\"desc\">Lumen does not support extensions in this build.</span></div></div></div>";

    // Search
    html += "<div class=\"section\"><h2>Search</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Default search engine</div>"
            "<span class=\"desc\">Used by the address bar when you type plain words.</span></div>";
    {
        std::vector<std::pair<std::string, std::string>> opts {
            {"google", "Google"}, {"bing", "Bing"}, {"duckduckgo", "DuckDuckGo"},
            {"yahoo", "Yahoo"}, {"ecosia", "Ecosia"}, {"startpage", "Startpage"}
        };
        html += selectRow(settingKey::kSearchEngine, engine, opts);
    }
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Search highlights</div>"
            "<span class=\"desc\">Show suggestions and shortcuts on the start page.</span></div>";
    html += pillToggle(settingKey::kSearchHighlights, searchHl);
    html += "</div></div>";

    // AutoFill (placeholder)
    html += "<div class=\"section\"><h2>AutoFill</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">AutoFill is not supported in Lumen</div>"
            "<span class=\"desc\">Saved passwords, cards and addresses are not stored in this build.</span></div></div></div>";

    // Profiles (placeholder)
    html += "<div class=\"section\"><h2>Profiles</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">No profiles</div>"
            "<span class=\"desc\">Lumen uses a single set of settings for now.</span></div></div></div>";

    // Advanced
    html += "<div class=\"section\"><h2>Advanced</h2>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Show full website address</div>"
            "<span class=\"desc\">Always show https://www in the address bar.</span></div>";
    html += pillToggle(settingKey::kShowFullUrl, showFullUrl);
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">JavaScript</div>"
            "<span class=\"desc\">Required for almost every modern site.</span></div>";
    html += pillToggle(settingKey::kJavaScript, javascript);
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">WebGL</div>"
            "<span class=\"desc\">Hardware-accelerated 3D rendering inside pages.</span></div>";
    html += pillToggle(settingKey::kWebGL, webgl);
    html += "</div>";
    html += "<div class=\"row\"><div class=\"label\"><div class=\"name\">Hardware acceleration</div>"
            "<span class=\"desc\">Off (CPU rendering) avoids some GPU glitches but is slower.</span></div>";
    html += pillToggle(settingKey::kHardwareAccel, hardwareAccel);
    html += "</div></div>";

    html += "</div>\n</body>\n</html>\n";
    return html;
}

} // namespace lumen

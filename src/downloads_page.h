#pragma once

#include "downloads.h"
#include "util.h"

#include <algorithm>
#include <string>

namespace lumen {

// escapeHtml lives in util.h so both downloads_page.h and settings_page.h
// can include it without colliding in the same translation unit.

namespace {

// A self-contained HTML page listing every download with live progress, in the
// same visual style as the start page. The browser pushes an updated HTML
// snapshot through WebView::loadHtml whenever Downloads::onChanged fires, so
// the page does not need to self-reload. The action buttons link to
// lumen:cancel?id=N, lumen:open?id=N, etc.; those are caught by
// WebView::decideNavigationAction and dispatched there.
inline std::string downloadsPageHtml(bool dark)
{
    const std::vector<DownloadItem>& items = Downloads::instance().items();

    std::string html;
    html.reserve(8192);

    html += "<!doctype html>\n<html class=\"";
    html += dark ? "dark" : "light";
    html += "\">\n<head>\n<meta charset=\"utf-8\">\n";
    html += "<title>Downloads</title>\n<style>\n";

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
        ".page{max-width:880px;margin:0 auto;padding:32px 32px 48px}\n"
        "header{display:flex;align-items:center;justify-content:space-between;margin-bottom:20px}\n"
        "h1{margin:0;font-size:22px;font-weight:700;letter-spacing:-.01em}\n"
        ".clear{color:var(--accent);text-decoration:none;font-size:13px;font-weight:500}\n"
        ".clear:hover{text-decoration:underline}\n"
        ".empty{text-align:center;padding:80px 0;color:var(--sub);font-size:14px}\n"
        ".item{background:var(--card);border:1px solid var(--border);border-radius:10px;"
        "padding:14px 18px;margin-bottom:12px;display:flex;align-items:flex-start;gap:14px}\n"
        ".info{flex:1;min-width:0}\n"
        ".name{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
        ".status{color:var(--sub);font-size:12px;margin-top:4px}\n"
        ".source{color:var(--sub);font-size:11px;margin-top:2px;overflow:hidden;"
        "text-overflow:ellipsis;white-space:nowrap}\n"
        ".bar{height:4px;background:var(--track);border-radius:2px;margin-top:10px;overflow:hidden}\n"
        ".fill{height:100%;background:var(--accent);border-radius:2px;transition:width .3s}\n"
        ".pulse{width:33%;margin:0 auto;opacity:.7}\n"
        ".actions{display:flex;flex-direction:column;gap:6px;min-width:32px}\n"
        ".icon{display:inline-flex;align-items:center;justify-content:center;"
        "width:30px;height:30px;border-radius:6px;color:var(--sub);text-decoration:none;"
        "background:var(--track);font-size:14px}\n"
        ".icon:hover{color:var(--text);background:var(--bg1)}\n"
        ".icon.danger{color:var(--danger)}\n";

    html += "</style>\n</head>\n<body>\n<div class=\"page\">\n";
    html += "<header><h1>Downloads</h1>";
    html += "<a class=\"clear\" href=\"lumen:clear\">Clear Finished</a>";
    html += "</header>\n";

    if (items.empty()) {
        html += "<div class=\"empty\">No downloads yet.</div>\n";
    } else {
        for (const auto& item : items) {
            html += "<div class=\"item\"><div class=\"info\">";
            html += "<div class=\"name\">" + escapeHtml(toUtf8(item.filename)) + "</div>";

            std::wstring status;
            switch (item.state) {
            case DownloadItem::State::Active:
                if (item.total > 0)
                    status = formatBytes(item.received) + L" of " + formatBytes(item.total);
                else if (item.received > 0)
                    status = formatBytes(item.received) + L" — Downloading";
                else
                    status = L"Starting…";
                break;
            case DownloadItem::State::Finished:
                status = formatBytes(item.total > 0 ? item.total : item.received);
                if (!item.source.empty())
                    status += L" — " + item.source;
                break;
            case DownloadItem::State::Failed:
                status = L"Download failed";
                break;
            case DownloadItem::State::Cancelled:
                status = L"Cancelled";
                break;
            }
            html += "<div class=\"status\">" + escapeHtml(toUtf8(status)) + "</div>";

            if (!item.source.empty() && item.state != DownloadItem::State::Finished) {
                html += "<div class=\"source\">" + escapeHtml(toUtf8(item.source)) + "</div>";
            }

            if (item.state == DownloadItem::State::Active) {
                html += "<div class=\"bar\">";
                if (item.total > 0) {
                    const double fraction = std::clamp(static_cast<double>(item.received) / static_cast<double>(item.total), 0.0, 1.0);
                    const int pct = static_cast<int>(fraction * 100);
                    html += "<div class=\"fill\" style=\"width:" + std::to_string(pct) + "%\"></div>";
                } else {
                    html += "<div class=\"fill pulse\"></div>";
                }
                html += "</div>";
            }

            html += "</div>";

            const std::string idStr = std::to_string(item.id);
            html += "<div class=\"actions\">";
            switch (item.state) {
            case DownloadItem::State::Active:
                html += "<a class=\"icon danger\" href=\"lumen:cancel?id=" + idStr + "\" title=\"Cancel\">\xC3\x97</a>";
                break;
            case DownloadItem::State::Finished:
                html += "<a class=\"icon\" href=\"lumen:open?id=" + idStr + "\" title=\"Open\">\xE2\x96\xB6</a>";
                html += "<a class=\"icon\" href=\"lumen:reveal?id=" + idStr + "\" title=\"Show in folder\">\xE2\x86\x97</a>";
                break;
            default:
                html += "<a class=\"icon danger\" href=\"lumen:remove?id=" + idStr + "\" title=\"Remove\">\xC3\x97</a>";
                break;
            }
            html += "</div></div>\n";
        }
    }

    html += "</div>\n</body>\n</html>\n";
    return html;
}

} // namespace lumen
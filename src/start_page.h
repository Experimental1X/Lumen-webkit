#pragma once

#include <string>

namespace lumen {

// The page shown in a new tab: a grid of Favorites, in the style of Safari's
// Start Page. `dark` picks the colors; the browser passes the system theme.
inline std::string startPageHtml(bool dark)
{
    struct Favorite {
        const char* name;
        const char* url;
        char letter;
        int hue;
    };

    static const Favorite favorites[] = {
        { "Apple", "https://www.apple.com", 'A', 210 },
        { "Google", "https://www.google.com", 'G', 8 },
        { "YouTube", "https://www.youtube.com", 'Y', 0 },
        { "Wikipedia", "https://www.wikipedia.org", 'W', 220 },
        { "GitHub", "https://github.com", 'G', 250 },
        { "Reddit", "https://www.reddit.com", 'R', 18 },
        { "LinkedIn", "https://www.linkedin.com", 'L', 205 },
        { "Netflix", "https://www.netflix.com", 'N', 355 },
    };

    std::string html = R"HTML(<!doctype html>
<html class=")HTML";
    html += dark ? "dark" : "light";
    html += R"HTML(">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Start Page</title>
<style>
html.light{--bg1:#f4f5f8;--bg2:#e4e7ee;--text:#1d1d1f;--sub:#6e6e73;color-scheme:light}
html.dark{--bg1:#2b2c32;--bg2:#16171b;--text:#f5f5f7;--sub:#9a9aa1;color-scheme:dark}
*{box-sizing:border-box}
html,body{margin:0;min-height:100%}
body{font:13px/1.35 "Segoe UI Variable Text","Segoe UI",system-ui,sans-serif;color:var(--text);
 background:
  radial-gradient(1100px 640px at 18% -8%,rgba(76,201,176,.22),transparent 62%),
  radial-gradient(900px 560px at 92% 0%,rgba(110,120,230,.18),transparent 58%),
  linear-gradient(180deg,var(--bg1),var(--bg2));
 background-attachment:fixed}
.page{max-width:880px;margin:0 auto;padding:76px 32px 48px}
h2{margin:0 0 20px;font-size:17px;font-weight:700;letter-spacing:-.01em}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(96px,1fr));gap:24px 14px}
.fav{display:flex;flex-direction:column;align-items:center;gap:9px;text-decoration:none;color:var(--text)}
.icon{width:68px;height:68px;border-radius:16px;display:grid;place-items:center;color:#fff;font-size:28px;font-weight:600;
 box-shadow:0 1px 2px rgba(0,0,0,.18),0 6px 16px rgba(0,0,0,.12)}
.label{max-width:96px;text-align:center;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:12px}
.foot{margin-top:64px;text-align:center;color:var(--sub);font-size:11px;letter-spacing:.18em;text-transform:uppercase}
</style>
</head>
<body>
<div class="page">
<h2>Favorites</h2>
<div class="grid">)HTML";

    for (const auto& favorite : favorites) {
        html += "<a class=\"fav\" href=\"";
        html += favorite.url;
        html += "\"><div class=\"icon\" style=\"background:hsl(";
        html += std::to_string(favorite.hue);
        html += ",52%,46%)\">";
        html += favorite.letter;
        html += "</div><div class=\"label\">";
        html += favorite.name;
        html += "</div></a>";
    }

    html += R"HTML(</div>
<div class="foot">Lumen</div>
</div>
</body>
</html>
)HTML";
    return html;
}

} // namespace lumen

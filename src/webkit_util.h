#pragma once

// Small conversions between WebKit strings/URLs and std::wstring.

#include "util.h"
#include "webkit_ref.h"

#include <WebKit/WKString.h>
#include <WebKit/WKURL.h>

#include <string>

namespace lumen {

inline std::wstring toWide(WKStringRef string)
{
    if (!string)
        return { };

    const size_t maximumSize = WKStringGetMaximumUTF8CStringSize(string);
    if (!maximumSize)
        return { };

    std::string utf8(maximumSize, '\0');
    const size_t written = WKStringGetUTF8CString(string, utf8.data(), maximumSize);
    // The returned count includes the terminating NUL.
    utf8.resize(written > 0 ? written - 1 : 0);
    return fromUtf8(utf8);
}

inline std::wstring toWide(WKURLRef url)
{
    if (!url)
        return { };

    auto string = adopt(WKURLCopyString(url));
    return toWide(string.get());
}

} // namespace lumen

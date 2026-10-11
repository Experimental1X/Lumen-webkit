#pragma once

// A minimal owning handle for WebKit C API objects.
//
// WebKit ships WKRetainPtr.h for this, but it includes WebKit's internal WTF
// headers, which are not part of the SDK. This header only needs WKRelease.

#include <WebKit/WKBase.h>
#include <WebKit/WKType.h>

#include <utility>

namespace lumen {

template<typename T>
class Ref {
public:
    Ref() = default;

    // Takes ownership of an object returned by a WK...Create or WK...Copy
    // function (a +1 reference).
    explicit Ref(T value)
        : m_value(value)
    {
    }

    Ref(Ref&& other) noexcept
        : m_value(std::exchange(other.m_value, nullptr))
    {
    }

    Ref& operator=(Ref&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_value = std::exchange(other.m_value, nullptr);
        }
        return *this;
    }

    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;

    ~Ref() { reset(); }

    void reset()
    {
        if (m_value)
            WKRelease(m_value);
        m_value = nullptr;
    }

    T get() const { return m_value; }
    explicit operator bool() const { return m_value != nullptr; }

private:
    T m_value { nullptr };
};

template<typename T>
Ref<T> adopt(T value)
{
    return Ref<T>(value);
}

} // namespace lumen

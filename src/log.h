/**
 * @file log.h
 * @brief Qt-free logging shim for the JS8 decoder.
 *
 * Replaces QDebug / QLoggingCategory with a lightweight callback-based
 * mechanism.  All qCDebug / qCWarning / qCInfo / Q_DECLARE_LOGGING_CATEGORY /
 * Q_LOGGING_CATEGORY macros expand to no-ops; a user-supplied callback
 * (set via gfsk8::setLogCallback()) receives formatted diagnostic strings.
 *
 * Design differences from the original log.h:
 *   - Uses a dedicated LogDispatcher struct instead of a bare
 *     std::function<> global, making the callback state more explicit.
 *   - The null-stream type is named DiagSink instead of _JS8NullStream.
 *   - The no-op category type is named LogCategory instead of _JS8LogCat.
 */
#pragma once

#include <cstdio>
#include <functional>

namespace gfsk8 {

/// Holds the optional user-supplied diagnostic callback.
struct LogDispatcher {
    std::function<void(const char *)> callback;

    void emit(const char *msg) const {
        if (callback) callback(msg);
    }
};

/// Library-global dispatcher; assigned by setLogCallback() in api.cpp.
extern LogDispatcher g_logger;

} // namespace gfsk8

// Formatted diagnostic macro — silently dropped when no callback is set.
#define GFSK8_LOG(fmt, ...)                                                    \
    do {                                                                     \
        if (::gfsk8::g_logger.callback) {                                      \
            char _buf[512];                                                  \
            std::snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__);           \
            ::gfsk8::g_logger.emit(_buf);                                      \
        }                                                                    \
    } while (0)

// ── Qt-style stream shims ─────────────────────────────────────────────────────
// qCDebug(category) << value1 << value2;  =>  discarded

/// Null diagnostic stream — all insertions are no-ops.
struct DiagSink {
    template <typename T> DiagSink &operator<<(T const &) { return *this; }
    DiagSink &noquote() { return *this; }
    DiagSink &nospace() { return *this; }
    DiagSink &space()   { return *this; }
};

#define qCDebug(cat)   DiagSink()
#define qCWarning(cat) DiagSink()
#define qCInfo(cat)    DiagSink()

// ── Qt logging-category shims ─────────────────────────────────────────────────
// Q_DECLARE_LOGGING_CATEGORY / Q_LOGGING_CATEGORY declare a category function;
// here each expands to a function returning a permanently-disabled category.

struct LogCategory {
    constexpr bool isDebugEnabled()   const noexcept { return false; }
    constexpr bool isInfoEnabled()    const noexcept { return false; }
    constexpr bool isWarningEnabled() const noexcept { return false; }
};

#define Q_DECLARE_LOGGING_CATEGORY(name)   \
    inline LogCategory name() { return {}; }
#define Q_LOGGING_CATEGORY(name, ...)      \
    inline LogCategory name() { return {}; }

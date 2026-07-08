#pragma once

#include <cstdlib>
#include <optional>
#include <string>

// Process-environment helpers shared by the unit and integration trees (tests
// run single-threaded; the environment is process-global). One copy here
// instead of one per test file.
namespace tctest {

/// Set (non-null value) or unset (nullptr) an environment variable in THIS
/// process.
inline void set_env(const char* key, const char* value) {
#if defined(_WIN32)
    ::_putenv_s(key, value ? value : ""); // empty value removes it
#else
    if (value) {
        ::setenv(key, value, /*overwrite*/ 1);
    } else {
        ::unsetenv(key);
    }
#endif
}

/// Save/set/restore an environment variable for a scope. A nullopt value
/// clears it.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::optional<std::string>& value) : name_(name) {
        if (const char* prev = std::getenv(name)) {
            saved_ = prev;
        }
        apply(value);
    }
    ~ScopedEnv() { apply(saved_); }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void apply(const std::optional<std::string>& value) {
        set_env(name_.c_str(), value ? value->c_str() : nullptr);
    }

    std::string name_;
    std::optional<std::string> saved_;
};

} // namespace tctest

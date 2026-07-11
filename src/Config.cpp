#include "Config.hpp"

#include "Env.hpp"
#include "FileRead.hpp"
#include "Strings.hpp"

#include <cstdlib>
#include <mutex>
#include <sstream>
#include <utility>

namespace testcontainers::detail {

std::map<std::string, std::string> parse_properties(const std::string& body) {
    std::map<std::string, std::string> out;
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == '!') {
            continue; // blank or comment line ('#' and '!' both start comments)
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(trimmed.substr(0, eq));
        if (key.empty()) {
            continue;
        }
        out[std::move(key)] = trim(trimmed.substr(eq + 1)); // last duplicate wins
    }
    return out;
}

namespace {

std::mutex g_properties_mutex;
// optional distinguishes "not read yet" from "read, file absent/empty".
std::optional<std::map<std::string, std::string>> g_properties_cache;

std::map<std::string, std::string> load_user_properties() {
    const std::string home = home_dir();
    if (home.empty()) {
        return {};
    }
    return parse_properties(read_file(home + "/.testcontainers.properties"));
}

} // namespace

std::optional<std::string> user_property(const std::string& key) {
    const std::lock_guard<std::mutex> lock(g_properties_mutex);
    if (!g_properties_cache) {
        g_properties_cache = load_user_properties();
    }
    const auto it = g_properties_cache->find(key);
    if (it == g_properties_cache->end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second;
}

void clear_user_properties_cache() {
    const std::lock_guard<std::mutex> lock(g_properties_mutex);
    g_properties_cache.reset();
}

std::optional<std::string> config_value(const char* env_name, const std::string& properties_key) {
    if (const char* v = std::getenv(env_name); v && *v) {
        return std::string(v);
    }
    return user_property(properties_key);
}

bool config_truthy(const char* env_name, const std::string& properties_key) {
    if (const char* v = std::getenv(env_name); v && *v) {
        return env_truthy(env_name);
    }
    const auto prop = user_property(properties_key);
    return prop && to_lower(*prop) == "true";
}

} // namespace testcontainers::detail

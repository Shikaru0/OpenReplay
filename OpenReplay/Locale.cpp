#include "Locale.h"
#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include "json.hpp"

using json = nlohmann::json;

static json g_langJson;
static std::unordered_map<std::string, std::string> g_stringCache;

const char* tr(const char* key) {
    if (!g_langJson.contains(key))
        return key;
    auto it = g_stringCache.find(key);
    if (it != g_stringCache.end())
        return it->second.c_str();
    auto ins = g_stringCache.emplace(key, g_langJson[key].get<std::string>());
    return ins.first->second.c_str();
}

void loadLocale(const char* path) {
    std::ifstream lf(path);
    if (lf.is_open()) {
        try { g_langJson = json::parse(lf); } catch (const std::exception& e) {
            std::cerr << "[Locale] Failed to parse locale: " << e.what() << "\n";
        }
    }
    g_stringCache.clear();
}

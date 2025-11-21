// usersettings.cpp. new handler of user settings. they are in exedir/usersettings.json

#include "usersettings.h"
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <cctype>

std::string UserSettings::controlSchemeToString(ControlScheme s) {
    switch (s) {
    case ControlScheme::Blender: return "blender";
    case ControlScheme::Industry:
    default: return "industry";
    }
}

ControlScheme UserSettings::controlSchemeFromString(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });
    if (lower.find("blender") != std::string::npos) return ControlScheme::Blender;
    return ControlScheme::Industry;
}

static std::string defaultSettingsPath() {
    std::filesystem::path p = std::filesystem::current_path();
    p /= "usersettings.json";
    return p.string();
}

bool UserSettings::load() {
    if (filePath.empty()) filePath = defaultSettingsPath();
    std::ifstream in(filePath);
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = content.find("control_scheme");
    if (pos != std::string::npos) {
        size_t colon = content.find(':', pos);
        if (colon != std::string::npos) {
            size_t quote = content.find('"', colon);
            if (quote != std::string::npos) {
                size_t quote2 = content.find('"', quote + 1);
                if (quote2 != std::string::npos && quote2 > quote) {
                    std::string val = content.substr(quote + 1, quote2 - (quote + 1));
                    control = controlSchemeFromString(val);
                    return true;
                }
            }
        }
    }
    if (content.find("blender") != std::string::npos) {
        control = ControlScheme::Blender;
        return true;
    }
    return false;
}

bool UserSettings::save() {
    if (filePath.empty()) filePath = defaultSettingsPath();
    std::ofstream out(filePath, std::ios::trunc);
    if (!out) return false;
    out << "{\n  \"control_scheme\": \"" << controlSchemeToString(control) << "\"\n}\n";
    out.close();
    return true;
}

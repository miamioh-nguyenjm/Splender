#pragma once
#include <string>

enum class ControlScheme {
    Industry,
    Blender
};

struct UserSettings {
    ControlScheme control = ControlScheme::Industry;
    std::string filePath;

    bool load();
    bool save();

    static std::string controlSchemeToString(ControlScheme s);
    static ControlScheme controlSchemeFromString(const std::string& s);
};

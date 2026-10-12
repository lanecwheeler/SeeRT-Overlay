#include "Settings.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <Windows.h>    // SHGetFolderPathW / GetEnvironmentVariableW
#include <Shlobj.h>

#pragma comment(lib, "Shell32.lib")

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace Settings {

std::string DefaultPath() {
    // Prefer SHGetKnownFolderPath so we get the true APPDATA even under
    // redirected profiles.
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        std::wstring ws(roaming);
        CoTaskMemFree(roaming);
        // Convert UTF-16 → UTF-8 properly via WideCharToMultiByte.
        int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(len > 0 ? len - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, utf8.data(), len, nullptr, nullptr);
        return utf8 + "\\SeeRT Overlay\\settings.json";
    }
    return "settings.json"; // fallback to cwd
}

void VisualToJson(const CRTSettings& s, json& j) {
    j["curvatureX"]          = s.curvatureX;
    j["curvatureY"]          = s.curvatureY;
    j["zoom"]                = s.zoom;
    j["scanlineIntensity"]   = s.scanlineIntensity;
    j["scanlineCount"]       = s.scanlineCount;
    j["vignetteStrength"]    = s.vignetteStrength;
    j["glowStrength"]        = s.glowStrength;
    j["brightness"]          = s.brightness;
    j["contrast"]            = s.contrast;
    j["saturation"]          = s.saturation;
    j["flickerIntensity"]    = s.flickerIntensity;
    j["flickerSpeed"]        = s.flickerSpeed;
    j["flickerRandomness"]   = s.flickerRandomness;
    j["humBarIntensity"]     = s.humBarIntensity;
    j["humBarSpeed"]         = s.humBarSpeed;
    j["humBarFrequency"]     = s.humBarFrequency;
    j["diffuseStrength"]     = s.diffuseStrength;
    j["diffuseThreshold"]    = s.diffuseThreshold;
    j["phosphorR"]           = s.phosphorR;
    j["phosphorG"]           = s.phosphorG;
    j["phosphorB"]           = s.phosphorB;
    j["staticIntensity"]     = s.staticIntensity;
    j["staticSpeed"]         = s.staticSpeed;
    j["shadowMaskIntensity"] = s.shadowMaskIntensity;
    j["shadowMaskScale"]     = s.shadowMaskScale;
    j["shadowMaskType"]      = s.shadowMaskType;
    j["convergence"]         = s.convergence;
    j["cursorScale"]         = s.cursorScale;
    j["cursorThickness"]     = s.cursorThickness;
    j["cursorR"]             = s.cursorR;
    j["cursorG"]             = s.cursorG;
    j["cursorB"]             = s.cursorB;
    j["cursorAutoHide"]      = s.cursorAutoHide;
    j["cursorStroke"]        = s.cursorStroke;
    j["cursorPlain"]         = s.cursorPlain;
}

void VisualFromJson(const json& j, CRTSettings& s) {
    auto getF = [&](const char* key, float& field) {
        auto it = j.find(key);
        if (it != j.end() && it->is_number()) field = it->get<float>();
    };
    auto getB = [&](const char* key, bool& field) {
        auto it = j.find(key);
        if (it != j.end() && it->is_boolean()) field = it->get<bool>();
    };
    getF("curvatureX",          s.curvatureX);
    getF("curvatureY",          s.curvatureY);
    getF("zoom",                s.zoom);
    getF("scanlineIntensity",   s.scanlineIntensity);
    getF("scanlineCount",       s.scanlineCount);
    getF("vignetteStrength",    s.vignetteStrength);
    getF("glowStrength",        s.glowStrength);
    getF("brightness",          s.brightness);
    getF("contrast",            s.contrast);
    getF("saturation",          s.saturation);
    getF("flickerIntensity",    s.flickerIntensity);
    getF("flickerSpeed",        s.flickerSpeed);
    getF("flickerRandomness",   s.flickerRandomness);
    getF("humBarIntensity",     s.humBarIntensity);
    getF("humBarSpeed",         s.humBarSpeed);
    getF("humBarFrequency",     s.humBarFrequency);
    getF("diffuseStrength",     s.diffuseStrength);
    getF("diffuseThreshold",    s.diffuseThreshold);
    getF("phosphorR",           s.phosphorR);
    getF("phosphorG",           s.phosphorG);
    getF("phosphorB",           s.phosphorB);
    getF("staticIntensity",     s.staticIntensity);
    getF("staticSpeed",         s.staticSpeed);
    getF("shadowMaskIntensity", s.shadowMaskIntensity);
    getF("shadowMaskScale",     s.shadowMaskScale);
    getF("shadowMaskType",      s.shadowMaskType);
    getF("convergence",         s.convergence);
    getF("cursorScale",         s.cursorScale);
    getF("cursorThickness",     s.cursorThickness);
    getF("cursorR",             s.cursorR);
    getF("cursorG",             s.cursorG);
    getF("cursorB",             s.cursorB);
    getB("cursorAutoHide",      s.cursorAutoHide);
    getB("cursorStroke",        s.cursorStroke);
    getB("cursorPlain",         s.cursorPlain);
}

CRTSettings Load(const std::string& path) {
    CRTSettings s; // filled with member-initializer defaults
    std::ifstream f(path);
    if (!f.is_open()) return s;

    try {
        json j = json::parse(f);
        if (!j.is_object()) return s;
        VisualFromJson(j, s);
        if (j.contains("menuStyle") && j["menuStyle"].is_number_integer())
            s.menuStyle = j["menuStyle"].get<int>();
        if (j.contains("hiddenSources") && j["hiddenSources"].is_array()) {
            for (auto& item : j["hiddenSources"])
                if (item.is_string()) s.hiddenSources.push_back(item.get<std::string>());
        }
        if (j.contains("audioCaptureDeviceId") && j["audioCaptureDeviceId"].is_string())
            s.audioCaptureDeviceId = j["audioCaptureDeviceId"].get<std::string>();
        // menuVisible is intentionally NOT loaded — the panel always starts
        // closed so the overlay is click-through immediately on launch.
        if (j.contains("toggleX") && j["toggleX"].is_number()) s.toggleX = j["toggleX"].get<float>();
        if (j.contains("toggleY") && j["toggleY"].is_number()) s.toggleY = j["toggleY"].get<float>();
    } catch (...) {
        // Malformed file — silently return defaults.
    }
    return s;
}

void Save(const std::string& path, const CRTSettings& s) {
    // Ensure parent directory exists.
    fs::path p(path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    // Ignore ec — if we can't create the dir, the ofstream open will fail and
    // we just swallow it rather than crashing the render loop.

    std::ofstream f(path);
    if (!f.is_open()) return;

    json j;
    VisualToJson(s, j);
    j["menuStyle"]            = s.menuStyle;
    j["hiddenSources"]        = s.hiddenSources;
    j["audioCaptureDeviceId"] = s.audioCaptureDeviceId;
    // menuVisible NOT saved — always starts closed on next launch.
    j["toggleX"]              = s.toggleX;
    j["toggleY"]              = s.toggleY;

    f << j.dump(4);
}

} // namespace Settings

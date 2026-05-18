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

CRTSettings Load(const std::string& path) {
    CRTSettings s; // filled with member-initializer defaults
    std::ifstream f(path);
    if (!f.is_open()) return s;

    try {
        json j = json::parse(f);
        if (j.contains("curvatureX"))       s.curvatureX       = j["curvatureX"].get<float>();
        if (j.contains("curvatureY"))       s.curvatureY       = j["curvatureY"].get<float>();
        if (j.contains("zoom"))             s.zoom             = j["zoom"].get<float>();
        if (j.contains("scanlineIntensity"))s.scanlineIntensity= j["scanlineIntensity"].get<float>();
        if (j.contains("scanlineCount"))    s.scanlineCount    = j["scanlineCount"].get<float>();
        if (j.contains("vignetteStrength")) s.vignetteStrength = j["vignetteStrength"].get<float>();
        if (j.contains("glowStrength"))     s.glowStrength     = j["glowStrength"].get<float>();
        if (j.contains("brightness"))       s.brightness       = j["brightness"].get<float>();
        if (j.contains("contrast"))         s.contrast         = j["contrast"].get<float>();
        if (j.contains("saturation"))       s.saturation       = j["saturation"].get<float>();
        if (j.contains("flickerIntensity"))  s.flickerIntensity  = j["flickerIntensity"].get<float>();
        if (j.contains("flickerSpeed"))      s.flickerSpeed      = j["flickerSpeed"].get<float>();
        if (j.contains("flickerRandomness")) s.flickerRandomness = j["flickerRandomness"].get<float>();
        if (j.contains("humBarIntensity"))   s.humBarIntensity    = j["humBarIntensity"].get<float>();
        if (j.contains("humBarSpeed"))       s.humBarSpeed        = j["humBarSpeed"].get<float>();
        if (j.contains("humBarFrequency"))   s.humBarFrequency    = j["humBarFrequency"].get<float>();
        if (j.contains("diffuseStrength"))  s.diffuseStrength   = j["diffuseStrength"].get<float>();
        if (j.contains("diffuseThreshold")) s.diffuseThreshold  = j["diffuseThreshold"].get<float>();
        if (j.contains("phosphorR"))        s.phosphorR         = j["phosphorR"].get<float>();
        if (j.contains("phosphorG"))        s.phosphorG         = j["phosphorG"].get<float>();
        if (j.contains("phosphorB"))        s.phosphorB         = j["phosphorB"].get<float>();
        if (j.contains("staticIntensity"))      s.staticIntensity     = j["staticIntensity"].get<float>();
        if (j.contains("staticSpeed"))          s.staticSpeed         = j["staticSpeed"].get<float>();
        if (j.contains("shadowMaskIntensity"))  s.shadowMaskIntensity = j["shadowMaskIntensity"].get<float>();
        if (j.contains("shadowMaskScale"))      s.shadowMaskScale     = j["shadowMaskScale"].get<float>();
        if (j.contains("shadowMaskType"))       s.shadowMaskType      = j["shadowMaskType"].get<float>();
        if (j.contains("convergence"))          s.convergence         = j["convergence"].get<float>();
        if (j.contains("menuStyle"))        s.menuStyle        = j["menuStyle"].get<int>();
        if (j.contains("cursorScale"))      s.cursorScale      = j["cursorScale"].get<float>();
        if (j.contains("cursorThickness")) s.cursorThickness  = j["cursorThickness"].get<float>();
        if (j.contains("cursorR"))          s.cursorR          = j["cursorR"].get<float>();
        if (j.contains("cursorG"))          s.cursorG          = j["cursorG"].get<float>();
        if (j.contains("cursorB"))          s.cursorB          = j["cursorB"].get<float>();
        if (j.contains("cursorAutoHide"))   s.cursorAutoHide   = j["cursorAutoHide"].get<bool>();
        if (j.contains("cursorStroke"))     s.cursorStroke     = j["cursorStroke"].get<bool>();
        if (j.contains("cursorPlain"))      s.cursorPlain      = j["cursorPlain"].get<bool>();
        if (j.contains("hiddenSources") && j["hiddenSources"].is_array()) {
            for (auto& item : j["hiddenSources"])
                if (item.is_string()) s.hiddenSources.push_back(item.get<std::string>());
        }
        if (j.contains("audioCaptureDeviceId")) s.audioCaptureDeviceId = j["audioCaptureDeviceId"].get<std::string>();
        // menuVisible is intentionally NOT loaded — the panel always starts
        // closed so the overlay is click-through immediately on launch.
        if (j.contains("toggleX"))          s.toggleX          = j["toggleX"].get<float>();
        if (j.contains("toggleY"))          s.toggleY          = j["toggleY"].get<float>();
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
    j["curvatureX"]        = s.curvatureX;
    j["curvatureY"]        = s.curvatureY;
    j["zoom"]              = s.zoom;
    j["scanlineIntensity"] = s.scanlineIntensity;
    j["scanlineCount"]     = s.scanlineCount;
    j["vignetteStrength"]  = s.vignetteStrength;
    j["glowStrength"]      = s.glowStrength;
    j["brightness"]        = s.brightness;
    j["contrast"]          = s.contrast;
    j["saturation"]        = s.saturation;
    j["flickerIntensity"]   = s.flickerIntensity;
    j["flickerSpeed"]       = s.flickerSpeed;
    j["flickerRandomness"]  = s.flickerRandomness;
    j["humBarIntensity"]    = s.humBarIntensity;
    j["humBarSpeed"]        = s.humBarSpeed;
    j["humBarFrequency"]    = s.humBarFrequency;
    j["diffuseStrength"]    = s.diffuseStrength;
    j["diffuseThreshold"]   = s.diffuseThreshold;
    j["phosphorR"]          = s.phosphorR;
    j["phosphorG"]          = s.phosphorG;
    j["phosphorB"]          = s.phosphorB;
    j["staticIntensity"]     = s.staticIntensity;
    j["staticSpeed"]         = s.staticSpeed;
    j["shadowMaskIntensity"] = s.shadowMaskIntensity;
    j["shadowMaskScale"]     = s.shadowMaskScale;
    j["shadowMaskType"]      = s.shadowMaskType;
    j["convergence"]         = s.convergence;
    j["menuStyle"]         = s.menuStyle;
    j["cursorScale"]       = s.cursorScale;
    j["cursorThickness"]   = s.cursorThickness;
    j["cursorR"]           = s.cursorR;
    j["cursorG"]           = s.cursorG;
    j["cursorB"]           = s.cursorB;
    j["cursorStroke"]      = s.cursorStroke;
    j["cursorPlain"]       = s.cursorPlain;
    j["hiddenSources"]          = s.hiddenSources;
    j["audioCaptureDeviceId"]   = s.audioCaptureDeviceId;
    // menuVisible NOT saved — always starts closed on next launch.
    j["toggleX"]           = s.toggleX;
    j["toggleY"]           = s.toggleY;

    f << j.dump(4);
}

} // namespace Settings

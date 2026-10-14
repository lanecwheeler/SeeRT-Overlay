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

fs::path DataDir() {
    // Prefer SHGetKnownFolderPath so we get the true APPDATA even under
    // redirected profiles.
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        fs::path dir = fs::path(roaming) / L"SeeRT Overlay";
        CoTaskMemFree(roaming);
        return dir;
    }
    return {}; // fallback to cwd
}

fs::path DefaultPath() {
    return DataDir() / L"settings.json";
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
    j["crtGamma"]            = s.crtGamma;
    j["displayGamma"]        = s.displayGamma;
    j["beamWidth"]           = s.beamWidth;
    j["beamDynamics"]        = s.beamDynamics;
    j["lineResolve"]         = s.lineResolve;
    j["maskCompensation"]    = s.maskCompensation;
    j["cornerRadius"]        = s.cornerRadius;
    j["chromaBleed"]         = s.chromaBleed;
    j["signalSoftness"]      = s.signalSoftness;
    j["glowRadius"]          = s.glowRadius;
    j["halationRadius"]      = s.halationRadius;
    j["persistence"]         = s.persistence;
    j["rasterBloom"]         = s.rasterBloom;
    j["cursorScale"]         = s.cursorScale;
    j["cursorThickness"]     = s.cursorThickness;
    j["cursorR"]             = s.cursorR;
    j["cursorG"]             = s.cursorG;
    j["cursorB"]             = s.cursorB;
    j["cursorAutoHide"]      = s.cursorAutoHide;
    j["cursorStroke"]        = s.cursorStroke;
    j["cursorPlain"]         = s.cursorPlain;
    j["cursorShowHotspot"]   = s.cursorShowHotspot;
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
    getF("crtGamma",            s.crtGamma);
    getF("displayGamma",        s.displayGamma);
    getF("beamWidth",           s.beamWidth);
    getF("beamDynamics",        s.beamDynamics);
    getF("lineResolve",         s.lineResolve);
    getF("maskCompensation",    s.maskCompensation);
    getF("cornerRadius",        s.cornerRadius);
    getF("chromaBleed",         s.chromaBleed);
    getF("signalSoftness",      s.signalSoftness);
    getF("glowRadius",          s.glowRadius);
    getF("halationRadius",      s.halationRadius);
    getF("persistence",         s.persistence);
    getF("rasterBloom",         s.rasterBloom);
    getF("cursorScale",         s.cursorScale);
    getF("cursorThickness",     s.cursorThickness);
    getF("cursorR",             s.cursorR);
    getF("cursorG",             s.cursorG);
    getF("cursorB",             s.cursorB);
    getB("cursorAutoHide",      s.cursorAutoHide);
    getB("cursorStroke",        s.cursorStroke);
    getB("cursorPlain",         s.cursorPlain);
    getB("cursorShowHotspot",   s.cursorShowHotspot);
}

CRTSettings Load(const fs::path& path) {
    CRTSettings s; // filled with member-initializer defaults
    std::ifstream f(path);
    if (!f.is_open()) return s;

    try {
        json j = json::parse(f);
        if (!j.is_object()) return s;
        VisualFromJson(j, s);
        if (j.contains("menuStyle") && j["menuStyle"].is_number_integer()) {
            s.menuStyle = j["menuStyle"].get<int>();
            if (s.menuStyle == 2) s.menuStyle = 6;   // old N64 style → its replacement, 64 Adventure
        }
        if (j.contains("hiddenSources") && j["hiddenSources"].is_array()) {
            for (auto& item : j["hiddenSources"])
                if (item.is_string()) s.hiddenSources.push_back(item.get<std::string>());
        }
        if (j.contains("audioCaptureDeviceId") && j["audioCaptureDeviceId"].is_string())
            s.audioCaptureDeviceId = j["audioCaptureDeviceId"].get<std::string>();
        if (j.contains("customCursorPath") && j["customCursorPath"].is_string())
            s.customCursorPath = j["customCursorPath"].get<std::string>();
        if (j.contains("customCursorHotX") && j["customCursorHotX"].is_number_integer())
            s.customCursorHotX = j["customCursorHotX"].get<int>();
        if (j.contains("customCursorHotY") && j["customCursorHotY"].is_number_integer())
            s.customCursorHotY = j["customCursorHotY"].get<int>();
        // menuVisible is intentionally NOT loaded — the panel always starts
        // closed so the overlay is click-through immediately on launch.
        if (j.contains("toggleX") && j["toggleX"].is_number()) s.toggleX = j["toggleX"].get<float>();
        if (j.contains("toggleY") && j["toggleY"].is_number()) s.toggleY = j["toggleY"].get<float>();
        if (j.contains("menuScale")  && j["menuScale"].is_number())  s.menuScale  = j["menuScale"].get<float>();
        if (j.contains("menuWidth")  && j["menuWidth"].is_number())  s.menuWidth  = j["menuWidth"].get<float>();
        if (j.contains("menuHeight") && j["menuHeight"].is_number()) s.menuHeight = j["menuHeight"].get<float>();
        if (j.contains("checkForUpdates") && j["checkForUpdates"].is_boolean())
            s.checkForUpdates = j["checkForUpdates"].get<bool>();
        if (j.contains("lastNotifiedUpdate") && j["lastNotifiedUpdate"].is_string())
            s.lastNotifiedUpdate = j["lastNotifiedUpdate"].get<std::string>();
        if (j.contains("adv64RedCoins") && j["adv64RedCoins"].is_number_integer())
            s.adv64RedCoins = j["adv64RedCoins"].get<int>();
        if (j.contains("adv64Gold") && j["adv64Gold"].is_boolean())
            s.adv64Gold = j["adv64Gold"].get<bool>();
        if (j.contains("vectorHighScore") && j["vectorHighScore"].is_number_integer())
            s.vectorHighScore = j["vectorHighScore"].get<int>();
        if (j.contains("vectorHighName") && j["vectorHighName"].is_string())
            s.vectorHighName = j["vectorHighName"].get<std::string>().substr(0, 3);
        if (j.contains("arcadeHighScore") && j["arcadeHighScore"].is_number_integer())
            s.arcadeHighScore = j["arcadeHighScore"].get<int>();
        if (j.contains("streamShowMenu") && j["streamShowMenu"].is_boolean())
            s.streamShowMenu = j["streamShowMenu"].get<bool>();
        if (j.contains("webHits") && j["webHits"].is_number_integer())
            s.webHits = j["webHits"].get<int>();
        if (j.contains("guestbook") && j["guestbook"].is_array()) {
            for (const auto& e : j["guestbook"]) {
                if (!e.is_object()) continue;
                GuestbookEntry g;
                if (e.contains("name")    && e["name"].is_string())    g.name    = e["name"].get<std::string>();
                if (e.contains("message") && e["message"].is_string()) g.message = e["message"].get<std::string>();
                if (e.contains("date")    && e["date"].is_string())    g.date    = e["date"].get<std::string>();
                if (!g.name.empty() || !g.message.empty()) s.guestbook.push_back(std::move(g));
            }
        }
    } catch (...) {
        // Malformed file — silently return defaults.
    }
    return s;
}

void Save(const fs::path& path, const CRTSettings& s) {
    // Ensure parent directory exists.
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    // Ignore ec — if we can't create the dir, the ofstream open will fail and
    // we just swallow it rather than crashing the render loop.

    std::ofstream f(path);
    if (!f.is_open()) return;

    json j;
    VisualToJson(s, j);
    j["menuStyle"]            = s.menuStyle;
    j["hiddenSources"]        = s.hiddenSources;
    j["audioCaptureDeviceId"] = s.audioCaptureDeviceId;
    j["customCursorPath"]     = s.customCursorPath;
    j["customCursorHotX"]     = s.customCursorHotX;
    j["customCursorHotY"]     = s.customCursorHotY;
    // menuVisible NOT saved — always starts closed on next launch.
    j["toggleX"]              = s.toggleX;
    j["toggleY"]              = s.toggleY;
    j["menuScale"]            = s.menuScale;
    j["menuWidth"]            = s.menuWidth;
    j["menuHeight"]           = s.menuHeight;
    j["checkForUpdates"]      = s.checkForUpdates;
    j["lastNotifiedUpdate"]   = s.lastNotifiedUpdate;
    j["adv64RedCoins"]        = s.adv64RedCoins;
    j["adv64Gold"]            = s.adv64Gold;
    j["vectorHighScore"]      = s.vectorHighScore;
    j["vectorHighName"]       = s.vectorHighName;
    j["arcadeHighScore"]      = s.arcadeHighScore;
    j["streamShowMenu"]       = s.streamShowMenu;
    j["webHits"]              = s.webHits;
    j["guestbook"]            = json::array();
    for (const GuestbookEntry& g : s.guestbook)
        j["guestbook"].push_back({ { "name", g.name }, { "message", g.message }, { "date", g.date } });

    f << j.dump(4);
}

} // namespace Settings

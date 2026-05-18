#include "Presets.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <Windows.h>
#include <Shlobj.h>

#pragma comment(lib, "Shell32.lib")

using json = nlohmann::json;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Apply — copies all visual (shader + cursor) fields from src to dst,
// leaving UI/session state (menuStyle, menuVisible, toggleX/Y, hiddenSources,
// time) untouched so the overlay never flickers or loses window state when a
// preset is applied.
// ---------------------------------------------------------------------------
void Presets::Apply(CRTSettings& dst, const CRTSettings& src) {
    // Shader fields
    dst.curvatureX        = src.curvatureX;
    dst.curvatureY        = src.curvatureY;
    dst.zoom              = src.zoom;
    dst.scanlineIntensity = src.scanlineIntensity;
    dst.scanlineCount     = src.scanlineCount;
    dst.vignetteStrength  = src.vignetteStrength;
    dst.glowStrength      = src.glowStrength;
    dst.brightness        = src.brightness;
    dst.contrast          = src.contrast;
    dst.saturation        = src.saturation;
    dst.flickerIntensity  = src.flickerIntensity;
    dst.flickerSpeed      = src.flickerSpeed;
    dst.flickerRandomness = src.flickerRandomness;
    dst.humBarIntensity   = src.humBarIntensity;
    dst.humBarSpeed       = src.humBarSpeed;
    dst.diffuseStrength   = src.diffuseStrength;
    dst.diffuseThreshold  = src.diffuseThreshold;
    dst.phosphorR             = src.phosphorR;
    dst.phosphorG             = src.phosphorG;
    dst.phosphorB             = src.phosphorB;
    dst.staticIntensity       = src.staticIntensity;
    dst.staticSpeed           = src.staticSpeed;
    dst.shadowMaskIntensity   = src.shadowMaskIntensity;
    dst.shadowMaskScale       = src.shadowMaskScale;
    dst.shadowMaskType        = src.shadowMaskType;
    dst.convergence           = src.convergence;
    // Cursor fields
    dst.cursorScale       = src.cursorScale;
    dst.cursorThickness   = src.cursorThickness;
    dst.cursorR           = src.cursorR;
    dst.cursorG           = src.cursorG;
    dst.cursorB           = src.cursorB;
    dst.cursorAutoHide    = src.cursorAutoHide;
    dst.cursorStroke      = src.cursorStroke;
    dst.cursorPlain       = src.cursorPlain;
    // NOT copied: time, menuStyle, menuVisible, toggleX, toggleY, hiddenSources
}

// ---------------------------------------------------------------------------
// GetBuiltIn — returns the static list of factory presets.
// Each preset is constructed by value from a default CRTSettings and then
// only the fields that differ from defaults are overwritten.
// ---------------------------------------------------------------------------
const std::vector<Presets::BuiltInPreset>& Presets::GetBuiltIn() {
    static const std::vector<BuiltInPreset> kPresets = []() {
        std::vector<BuiltInPreset> v;

        // --- Default ---
        v.push_back({ "Default", CRTSettings{} });

        // --- Early 2000s Laptop — barely-there hint of CRT ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.04f;
            s.curvatureY          = 0.04f;
            s.zoom                = 1.02f;
            s.scanlineIntensity   = 0.15f;
            s.scanlineCount       = 800.f;
            s.glowStrength        = 0.1f;
            s.diffuseStrength     = 0.0f;
            s.vignetteStrength    = 0.15f;
            s.brightness          = 1.0f;
            s.contrast            = 1.0f;
            s.saturation          = 1.0f;
            s.flickerIntensity    = 0.0f;
            s.humBarIntensity     = 0.0f;
            s.phosphorR           = 1.0f;
            s.phosphorG           = 1.0f;
            s.phosphorB           = 1.0f;
            s.staticIntensity     = 0.02f;  // faint grain — TFT panels had noise too
            s.staticSpeed         = 10.0f;
            s.shadowMaskIntensity = 0.12f;  // subtle aperture grille
            s.shadowMaskScale     = 1.5f;
            s.shadowMaskType      = 0.0f;   // aperture grille
            v.push_back({ "Early 2000s Laptop", s });
        }

        // --- Arcade — bold arcade cabinet look ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.12f;
            s.curvatureY          = 0.10f;
            s.zoom                = 1.06f;
            s.scanlineIntensity   = 0.70f;
            s.scanlineCount       = 400.f;
            s.glowStrength        = 0.6f;
            s.diffuseStrength     = 0.5f;
            s.diffuseThreshold    = 0.35f;
            s.vignetteStrength    = 0.55f;
            s.brightness          = 1.1f;
            s.contrast            = 1.25f;
            s.saturation          = 1.4f;
            s.flickerIntensity    = 0.15f;
            s.flickerSpeed        = 6.0f;
            s.flickerRandomness   = 0.2f;
            s.humBarIntensity     = 0.2f;
            s.humBarSpeed         = 0.08f;
            s.phosphorR           = 1.0f;
            s.phosphorG           = 1.0f;
            s.phosphorB           = 1.0f;
            s.staticIntensity     = 0.08f;  // noisy, worn arcade cabinet
            s.staticSpeed         = 20.0f;
            s.shadowMaskIntensity = 0.45f;  // prominent dot triad for arcade authenticity
            s.shadowMaskScale     = 1.0f;   // tight — arcade monitors had fine dots
            s.shadowMaskType      = 1.0f;   // shadow mask dots
            v.push_back({ "Arcade", s });
        }

        // --- Green Phosphor — P31 monochrome terminal ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.08f;
            s.curvatureY          = 0.08f;
            s.zoom                = 1.04f;
            s.scanlineIntensity   = 0.55f;
            s.scanlineCount       = 600.f;
            s.glowStrength        = 0.9f;
            s.diffuseStrength     = 1.0f;
            s.diffuseThreshold    = 0.3f;
            s.vignetteStrength    = 0.5f;
            s.brightness          = 1.15f;
            s.contrast            = 1.3f;
            s.saturation          = 0.0f;
            s.flickerIntensity    = 0.05f;
            s.flickerSpeed        = 8.0f;
            s.flickerRandomness   = 0.1f;
            s.humBarIntensity     = 0.12f;
            s.humBarSpeed         = 0.07f;
            s.phosphorR           = 0.20f;
            s.phosphorG           = 1.0f;
            s.phosphorB           = 0.30f;
            s.staticIntensity     = 0.06f;  // phosphor screen flicker/noise
            s.staticSpeed         = 8.0f;
            s.shadowMaskIntensity = 0.20f;  // light aperture grille texture
            s.shadowMaskScale     = 1.5f;
            s.shadowMaskType      = 0.0f;   // aperture grille
            v.push_back({ "Green Phosphor", s });
        }

        // --- Amber Phosphor — P3 amber/orange terminal ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.08f;
            s.curvatureY          = 0.08f;
            s.zoom                = 1.04f;
            s.scanlineIntensity   = 0.50f;
            s.scanlineCount       = 600.f;
            s.glowStrength        = 0.8f;
            s.diffuseStrength     = 0.8f;
            s.diffuseThreshold    = 0.35f;
            s.vignetteStrength    = 0.45f;
            s.brightness          = 1.1f;
            s.contrast            = 1.25f;
            s.saturation          = 0.0f;
            s.flickerIntensity    = 0.05f;
            s.flickerSpeed        = 8.0f;
            s.flickerRandomness   = 0.1f;
            s.humBarIntensity     = 0.10f;
            s.humBarSpeed         = 0.07f;
            s.phosphorR           = 1.0f;
            s.phosphorG           = 0.55f;
            s.phosphorB           = 0.05f;
            s.staticIntensity     = 0.05f;  // warm analogue grain
            s.staticSpeed         = 8.0f;
            s.shadowMaskIntensity = 0.20f;  // light aperture grille texture
            s.shadowMaskScale     = 1.5f;
            s.shadowMaskType      = 0.0f;   // aperture grille
            v.push_back({ "Amber Phosphor", s });
        }

        // --- Heavy CRT — old boxy TV from the '80s ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.20f;
            s.curvatureY          = 0.18f;
            s.zoom                = 1.10f;
            s.scanlineIntensity   = 0.65f;
            s.scanlineCount       = 350.f;
            s.glowStrength        = 1.2f;
            s.diffuseStrength     = 1.2f;
            s.diffuseThreshold    = 0.4f;
            s.vignetteStrength    = 0.55f;
            s.brightness          = 1.1f;
            s.contrast            = 1.3f;
            s.saturation          = 1.2f;
            s.flickerIntensity    = 0.35f;
            s.flickerSpeed        = 10.0f;
            s.flickerRandomness   = 0.4f;
            s.humBarIntensity     = 0.35f;
            s.humBarSpeed         = 0.15f;
            s.phosphorR           = 1.0f;
            s.phosphorG           = 1.0f;
            s.phosphorB           = 1.0f;
            s.staticIntensity     = 0.14f;  // heavy TV noise, degraded signal
            s.staticSpeed         = 25.0f;
            s.shadowMaskIntensity = 0.55f;  // chunky '80s shadow mask dots
            s.shadowMaskScale     = 2.0f;   // large cells — old TVs had coarser masks
            s.shadowMaskType      = 1.0f;   // shadow mask dots
            v.push_back({ "Heavy CRT", s });
        }

        // --- crt-geom — balanced reference CRT, ported from the classic RetroArch shader ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.10f;
            s.curvatureY          = 0.10f;
            s.zoom                = 1.05f;
            s.scanlineIntensity   = 0.45f;
            s.scanlineCount       = 500.f;
            s.glowStrength        = 0.40f;
            s.diffuseStrength     = 0.20f;
            s.diffuseThreshold    = 0.35f;
            s.vignetteStrength    = 0.35f;
            s.brightness          = 1.00f;
            s.contrast            = 1.10f;
            s.saturation          = 1.10f;
            s.flickerIntensity    = 0.02f;
            s.flickerSpeed        = 8.0f;
            s.flickerRandomness   = 0.05f;
            s.humBarIntensity     = 0.05f;
            s.humBarSpeed         = 0.08f;
            s.staticIntensity     = 0.03f;
            s.staticSpeed         = 10.0f;
            s.shadowMaskIntensity = 0.25f;
            s.shadowMaskScale     = 1.5f;
            s.shadowMaskType      = 0.0f;   // aperture grille
            s.convergence         = 0.0f;
            v.push_back({ "crt-geom", s });
        }

        // --- crt-lottes — Tim Lottes' shader: crisp image, tight slot mask, minimal warp ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.03f;
            s.curvatureY          = 0.04f;
            s.zoom                = 1.02f;
            s.scanlineIntensity   = 0.55f;
            s.scanlineCount       = 700.f;
            s.glowStrength        = 0.20f;
            s.diffuseStrength     = 0.0f;
            s.vignetteStrength    = 0.25f;
            s.brightness          = 1.05f;
            s.contrast            = 1.15f;
            s.saturation          = 1.20f;
            s.flickerIntensity    = 0.0f;
            s.humBarIntensity     = 0.0f;
            s.staticIntensity     = 0.01f;
            s.staticSpeed         = 10.0f;
            s.shadowMaskIntensity = 0.50f;  // lottes uses a strong saturated mask
            s.shadowMaskScale     = 1.0f;   // tight cells — fine dot pitch
            s.shadowMaskType      = 2.0f;   // slot mask
            s.convergence         = 0.0f;
            v.push_back({ "crt-lottes", s });
        }

        // --- Sony PVM — professional reference monitor: flat, clean, accurate ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.02f;
            s.curvatureY          = 0.02f;
            s.zoom                = 1.01f;
            s.scanlineIntensity   = 0.30f;
            s.scanlineCount       = 600.f;
            s.glowStrength        = 0.15f;
            s.diffuseStrength     = 0.0f;
            s.vignetteStrength    = 0.15f;
            s.brightness          = 1.00f;
            s.contrast            = 1.05f;
            s.saturation          = 1.05f;
            s.flickerIntensity    = 0.0f;
            s.humBarIntensity     = 0.0f;
            s.staticIntensity     = 0.005f;
            s.staticSpeed         = 10.0f;
            s.shadowMaskIntensity = 0.20f;  // subtle aperture grille — PVMs were very clean
            s.shadowMaskScale     = 1.5f;
            s.shadowMaskType      = 0.0f;   // aperture grille
            s.convergence         = 0.0f;
            v.push_back({ "Sony PVM", s });
        }

        // --- Trinitron — Sony's flat aperture-grille tube; iconic SNES/PS1 look ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.00f;  // Trinitrons were flat horizontally
            s.curvatureY          = 0.04f;  // slight vertical curve only
            s.zoom                = 1.02f;
            s.scanlineIntensity   = 0.40f;
            s.scanlineCount       = 525.f;  // NTSC 480i equivalent line count
            s.glowStrength        = 0.35f;
            s.diffuseStrength     = 0.15f;
            s.diffuseThreshold    = 0.35f;
            s.vignetteStrength    = 0.30f;
            s.brightness          = 1.05f;
            s.contrast            = 1.10f;
            s.saturation          = 1.15f;
            s.flickerIntensity    = 0.02f;
            s.flickerSpeed        = 8.0f;
            s.flickerRandomness   = 0.05f;
            s.humBarIntensity     = 0.05f;
            s.humBarSpeed         = 0.07f;
            s.staticIntensity     = 0.025f;
            s.staticSpeed         = 10.0f;
            s.shadowMaskIntensity = 0.30f;  // aperture grille — Trinitron's trademark look
            s.shadowMaskScale     = 1.5f;
            s.shadowMaskType      = 0.0f;   // aperture grille
            s.convergence         = 0.0f;
            v.push_back({ "Trinitron", s });
        }

        // --- NTSC Composite — degraded consumer signal; heavy noise, colour bleed, convergence error ---
        {
            CRTSettings s{};
            s.curvatureX          = 0.15f;
            s.curvatureY          = 0.12f;
            s.zoom                = 1.07f;
            s.scanlineIntensity   = 0.50f;
            s.scanlineCount       = 350.f;
            s.glowStrength        = 0.80f;
            s.diffuseStrength     = 0.60f;
            s.diffuseThreshold    = 0.40f;
            s.vignetteStrength    = 0.50f;
            s.brightness          = 0.95f;
            s.contrast            = 1.20f;
            s.saturation          = 1.30f;
            s.flickerIntensity    = 0.25f;
            s.flickerSpeed        = 10.0f;
            s.flickerRandomness   = 0.50f;
            s.humBarIntensity     = 0.30f;
            s.humBarSpeed         = 0.12f;
            s.humBarFrequency     = 2.0f;
            s.staticIntensity     = 0.12f;
            s.staticSpeed         = 18.0f;
            s.shadowMaskIntensity = 0.40f;
            s.shadowMaskScale     = 1.5f;
            s.shadowMaskType      = 1.0f;   // shadow mask dots
            s.convergence         = 0.30f;  // visible RGB misalignment from worn gun alignment
            v.push_back({ "NTSC Composite", s });
        }

        return v;
    }();

    return kPresets;
}

// ---------------------------------------------------------------------------
// DefaultPath — %APPDATA%\SeeRT Overlay\presets.json
// ---------------------------------------------------------------------------
std::string Presets::DefaultPath() {
    std::string settingsPath = Settings::DefaultPath();
    // Replace "settings.json" suffix with "presets.json".
    const std::string kOld = "settings.json";
    const std::string kNew = "presets.json";
    auto pos = settingsPath.rfind(kOld);
    if (pos != std::string::npos)
        settingsPath.replace(pos, kOld.size(), kNew);
    return settingsPath;
}

// ---------------------------------------------------------------------------
// Helper: read/write the visual fields of a CRTSettings to/from a JSON object.
// Mirrors the field list in Presets::Apply() exactly.
// ---------------------------------------------------------------------------
static json SettingsToJson(const CRTSettings& s) {
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
    j["flickerIntensity"]  = s.flickerIntensity;
    j["flickerSpeed"]      = s.flickerSpeed;
    j["flickerRandomness"] = s.flickerRandomness;
    j["humBarIntensity"]   = s.humBarIntensity;
    j["humBarSpeed"]       = s.humBarSpeed;
    j["diffuseStrength"]   = s.diffuseStrength;
    j["diffuseThreshold"]  = s.diffuseThreshold;
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
    j["cursorThickness"]   = s.cursorThickness;
    j["cursorR"]           = s.cursorR;
    j["cursorG"]           = s.cursorG;
    j["cursorB"]           = s.cursorB;
    j["cursorAutoHide"]    = s.cursorAutoHide;
    j["cursorStroke"]      = s.cursorStroke;
    j["cursorPlain"]       = s.cursorPlain;
    return j;
}

static void JsonToSettings(const json& j, CRTSettings& s) {
    auto getF = [&](const char* key, float& field) {
        if (j.contains(key)) field = j[key].get<float>();
    };
    auto getB = [&](const char* key, bool& field) {
        if (j.contains(key)) field = j[key].get<bool>();
    };
    getF("curvatureX",        s.curvatureX);
    getF("curvatureY",        s.curvatureY);
    getF("zoom",              s.zoom);
    getF("scanlineIntensity", s.scanlineIntensity);
    getF("scanlineCount",     s.scanlineCount);
    getF("vignetteStrength",  s.vignetteStrength);
    getF("glowStrength",      s.glowStrength);
    getF("brightness",        s.brightness);
    getF("contrast",          s.contrast);
    getF("saturation",        s.saturation);
    getF("flickerIntensity",  s.flickerIntensity);
    getF("flickerSpeed",      s.flickerSpeed);
    getF("flickerRandomness", s.flickerRandomness);
    getF("humBarIntensity",   s.humBarIntensity);
    getF("humBarSpeed",       s.humBarSpeed);
    getF("diffuseStrength",   s.diffuseStrength);
    getF("diffuseThreshold",  s.diffuseThreshold);
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
    getF("cursorThickness",   s.cursorThickness);
    getF("cursorR",           s.cursorR);
    getF("cursorG",           s.cursorG);
    getF("cursorB",           s.cursorB);
    getB("cursorAutoHide",    s.cursorAutoHide);
    getB("cursorStroke",      s.cursorStroke);
    getB("cursorPlain",       s.cursorPlain);
}

// ---------------------------------------------------------------------------
// IsBuiltIn — true if name matches any factory preset.
// ---------------------------------------------------------------------------
bool Presets::IsBuiltIn(const std::string& name) {
    for (const auto& bi : GetBuiltIn())
        if (bi.name == name) return true;
    return false;
}

// ---------------------------------------------------------------------------
// BuildOrderedList — merges the saved name-order with the live preset data.
// ---------------------------------------------------------------------------
std::vector<PresetItem> Presets::BuildOrderedList(
    const std::vector<std::string>& order,
    const std::vector<UserPreset>&  userPresets)
{
    const auto& builtins = GetBuiltIn();

    std::unordered_map<std::string, int> biMap, upMap;
    for (int i = 0; i < (int)builtins.size();    i++) biMap[builtins[i].name]    = i;
    for (int i = 0; i < (int)userPresets.size(); i++) upMap[userPresets[i].name] = i;

    std::vector<PresetItem>        result;
    std::unordered_set<std::string> seen;

    for (const auto& name : order) {
        if (seen.count(name)) continue;
        seen.insert(name);
        auto bi = biMap.find(name);
        if (bi != biMap.end()) {
            result.push_back({ name, true,  builtins[bi->second].settings });
        } else {
            auto up = upMap.find(name);
            if (up != upMap.end())
                result.push_back({ name, false, userPresets[up->second].settings });
        }
    }
    // Append any built-ins not mentioned in the saved order (e.g. newly added presets).
    for (const auto& bi : builtins)
        if (!seen.count(bi.name))
            result.push_back({ bi.name, true, bi.settings });
    // Append any user presets not mentioned in the saved order.
    for (const auto& p : userPresets)
        if (!seen.count(p.name))
            result.push_back({ p.name, false, p.settings });

    return result;
}

// ---------------------------------------------------------------------------
// DefaultOrder — all built-ins then all user presets, in natural order.
// ---------------------------------------------------------------------------
std::vector<std::string> Presets::DefaultOrder(const std::vector<UserPreset>& userPresets) {
    std::vector<std::string> order;
    for (const auto& bi : GetBuiltIn())
        order.push_back(bi.name);
    for (const auto& p : userPresets)
        order.push_back(p.name);
    return order;
}

// ---------------------------------------------------------------------------
// Load — reads { "order": [...], "presets": [...] }.
// Falls back gracefully to the old bare-array format.
// ---------------------------------------------------------------------------
void Presets::Load(const std::string& path,
                   std::vector<UserPreset>& outPresets,
                   std::vector<std::string>& outOrder)
{
    outPresets.clear();
    outOrder.clear();
    try {
        std::ifstream f(path);
        if (!f.is_open()) return;
        json root = json::parse(f);

        if (root.is_array()) {
            // Old format: bare array of user presets, no saved order.
            for (const auto& elem : root) {
                if (!elem.contains("name") || !elem["name"].is_string()) continue;
                UserPreset p;
                p.name = elem["name"].get<std::string>();
                JsonToSettings(elem, p.settings);
                outPresets.push_back(std::move(p));
            }
        } else if (root.is_object()) {
            if (root.contains("presets") && root["presets"].is_array()) {
                for (const auto& elem : root["presets"]) {
                    if (!elem.contains("name") || !elem["name"].is_string()) continue;
                    UserPreset p;
                    p.name = elem["name"].get<std::string>();
                    JsonToSettings(elem, p.settings);
                    outPresets.push_back(std::move(p));
                }
            }
            if (root.contains("order") && root["order"].is_array()) {
                for (const auto& n : root["order"])
                    if (n.is_string()) outOrder.push_back(n.get<std::string>());
            }
        }
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// Save — writes { "order": [...], "presets": [...] }.
// ---------------------------------------------------------------------------
void Presets::Save(const std::string& path,
                   const std::vector<UserPreset>& presets,
                   const std::vector<std::string>& order)
{
    try {
        fs::path p(path);
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);

        json presetArr = json::array();
        for (const auto& preset : presets) {
            json elem = SettingsToJson(preset.settings);
            elem["name"] = preset.name;
            presetArr.push_back(std::move(elem));
        }
        json root;
        root["order"]   = order;
        root["presets"] = std::move(presetArr);

        std::ofstream f(path);
        if (!f.is_open()) return;
        f << root.dump(4);
    } catch (...) {}
}

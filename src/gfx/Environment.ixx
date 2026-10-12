module;

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

export module openape.environment;

import openape.util;
import openape.math;

// The stock maps' worldspawn lighting settings (https://wiki.zeroy.com/index.php?title=Call_of_Duty_4:_Stock_worldspawn_settings),
// used as preview lighting presets. `sundirection` is "pitch yaw roll" in degrees; a negative pitch looks up at the sun.
export namespace oa
{
    struct StockLighting
    {
        const char* name;
        bool multiplayer;
        float sunlight;                          // intensity
        std::array<float, 3> sunColor;           // "suncolor"
        std::array<float, 3> diffuseColor;       // "sundiffusecolor": colour of the light bounced from the sky
        float diffuseFraction;
        float ambient;
        float pitch, yaw;                        // "sundirection"
    };

    // The values the renderer needs.
    struct LightSetup
    {
        bool headlight = false;                  // camera-attached light instead of a world-fixed sun
        Vec3 toSun{ 0.5f, 0.3f, 0.8f };          // unit vector from the scene towards the sun
        Vec3 sunColor{ 1, 0.97f, 0.9f };
        Vec3 ambient{ 0.3f, 0.32f, 0.38f };
    };

    inline const std::vector<StockLighting>& StockLightings()
    {
        static const std::vector<StockLighting> t = {
            // ---- single player
            { "aftermath",        false, 0.8f,  { 1, 1, 1 },                 { 1, 1, 1 },                 0.45f, 0.15f, -35, 170 },
            { "airlift",          false, 1.5f,  { 1, .92f, .88f },           { .9f, .75f, .88f },         0.25f, 0.05f, -17, 56.333f },
            { "airplane",         false, 1.5f,  { .9f, .9f, 1 },             { .015f, .015f, .013f },     0.2f,  0.03f, -18, 100 },
            { "ambush",           false, 1.5f,  { .9f, .98f, 1 },            { .43f, .39f, 1 },           0.05f, 0.1f,  -146, -34 },
            { "armada",           false, 1.4f,  { .85f, .76f, .53f },        { .75f, .84f, 1 },           0.1f,  0.0f,  -30, -340 },
            { "blackout",         false, 0.6f,  { .75f, .9f, 1 },            { .5f, .6f, 1 },             0.35f, 0.1f,  -30, 42 },
            { "bog_a",            false, 0.5f,  { .68f, .72f, .76f },        { .718f, .776f, 1 },         0.4f,  0.125f,-22, 247 },
            { "bog_b",            false, 1.0f,  { 1, .92f, .88f },           { .9f, .88f, .75f },         0.5f,  0.1f,  -50, 136 },
            { "cargoship",        false, 1.0f,  { .5f, .6f, 1 },             { .5f, .5f, .7f },           0.15f, 0.1f,  -25, 315 },
            { "coup",             false, 1.1f,  { 1, 1, .85f },              { 1, .95f, .8f },            0.2f,  0.02f, -40, 72 },
            { "hunted",           false, 0.6f,  { .75f, .9f, 1 },            { .5f, .6f, 1 },             0.35f, 0.2f,  -40, 49 },
            { "ICBM",             false, 1.0f,  { 1, 1, 1 },                 { 1, 1, 1 },                 0.6f,  0.1f,  -32.5f, -4 },
            { "jeepride",         false, 1.7f,  { .996f, .96f, .88f },       { .718f, .776f, 1 },         0.1f,  0.1f,  -25, 231 },
            { "killhouse",        false, 1.5f,  { 1, .95f, .8f },            { .8f, .85f, 1 },            0.35f, 0.1f,  -40, 290 },
            { "launchfacility_a", false, 1.5f,  { 1, .8f, .6f },             { .718f, .776f, 1 },         0.4f,  0.2f,  -30, 0 },
            { "launchfacility_b", false, 0.8f,  { .95f, .85f, .65f },        { 1, 1, 1 },                 0.2f,  0.1f,  -35, 290 },
            { "scoutsniper",      false, 1.7f,  { 1, .976f, .875f },         { .467f, .525f, .659f },     0.08f, 0.2f,  -30, -56 },
            { "sniperescape",     false, 0.9f,  { .9f, .98f, 1 },            { .9f, .92f, 1 },            0.25f, 0.0f,  -155, -50 },
            { "village_assault",  false, 0.8f,  { .8f, .9f, 1 },             { .4f, .6f, 1 },             0.6f,  0.1f,  -30, 91 },
            { "village_defend",   false, 1.6f,  { .996f, .922f, .9f },       { .718f, .776f, 1 },         0.2f,  0.04f, -40, 225 },
            // ---- multiplayer
            { "mp_backlot",       true,  1.3f,  { 1, .92f, .88f },           { .9f, .88f, .75f },         0.5f,  0.15f, -43.5f, 25.11f },
            { "mp_bloc",          true,  0.9f,  { .9f, .98f, 1 },            { .9f, .92f, 1 },            0.25f, 0.0f,  -120, -50 },
            { "mp_broadcast",     true,  1.4f,  { 1, .85f, .67f },           { .87f, .77f, .65f },        0.2f,  0.07f, -20, -280 },
            { "mp_carentan",      true,  0.75f, { .8f, .9f, 1 },             { .98f, .95f, 1 },           0.7f,  0.1f,  -31, 4.04f },
            { "mp_cargoship",     true,  1.3f,  { .5f, .6f, 1 },             { .5f, .5f, .7f },           0.15f, 0.1f,  -25, 315 },
            { "mp_citystreets",   true,  0.78f, { 1, .92f, .88f },           { 1, .89f, .74f },           0.5f,  0.2f,  -67, 97 },
            { "mp_convoy",        true,  1.6f,  { .85f, .76f, .53f },        { .75f, .84f, 1 },           0.1f,  0.3f,  -30, -340 },
            { "mp_countdown",     true,  1.5f,  { 1, .8f, .6f },             { .718f, .776f, 1 },         0.2f,  0.2f,  -60, 0 },
            { "mp_crash",         true,  1.3f,  { 1, .92f, .88f },           { .9f, .88f, .75f },         0.5f,  0.02f, -50, 240 },
            { "mp_crash_snow",    true,  0.25f, { .65f, .7f, 1 },           { .65f, .7f, 1 },            0.95f, 0.1f,  -120, -50 },
            { "mp_creek",         true,  1.5f,  { .92f, .922f, .9f },        { .69f, .79f, 1 },           0.2f,  0.04f, -55, 225 },
            { "mp_crossfire",     true,  3.0f,  { 1, .92f, .88f },           { .9f, .88f, .75f },         0.5f,  0.1f,  -60, 136 },
            { "mp_farm",          true,  1.0f,  { .9f, .98f, 1 },            { .9f, .92f, 1 },            0.25f, 0.0f,  -40, 225 },
            { "mp_killhouse",     true,  1.5f,  { 1, .95f, .8f },            { .8f, .85f, 1 },            0.35f, 0.1f,  -40, 290 },
            { "mp_overgrown",     true,  1.1f,  { .84f, .81f, .58f },        { .8f, .88f, .94f },         0.25f, 0.1f,  -147, 119 },
            { "mp_pipeline",      true,  1.15f, { .9f, .98f, 1 },            { .8f, .88f, .94f },         0.25f, 0.15f, -44, 220 },
            { "mp_shipment",      true,  1.3f,  { 1, 1, .95f },              { .9f, 1, .75f },            0.2f,  0.1f,  -43, 51.5f },
            { "mp_showdown",      true,  1.6f,  { .85f, .76f, .53f },        { .75f, .84f, 1 },           0.1f,  0.02f, -33.34f, 117.76f },
            { "mp_strike",        true,  1.0f,  { 1, .94f, .8f },            { .9f, .88f, .75f },         0.5f,  0.02f, -45, -144 },
            { "mp_vacent",        true,  1.3f,  { .84f, .81f, .58f },        { .8f, .88f, .94f },         0.25f, 0.15f, -45, 25 },
        };
        return t;
    }

    // A named time-of-day look: a sky box plus lighting written in the same terms as the stock map settings.
    struct EnvironmentPreset
    {
        const char* name;
        const char* sky;          // stock sky box (images/<sky>.iwi)
        StockLighting lighting;
    };

    inline const std::vector<EnvironmentPreset>& EnvironmentPresets()
    {
        static const std::vector<EnvironmentPreset> t = {
            { "Morning",   "sp_marksman_ft",      { "Morning",   false, 1.0f, { 1, .78f, .58f },   { .70f, .78f, 1 },   0.35f, 0.08f, -20, 70 } },
            { "Noon",      "sp_jeepride_ft",      { "Noon",      false, 1.6f, { 1, .98f, .94f },   { .72f, .84f, 1 },   0.30f, 0.12f, -78, 110 } },
            { "Day",       "mp_creek_ft",         { "Day",       false, 1.3f, { 1, .96f, .88f },   { .75f, .85f, 1 },   0.30f, 0.10f, -50, 40 } },
            { "Afternoon", "mp_backlot_ft",       { "Afternoon", false, 1.3f, { 1, .90f, .72f },   { .85f, .85f, .80f }, 0.40f, 0.12f, -32, 255 } },
            { "Overcast",  "mp_canal_ft",         { "Overcast",  false, 0.95f,{ .96f, .97f, 1 },   { .92f, .95f, 1 },   0.90f, 0.45f, -60, 200 } },
            { "Sunset",    "mp_dusk_ft",          { "Sunset",    false, 1.3f, { 1, .52f, .26f },   { .70f, .55f, .70f }, 0.30f, 0.06f, -6, 250 } },
            { "Dusk",      "mp_overgrown_dusk_ft",{ "Dusk",      false, 0.8f, { 1, .66f, .62f },   { .55f, .62f, 1 },   0.55f, 0.22f, -9, 300 } },
            { "Night",     "sp_village_assault_ft",{ "Night",    false, 0.6f, { .55f, .68f, 1 },   { .30f, .42f, .85f }, 0.60f, 0.05f, -38, 60 } },
            { "Moonlit",   "mp_argun_ft",         { "Moonlit",   false, 0.45f,{ .62f, .72f, 1 },   { .25f, .35f, .75f }, 0.50f, 0.04f, -55, 140 } },
        };
        return t;
    }

    // The renderer's light for a stock setting.
    inline LightSetup MakeLightSetup(const StockLighting& s)
    {
        LightSetup l;
        float p = s.pitch * kPi / 180.0f, y = s.yaw * kPi / 180.0f;
        // Angles use the engine convention (pitch positive looks down), so the sun is where a negative pitch points.
        l.toSun = Vec3{ std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p) }.Normalized();
        if (l.toSun.z < 0.05f) l.toSun = Vec3{ l.toSun.x, l.toSun.y, 0.05f }.Normalized();   // the preview has no underground sun
        float k = std::clamp(s.sunlight, 0.0f, 3.0f) * 0.62f;
        l.sunColor = { std::min(s.sunColor[0] * k, 1.6f), std::min(s.sunColor[1] * k, 1.6f), std::min(s.sunColor[2] * k, 1.6f) };
        float a = std::clamp(0.14f + s.ambient * 1.4f + s.diffuseFraction * 0.30f, 0.12f, 0.7f);
        l.ambient = { s.diffuseColor[0] * a, s.diffuseColor[1] * a, s.diffuseColor[2] * a };
        return l;
    }

    // Sky box that goes with a map (images/<name>_ft in the stock archives), or empty if there is no obvious one.
    inline std::string SkyForMap(const std::string& map, const std::vector<std::string>& available)
    {
        auto has = [&](const std::string& n) { return std::find(available.begin(), available.end(), n) != available.end(); };
        static const std::vector<std::pair<std::string, std::string>> special = {
            { "bog_a", "sp_boga" }, { "bog_b", "sp_bog" }, { "cargoship", "sp_cargo" }, { "ICBM", "sky_icbm_a" },
            { "mp_cargoship", "sp_cargo" }, { "mp_countdown", "sp_launchfacility_a" }, { "mp_bloc", "pripyat" },
            { "armada", "sp_seaknight_assault" }, { "airplane", "sp_desert" } };
        std::string m = ToLower(map);
        for (auto& [k, v] : special) if (ToLower(k) == m && has(v + "_ft")) return v + "_ft";
        for (auto cand : { m, "sp_" + m, "mp_" + m }) if (has(cand + "_ft")) return cand + "_ft";
        return {};
    }
}

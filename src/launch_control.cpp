// 2-Step Launch Control for Forza Horizon 3 (FH3 Mod Platform mod).
//
// Garage > Upgrades > Engine > "2-Step Launch Control" (Stock / 2-Step). A car with it gets a row on its Tuning > Springs
// page ("Launch Control | 2-Step RPM", 1000 to 8000, default 3500). Launch control is only active when ALL of these hold:
//   - the car is in first gear and standing still,
//   - the handbrake is held, and the throttle is (nearly) fully pressed.
// Then the engine is held at the 2-step RPM. Let go of the handbrake and the car launches from that RPM.
//
// How it works (read from the game's code; the log shows every step when Log=1):
//  - Car::Update (game+0x18B8D20, once a frame on the game's thread) is hooked; the HUD controller (game+0xB26690) says which Car is
//    the player's.
//  - A Car's dynamics interface is at Car+0x71E8 (CCarDynamics + 0x4B0). In it: +0x1D4 is the engine's angular velocity (rad/s; the HUD's
//    RPM is this x 9.5493), +0xAA8 the current gear (byte, low nibble; 0 = reverse, 1 = first), the dynamics' velocity is at
//    CCarDynamics + 0x10 (the same as Airride reads).
//  - While launch control is active, the engine speed is clamped to the 2-step RPM after the Car's update.
//  - The throttle (right trigger or W / Up arrow) and the handbrake (A or B, or Space) are read from the controller and keyboard,
//    as the Airride mod reads L3. Wheels are not supported.
#include "fh3mod.h"
#include "icon_data.h"   // icons/launchcontrol.png as FH3 .swatchbin (scripts/gen_icons.py)
#include "icon_pack.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr size_t kOffIface     = 0x71E8;   // Car -> car dynamics interface (CCarDynamics + 0x4B0)
constexpr size_t kIfaceToDyn   = 0x4B0;
constexpr size_t kDynVelocity  = 0x10;     // CCarDynamics: velocity (3 floats)
constexpr size_t kIfOmega      = 0x1D4;    // interface: engine angular velocity (rad/s)
constexpr size_t kIfGear       = 0xAA8;    // interface: gear (byte, low nibble)
constexpr size_t kDynEngMax    = 0x684;    // CCarDynamics: the engine speed limit (rad/s); the dynamics caps the engine speed (dyn+0x5F8) at this and slows the driveline with it
constexpr size_t kDynThrottle  = 0x1BD0;   // CCarDynamics: the throttle (0..1) the engine torque is blended with (between the closed- and open-throttle torque curves)
constexpr size_t kDynThrCopy   = 0x1708;   // CCarDynamics: a copy of the throttle kept at the end of the input step
constexpr size_t kDynThrRaw    = 0x1748;   // CCarDynamics: the raw input block (throttle first), copied to +0x1BD0 by the input step
constexpr uintptr_t kRvaDynInput = 0x1D78F70;  // the dynamics' input step (rcx = CCarDynamics, xmm1 = dt)
constexpr size_t kDynClutch    = 0xF44;    // CCarDynamics: the clutch pedal (0 = released/engaged, 1 = pressed); the clutch's torque capacity is (1 - this) * ...
constexpr size_t kDynPowerCut  = 0xCF60;   // CCarDynamics: fraction of the engine torque taken away (0..1); the torque nodes use (1 - this)
constexpr uintptr_t kRvaDynPower = 0x1D7D740;  // computes it every step (rcx = CCarDynamics)
constexpr size_t kDynEngOmega  = 0x5F8;    // CCarDynamics: the engine speed (rad/s)
constexpr uintptr_t kRvaCarUpdate = 0x18B8D20;
constexpr uintptr_t kRvaHudCtrl   = 0xB26690;
constexpr float kRadPerSecToRpm   = 9.549296586f;

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;
int g_handle           = -1;
char* g_game           = nullptr;
std::atomic<bool> g_stop{false};   // fh3mod_shutdown (hot reload, platform API 16): the mod's threads and hooks stand down
HANDLE g_threads[2]    = {};

// fh3-mods\LaunchControl.ini (written with these defaults on first start).
int g_default_rpm   = 3500;    // for a car whose slider was never touched
int g_price         = 4000;
int g_first_gear    = 1;       // the gear value of first gear (0 = reverse, 1 = first)
float g_still_mps   = 1.5f;    // "standing" below this speed
int g_throttle_min  = 230;     // right trigger value (0..255) that counts as full throttle
bool g_hand_a       = true;    // the A button counts as the handbrake
bool g_hand_b       = true;    // the B button counts as the handbrake
bool g_verbose      = false;   // Log=1 writes lines to platform.log
int g_method        = 4;
int g_dump_ms       = 1500;   // after the launch the clutch is forced shut (dumped) for this long       // 1 = clamp the engine speed after the Car update

std::wstring g_cfg_path;

bool is_problem(const char* s) {
    char low[256];
    size_t i = 0;
    for (; s[i] && i < sizeof low - 1; ++i) low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    low[i] = 0;
    static const char* const words[] = {"could not", "couldn't", "can't", "cannot", "failed", "not found", "not added", "isn't the code", "not hooked", "needs api", "error"};
    for (const char* w : words)
        if (std::strstr(low, w)) return true;
    return false;
}

void say(const char* fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_api && g_mod && (g_verbose || is_problem(line))) g_api->log(g_mod, line);
}

// ---- config ----------------------------------------------------------------------------------------------------------------
float cfg_float(const wchar_t* key, float def) {
    wchar_t buf[64] = L"";
    GetPrivateProfileStringW(L"LaunchControl", key, L"", buf, 64, g_cfg_path.c_str());
    if (buf[0]) return static_cast<float>(std::wcstod(buf, nullptr));
    wchar_t v[48];
    _snwprintf(v, 48, L"%g", static_cast<double>(def));
    WritePrivateProfileStringW(L"LaunchControl", key, v, g_cfg_path.c_str());
    return def;
}
int cfg_int(const wchar_t* key, int def) { return static_cast<int>(std::lround(cfg_float(key, static_cast<float>(def)))); }

void load_config() {
    g_default_rpm  = cfg_int(L"DefaultRpm", g_default_rpm);
    g_price        = cfg_int(L"Price", g_price);
    g_first_gear   = cfg_int(L"FirstGear", g_first_gear);
    g_still_mps    = cfg_float(L"StandstillMps", g_still_mps);
    g_throttle_min = cfg_int(L"ThrottleMin", g_throttle_min);
    g_hand_a       = cfg_int(L"HandbrakeA", 1) != 0;
    g_hand_b       = cfg_int(L"HandbrakeB", 1) != 0;
    g_method       = cfg_int(L"Method", g_method);
    g_dump_ms      = cfg_int(L"DumpMs", g_dump_ms);
    g_verbose      = cfg_int(L"Log", 0) != 0;
    if (g_default_rpm < 1000) g_default_rpm = 1000;
    if (g_default_rpm > 8000) g_default_rpm = 8000;
    if (g_throttle_min < 1) g_throttle_min = 1;
    if (g_throttle_min > 255) g_throttle_min = 255;
}

// ---- the 2-step RPM, per garage car (LaunchControl.ini [Rpm]) ----------------------------------------------------------------
float rpm_for(uint32_t garage) {
    wchar_t key[24], buf[32];
    _snwprintf(key, 24, L"%u", garage);
    GetPrivateProfileStringW(L"Rpm", key, L"", buf, 32, g_cfg_path.c_str());
    float v = static_cast<float>(g_default_rpm);
    if (buf[0]) {
        wchar_t* end = nullptr;
        const double d = std::wcstod(buf, &end);
        if (end != buf && std::isfinite(d)) v = static_cast<float>(d);
    }
    return v < 1000.0f ? 1000.0f : v > 8000.0f ? 8000.0f : v;
}
void set_rpm(uint32_t garage, float rpm) {
    wchar_t key[24], val[32];
    _snwprintf(key, 24, L"%u", garage);
    _snwprintf(val, 32, L"%.0f", static_cast<double>(rpm));
    WritePrivateProfileStringW(L"Rpm", key, val, g_cfg_path.c_str());
}

bool car_has_kit() { return g_handle >= 0 && g_api->upgrade_stage(g_mod, g_handle, 0) > 0; }

float row_get(void*) { return rpm_for(g_api->upgrade_active_garage()); }
void row_set(void*, float v) {
    const uint32_t garage = g_api->upgrade_active_garage();
    const float rpm       = std::round(v / 100.0f) * 100.0f;
    set_rpm(garage, rpm);
    say("tuning: 2-step RPM for garage car %u set to %.0f", garage, static_cast<double>(rpm));
}
int row_visible(void*) { return car_has_kit() ? 1 : 0; }

// ---- input: throttle and handbrake (controller and keyboard) -----------------------------------------------------------------
std::atomic<int> g_rt{0};            // right trigger 0..255 (or 255 for the throttle keys)
std::atomic<bool> g_hand{false};     // handbrake held
std::atomic<void*> g_hud_player{nullptr};
std::atomic<ULONGLONG> g_hud_tick{0};

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, void*);
struct PadState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};

bool game_in_front() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI input_thread(LPVOID) {
    XInputGetStateFn get = nullptr;
    for (const wchar_t* n : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryW(n)) {
            get = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
            if (get) break;
        }
    }
    if (!get) say("XInput couldn't be loaded: only the keyboard can drive launch control");
    for (;;) {
        Sleep(8);
        if (g_stop.load()) return 0;
        int rt = 0;
        bool hand = false;
        if (game_in_front()) {
            if (get) {
                for (DWORD i = 0; i < 4; ++i) {
                    PadState st{};
                    if (get(i, &st) == ERROR_SUCCESS) {
                        if (st.rt > rt) rt = st.rt;
                        if (g_hand_a && (st.buttons & 0x1000)) hand = true;   // XINPUT_GAMEPAD_A
                        if (g_hand_b && (st.buttons & 0x2000)) hand = true;   // XINPUT_GAMEPAD_B
                    }
                }
            }
            if ((GetAsyncKeyState(VK_UP) & 0x8000) || (GetAsyncKeyState('W') & 0x8000)) rt = 255;
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) hand = true;
        }
        g_rt.store(rt);
        g_hand.store(hand);
    }
}

// ---- the player's car ------------------------------------------------------------------------------------------------------------
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

float car_speed(char* iface) {
    __try {
        const float* v = reinterpret_cast<const float*>(iface - kIfaceToDyn + kDynVelocity);
        const float s  = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return std::isfinite(s) ? s : 1e9f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1e9f;
    }
}

// Probe (Method=0, Log=1): once a second while the throttle is down, logs every float in the car's dynamics object (CCarDynamics, 0xD600 bytes)
// that looks like an engine speed (30..950 rad/s) and changed since the last sample, to find where the real engine speed lives.
float g_snap[0xD600 / 4];
bool g_snap_ok     = false;
int g_probe_lines  = 0;
ULONGLONG g_probe_t = 0;
void probe_dyn(char* iface, int gear, float speed, int rt) {
    if (g_method != 0 || !g_verbose || g_probe_lines > 260) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_probe_t < 1000 || rt < 20) return;
    g_probe_t = now;
    char* dyn = iface - kIfaceToDyn;
    char buf[560];
    int n = std::snprintf(buf, sizeof buf, "dyn gear %d speed %.1f rt %d iface+1D4 %.2f +1D0 %.2f +AAC %.2f |", gear, static_cast<double>(speed), rt,
                          static_cast<double>(*reinterpret_cast<float*>(iface + 0x1D4)), static_cast<double>(*reinterpret_cast<float*>(iface + 0x1D0)),
                          static_cast<double>(*reinterpret_cast<float*>(iface + 0xAAC)));
    int cnt = 0;
    __try {
        for (size_t o = 0; o < 0xD600; o += 4) {
            const float v = *reinterpret_cast<const float*>(dyn + o);
            const bool plausible = std::isfinite(v) && v > 30.0f && v < 950.0f;
            const float old = g_snap[o / 4];
            g_snap[o / 4] = v;
            if (!g_snap_ok || !plausible) continue;
            if (std::fabs(v - old) > 0.005f * v) {
                n += std::snprintf(buf + n, sizeof buf - n, " %zX=%.0f", o, static_cast<double>(v));
                if (++cnt % 14 == 0 || n > 500) {
                    say("%s", buf);
                    ++g_probe_lines;
                    n = std::snprintf(buf, sizeof buf, "dyn+ (cont) |");
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        say("probe: a fault while reading the dynamics object");
    }
    if (n > 20) { say("%s", buf); ++g_probe_lines; }
    g_snap_ok = true;
}

// Method=2: where does the game keep the throttle? Hold a partial throttle (about 30..85 %) and every 0.5 s all floats near rt/255 are logged.
int g_tp_lines = 0;
ULONGLONG g_tp_t = 0;
void probe_throttle(char* car, char* iface, int rt) {
    if (g_method != 2 || !g_verbose || g_tp_lines > 150 || rt < 30 || rt > 225) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_tp_t < 500) return;
    g_tp_t = now;
    const float want = rt / 255.0f;
    char buf[560];
    int n = std::snprintf(buf, sizeof buf, "throttle %.3f (rt %d) |", static_cast<double>(want), rt);
    int cnt = 0;
    struct Region { const char* tag; char* base; size_t size; } regs[2] = {{"d", iface - kIfaceToDyn, 0xD600}, {"c", car, 0x7800}};
    for (const Region& r : regs) {
        __try {
            for (size_t o = 0; o < r.size; o += 4) {
                const float v = *reinterpret_cast<const float*>(r.base + o);
                if (std::isfinite(v) && std::fabs(v - want) < 0.02f) {
                    n += std::snprintf(buf + n, sizeof buf - n, " %s%zX=%.3f", r.tag, o, static_cast<double>(v));
                    if (++cnt % 14 == 0 || n > 500) {
                        say("%s", buf);
                        ++g_tp_lines;
                        n = std::snprintf(buf, sizeof buf, "throttle (cont) |");
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            say("probe: a fault while reading the %s region", r.tag);
        }
    }
    if (n > 20) { say("%s", buf); ++g_tp_lines; }
}

bool g_active       = false;
ULONGLONG g_last_log = 0;
ULONGLONG g_last_probe = 0;
int g_clamps        = 0;

// Lowers the engine's speed limit (dyn+0x684) while active and puts the car's own value back otherwise.
char* g_limit_iface = nullptr;   // the interface whose limit is lowered right now
float g_orig_max    = 0;
ULONGLONG g_limit_log = 0;
// Method 3: the engine's own speed state (dyn+0x5F8) is held at the set rpm after every Car update, like a fuel cut limiter:
// when it reaches the limit the engine is "cut" for 10 ms (speed dropped a little), then it may rise again. The driveline keeps the
// engine speed on release, so the car launches from the set rpm instead of jumping to the factory limiter.
ULONGLONG g_cut_until = 0;
void hold_engine(char* iface, bool active, float rpm) {
    if (!active) { g_cut_until = 0; return; }
    __try {
        float* w = reinterpret_cast<float*>(iface - kIfaceToDyn + kDynEngOmega);
        const float target = rpm / kRadPerSecToRpm;
        const ULONGLONG now = GetTickCount64();
        if (*w >= target) {
            if (!g_cut_until || now >= g_cut_until + 10) g_cut_until = now + 10;
            *w = target * 0.97f;
            ++g_clamps;
        } else if (g_cut_until && now < g_cut_until) {
            if (*w > target * 0.97f) *w = target * 0.97f;
        }
        if (g_verbose && now - g_limit_log > 400) {
            g_limit_log = now;
            say("hold: engine %.0f rpm (limit %.0f)", static_cast<double>(*w * kRadPerSecToRpm), static_cast<double>(rpm));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Method 4: a real throttle cut. The dynamics' input step is hooked; after it has set the throttle (dyn+0x1BD0) the throttle is forced to 0 whenever
// the engine has reached the set rpm, for at least 10 ms each time (a fuel cut limiter). The engine torque then follows the closed-throttle curve.
std::atomic<char*> g_player_dyn{nullptr};
std::atomic<bool> g_lc_on{false};
std::atomic<float> g_lc_target{0};
std::atomic<bool> g_cut_now{false};
double g_cut_until_ms = 0;
std::atomic<double> g_dump_until_ms{0};
ULONGLONG g_cut_log = 0;
double now_ms() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}
typedef uint64_t (*DynInputFn)(void*, float);
DynInputFn g_orig_dyn_input = nullptr;
uint64_t detour_dyn_input(void* self, float dt) {
    const uint64_t r = g_orig_dyn_input(self, dt);
    if ((g_method != 4 && g_method != 5) || self != g_player_dyn.load()) return r;
    __try {
        char* d       = static_cast<char*>(self);
        float* thr    = reinterpret_cast<float*>(d + kDynThrottle);
        const float w = *reinterpret_cast<const float*>(d + kDynEngOmega);
        const bool on = g_lc_on.load();
        bool cut      = false;
        if (on) {
            const double t = now_ms();
            if (w >= g_lc_target.load()) g_cut_until_ms = t + 10.0;
            cut = t < g_cut_until_ms;
        }
        if (g_verbose && (on || g_rt.load() >= 30) && GetTickCount64() - g_cut_log > 400) {
            g_cut_log = GetTickCount64();
            say("cut: rt %d  thr(1BD0) %.3f  raw(1748) %.3f  clutch(F44) %.2f lock(F48) %d  engine %.0f rpm  on %d  cut %d", g_rt.load(), static_cast<double>(*thr),
                static_cast<double>(*reinterpret_cast<const float*>(d + kDynThrRaw)), static_cast<double>(*reinterpret_cast<const float*>(d + kDynClutch)),
                static_cast<int>(*reinterpret_cast<const int*>(d + 0xF48)), static_cast<double>(w * kRadPerSecToRpm), on ? 1 : 0, cut ? 1 : 0);
        }
        if (!on && g_dump_until_ms.load() > 0 && g_rt.load() >= g_throttle_min) {
            if (now_ms() < g_dump_until_ms.load()) {
                *reinterpret_cast<float*>(d + kDynClutch) = 0.0f;   // dump the clutch
            } else {
                g_dump_until_ms.store(0);
            }
        } else if (on) {
            g_dump_until_ms.store(0);
        }
        g_cut_now.store(cut);
        if (cut) {
            if (g_method == 5) {
                *reinterpret_cast<float*>(d + kDynPowerCut) = 1.0f;   // torque cut: the throttle stays open so the turbo keeps its boost
            } else {
                *thr = 0.0f;
                *reinterpret_cast<float*>(d + kDynThrCopy) = 0.0f;
            }
            ++g_clamps;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

DynInputFn g_orig_dyn_power = nullptr;
uint64_t detour_dyn_power(void* self, float dt) {
    const uint64_t r = g_orig_dyn_power(self, dt);
    if (g_method == 5 && self == g_player_dyn.load() && g_cut_now.load() && g_lc_on.load()) {
        __try { *reinterpret_cast<float*>(static_cast<char*>(self) + kDynPowerCut) = 1.0f; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

void apply_limit(char* iface, bool active, float rpm) {
    if (g_method == 4 || g_method == 5) {
        g_player_dyn.store(iface - kIfaceToDyn);
        g_lc_target.store(rpm / kRadPerSecToRpm);
        g_lc_on.store(active);
        return;
    }
    if (g_method == 3) { hold_engine(iface, active, rpm); return; }
    if (g_method != 1) return;
    __try {
        float* maxp = reinterpret_cast<float*>(iface - kIfaceToDyn + kDynEngMax);
        const float live = *reinterpret_cast<const float*>(iface - kIfaceToDyn + kDynEngOmega);
        if (g_limit_iface && g_limit_iface != iface) g_limit_iface = nullptr;   // another car: the old one isn't touched (it may be gone)
        if (active) {
            const float target = rpm / kRadPerSecToRpm;
            if (!g_limit_iface) {
                g_orig_max    = *maxp;
                g_limit_iface = iface;
                say("limit: car's own engine speed limit is %.1f rad/s (%.0f rpm); lowering to %.0f rpm", static_cast<double>(g_orig_max),
                    static_cast<double>(g_orig_max * kRadPerSecToRpm), static_cast<double>(rpm));
            }
            if (*maxp != target) *maxp = target;
            if (g_verbose && GetTickCount64() - g_limit_log > 400) {
                g_limit_log = GetTickCount64();
                say("limit: engine %.0f rpm (limit %.0f rpm)", static_cast<double>(live * kRadPerSecToRpm), static_cast<double>(*maxp * kRadPerSecToRpm));
            }
        } else if (g_limit_iface == iface) {
            *maxp         = g_orig_max;
            g_limit_iface = nullptr;
            say("limit: put back to %.0f rpm", static_cast<double>(g_orig_max * kRadPerSecToRpm));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_limit_iface = nullptr;
    }
}

void step(char* car) {
    char* iface = nullptr;
    int gear     = -1;
    float omega  = 0;
    __try {
        iface = *reinterpret_cast<char**>(car + kOffIface);
        if (!iface) return;
        gear  = *reinterpret_cast<const uint8_t*>(iface + kIfGear) & 0x0F;
        omega = *reinterpret_cast<const float*>(iface + kIfOmega);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    probe_dyn(iface, gear, car_speed(iface), g_rt.load());
    probe_throttle(car, iface, g_rt.load());
    const bool owned   = car_has_kit();
    const uint32_t garage = g_api->upgrade_active_garage();
    const bool driving = GetTickCount64() - g_hud_tick.load() < 600;
    const int rt       = g_rt.load();
    const bool hand    = g_hand.load();
    const float speed  = car_speed(iface);
    const bool throttle = rt >= g_throttle_min;
    const bool still    = speed < g_still_mps;
    const bool first    = gear == g_first_gear;
    const bool want     = owned && driving && throttle && hand;

    // While the player holds throttle + handbrake, log why launch control is (not) active, twice a second.
    if (g_verbose && want) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_last_probe > 500) {
            g_last_probe = now;
            say("probe: garage %u gear %d (want %d) speed %.2f m/s (below %.2f: %d) omega %.1f rad/s = %.0f rpm, rt %d, active %d", garage, gear, g_first_gear,
                static_cast<double>(speed), static_cast<double>(g_still_mps), still ? 1 : 0, static_cast<double>(omega),
                static_cast<double>(omega * kRadPerSecToRpm), rt, g_active ? 1 : 0);
        }
    }

    const bool active = want && first && still;
    if (active != g_active) {
        if (g_active && !active && (g_method == 4 || g_method == 5) && throttle && !hand && first && g_dump_ms > 0) g_dump_until_ms.store(now_ms() + g_dump_ms);
        g_active = active;
        if (active) say("launch control ON: garage car %u, %.0f rpm", garage, static_cast<double>(rpm_for(garage)));
        else say("launch control OFF (%d clamps)", g_clamps);
        g_clamps = 0;
    }
    apply_limit(iface, active, rpm_for(garage));
}

// ---- hooks -----------------------------------------------------------------------------------------------------------------------
typedef uint64_t (*CarUpdateFn)(void*, float);
CarUpdateFn g_orig_car_update = nullptr;
uint64_t detour_car_update(void* self, float dt) {
    const uint64_t r = g_orig_car_update(self, dt);
    char* car        = static_cast<char*>(self);
    void* hud        = g_hud_player.load();
    if (!car || !hud || g_stop.load()) return r;
    __try {
        void* iface = *reinterpret_cast<void**>(car + kOffIface);
        if (static_cast<void*>(car) == hud || (iface && iface == hud)) step(car);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int logged = 0;
        if (logged++ < 5) say("launch control: a fault in the Car update step");
    }
    return r;
}

typedef uint64_t (*HudCtrlFn)(void*, void*, void*, void*);
HudCtrlFn g_orig_hud = nullptr;
uint64_t detour_hud(void* c, void* a, void* b, void* d) {
    const uint64_t r = g_orig_hud(c, a, b, d);
    if (c && readable(c, 0x130)) {
        void* obj = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x120);
        void* cb  = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x128);
        g_hud_player.store(cb ? obj : nullptr);
        g_hud_tick.store(GetTickCount64());
    }
    return r;
}

DWORD WINAPI install_hooks(LPVOID) {
    g_game = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!g_game) { say("the game's code never became ready: no hooks"); return 0; }
    FH3Hook* h1 = g_api->hook_function(g_mod, "Car update (Launch Control)", g_game + kRvaCarUpdate, reinterpret_cast<void*>(&detour_car_update),
                                       reinterpret_cast<void**>(&g_orig_car_update));
    FH3Hook* h2 = g_api->hook_function(g_mod, "HUD controller (Launch Control: the player's car)", g_game + kRvaHudCtrl, reinterpret_cast<void*>(&detour_hud),
                                       reinterpret_cast<void**>(&g_orig_hud));
    FH3Hook* h3 = g_api->hook_function(g_mod, "Dynamics input step (Launch Control: throttle cut)", g_game + kRvaDynInput,
                                       reinterpret_cast<void*>(&detour_dyn_input), reinterpret_cast<void**>(&g_orig_dyn_input));
    say("hook: dynamics input step %s", h3 && g_api->enable_hooks(g_mod, h3) ? "on" : "FAILED");
    if (g_method == 5) {   // only the experimental torque-cut method needs this hook
        FH3Hook* h4 = g_api->hook_function(g_mod, "Engine power reduction (Launch Control: torque cut)", g_game + kRvaDynPower,
                                           reinterpret_cast<void*>(&detour_dyn_power), reinterpret_cast<void**>(&g_orig_dyn_power));
        say("hook: power reduction %s", h4 && g_api->enable_hooks(g_mod, h4) ? "on" : "FAILED");
    }
    const bool a = h1 && g_api->enable_hooks(g_mod, h1);
    const bool b = h2 && g_api->enable_hooks(g_mod, h2);
    say("hooks: Car update %s, HUD controller %s", a ? "on" : "FAILED", b ? "on" : "FAILED");
    return 0;
}

// icons/launchcontrol.png is handed to the game as Upgrade_Parts\Custom\launchcontrol_launchcontrol_Icon.swatchbin (platform set_file); no game file changes.
const char kIconDir[]   = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\";
const char kLargeIcon[] = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\Engine_Power\\engine_power_LG_Icon.swatchbin";
std::string g_icon_path;

void install_icons() {
    if (g_api->api_version < 2 || !g_api->set_file) return;
    static const char base[] = "ui/textures/data_bound/Upgrade_Parts/";
    int ok = 0, total = 0;
    for (const IconEntry* e = k_icons; e->slot; e++) {
        char path[200];
        int n = 0;
        for (int i = 0; base[i]; i++) path[n++] = base[i];
        icon_name(e, path + n, '/');
        const unsigned size = icon_unpack(e, g_icon_buf);
        ++total;
        if (g_api->set_file(g_mod, path, g_icon_buf, size)) ++ok;
    }
    say("icons handed to the game: %d/%d", ok, total);
    IconEntry e{"launchcontrol", 0, nullptr, 0};
    char nm[160];
    icon_name(&e, nm, '\\');
    g_icon_path = std::string(kIconDir) + nm;
}

int add_upgrade() {
    install_icons();
    static const wchar_t* names[2] = {L"Stock", L"2-Step Launch Control"};
    static const wchar_t* descs[2] = {L"No launch control.",
                                      L"Hold the engine at a set RPM for a perfect start: in first gear, standing still, hold the handbrake and full throttle, "
                                      L"then let go of the handbrake. Set the RPM (1000 to 8000, 3500 by default) on the car's Tuning, Springs page, in the Launch Control row."};
    static FH3UpgradeStage stages[2];
    for (int i = 0; i < 2; ++i) stages[i] = {names[i], descs[i], i ? g_price : 0, i && !g_icon_path.empty() ? g_icon_path.c_str() : nullptr, i && !g_icon_path.empty() ? kLargeIcon : nullptr};
    FH3UpgradeDef d{};
    d.size           = sizeof d;
    d.id             = "launchcontrol.twostep";
    d.name           = L"2-Step Launch Control";
    d.description    = L"Holds the engine at a set RPM while you hold the handbrake and full throttle in first gear, for a launch from that RPM.";
    d.icon           = g_icon_path.empty() ? nullptr : g_icon_path.c_str();
    d.icon_large     = g_icon_path.empty() ? nullptr : kLargeIcon;
    d.area           = 1;   // Engine
    d.stage_count    = 2;
    d.stages         = stages;
    d.preferred_slot = -1;
    return g_api->register_upgrade(g_mod, &d);
}

}  // namespace

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 11) return 1;
    g_api = api;

    FH3ModInfo info{};
    info.size             = sizeof info;
    info.name             = "Launch Control";
    info.version          = LAUNCHCONTROL_VERSION;
    info.github_repo      = "Levilente/FH3_LaunchControl";
    info.platform_version = FH3MOD_PLATFORM_VERSION;
    info.check_updates    = 1;
    info.description      = "2-step launch control: hold the engine at a set RPM in first gear with the handbrake and full throttle.";
    info.offline_only     = 1;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;

    g_cfg_path = std::wstring(api->azimuth_dir) + L"\\fh3-mods\\LaunchControl.ini";
    load_config();

    g_handle = add_upgrade();
    say("registered: upgrade handle %d (price %d, default %d rpm, first gear = %d, method %d)", g_handle, g_price, g_default_rpm, g_first_gear, g_method);
    if (g_handle < 0) return 1;

    if (api->api_version >= 13 && api->add_tuning_row) {
        FH3TuningRow row{};
        row.size     = sizeof row;
        row.key      = "Rpm";
        row.label    = "Launch Control|2-Step RPM";
        row.help     = "The RPM the engine is held at while you hold the handbrake and full throttle in first gear, standing still. Let go of the handbrake to launch.";
        row.min      = 1000;
        row.max      = 8000;
        row.step     = 100;
        row.decimals = 0;
        row.get      = row_get;
        row.set      = row_set;
        row.visible  = row_visible;
        row.user     = nullptr;
        row.page     = "Engine";   // API 17: the ENGINE tab (older platforms: Springs)
        say("tuning row: %s", api->add_tuning_row(g_mod, &row) ? "added" : "NOT added (see the platform's lines above)");
    } else {
        say("this platform (API %u) can't add the tuning slider (needs API 13); the 2-step RPM stays at %d", api->api_version, g_default_rpm);
    }

    g_threads[0] = CreateThread(nullptr, 0, install_hooks, nullptr, 0, nullptr);
    g_threads[1] = CreateThread(nullptr, 0, input_thread, nullptr, 0, nullptr);
    return 0;
}

// Hot reload (platform API 16, see the platform's docs/writing-a-mod.md): called before a rebuilt launchcontrol.dll replaces this one.
// Stops the threads, lets go of the car, and puts back what the mod changed in it; the platform then switches the hooks off.
FH3MOD_EXPORT void fh3mod_shutdown(void) {
    g_stop.store(true);
    for (HANDLE& h : g_threads)
        if (h) {
            WaitForSingleObject(h, 3000);
            CloseHandle(h);
            h = nullptr;
        }
    g_lc_on.store(false);
    g_active = false;
    if (g_limit_iface) {   // method 1 lowered the engine's own speed limit
        __try { *reinterpret_cast<float*>(g_limit_iface - kIfaceToDyn + kDynEngMax) = g_orig_max; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_limit_iface = nullptr;
    }
}

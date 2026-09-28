// SPDX-License-Identifier: AGPL-3.0-or-later
//
// svcjcinavi compass hook — keep the instrument-cluster compass alive.
//
// The compass is gated on one static flag, s_bCompassAllowed (.bss
// 0xAAD4C): written only by enableCompass @0x62720 / disableCompass
// @0x627DC, read only by thInitLDSPositionUpdate @0x51FD4 (8 sites),
// which when set publishes the heading to the cluster through
// VBS_NAVI_TMC_NaviCompassVal and to the screen through
// NAVI2IHU_CurrentLocationInfo. The flag simply follows the byte
// gs_vbs_bcm_TouchDisplayCarSpeedThr (libjcimod_bcm.so 0x164BC) that the
// two callbacks we wrap carry — both are `value ? enableCompass() :
// disableCompass()`. That byte is zero, and the compass dead, in two
// separate situations:
//
//  1. Below ~9 km/h. It is JCI PID 0xD909, not a CAN signal: the VIP
//     derives it from road speed — 1 at >= 9.00, 0 at <= 7.00 km/h,
//     hysteresis between — so stock the compass only runs while driving.
//  2. At any speed once NVRAM bus_bcm_speed_restriction is "disable".
//     VBS_BCM_ReceiveHandler (libjcimod_bcm 0x86C4, site 0x8E50) sends
//     `getSpeedRestconfigNVRAM() == 1 ? raw : 0`, and every touchscreen-
//     while-driving tweak sets that key, pinning the byte to 0 for good.
//
// So without this hook you get one or the other: a compass, or a
// touchscreen and nav app usable while moving. Forcing the byte non-zero
// here removes the choice — the key can stay "disable" and the compass
// still runs. It has to be forced HERE rather than at the shared
// producer — the same byte feeds blmjciaapa, blmjcicarplay, libjciuiadvd
// and svcjcilvds_blm, so patching libjcimod_bcm would re-arm the driving
// restriction in all of them, whereas inside svcjcinavi these two
// signals have exactly one consumer each and it is the compass gate.
// VBS_BCM_CarSpeedThresholdNotification_enable and
// VBS_BCM_GetCarSpeedThrshld are left alone for the same reason: they
// are the OEM nav app's own lockout (updateCarSpeedThresholdToNNG
// @0x62664), and must keep reporting 0 so it stays usable while moving.
//
// Parked (confirmed on-car) there is no valid GPS heading, so
// updateCompassDirection @0x67418 falls through its 11.25-degree bucket
// table (.data 0xAA290) and publishes sentinel 16. The cluster renders
// that as "N" rather than blanking — same as the OEM nav app — so the
// sentinel needs no suppression.
//
// Both hooked symbols are R_ARM_JUMP_SLOT imports resolved from
// libjcivbsbcmclient.so, so an LD_PRELOAD definition binds svcjcinavi's
// calls to ours — the mechanism merge.cpp uses for the HUD setters.

#define LOG_TAG "COMPASS"
#include "log.h"
#include "compass.h"
#include "common/preload.h"

#include <dlfcn.h>
#include <stdint.h>

// Callback shape read off the callee, not guessed:
// ..._dbus_async_callback @libjcivbsbcmclient.so:0x33CC calls back with
// (r0 = Dbus_conn_sh*, r1 = the byte, r2 = user data) and svcjcinavi's
// handlers @0x62900/0x62940 agree. Return value is ignored.
typedef void (*BcmByteCb)(void *conn, uint8_t value, void *user_data);

// Our exported PLT shadows, forward-declared so ensure_gate() can take
// their addresses for the resolve_real_symbol self-loop guard.
extern "C" PRELOAD_EXPORT int
VBS_BCM_NoSpeedRestrict_TouchDisplay_enable(void *conn, int enable,
                                            BcmByteCb cb, void *user_data);
extern "C" PRELOAD_EXPORT int
VBS_BCM_GetTouchDisplayCarSpeedThrshld(void *conn, int flag,
                                       BcmByteCb cb, void *user_data);

namespace {

// Self-gate: only act inside the navigation service PID.
constexpr const char *kSvcjcinaviSo = "/jci/navi/svcjcinavi.so";

// Where the real implementations live (DT_NEEDED of svcjcinavi.so).
constexpr const char *kBcmClientSoname  = "libjcivbsbcmclient.so";
constexpr const char *kBcmClientAbspath = "/jci/lib/libjcivbsbcmclient.so";

// svcjcinavi only tests this against zero, so any non-zero is
// equivalent. Genuine non-zero readings pass through UNCHANGED.
constexpr uint8_t kForcedValue = 1;

typedef int (*EnableFn)(void *, int, BcmByteCb, void *);
typedef int (*GetFn)(void *, int, BcmByteCb, void *);

bool g_armed     = false;   // compass_init() ran (config gate passed)
bool g_gate_done = false;
bool g_enabled   = false;   // armed AND svcjcinavi.so mapped here

void    *g_bcm_handle  = nullptr;
EnableFn g_real_enable = nullptr;
GetFn    g_real_get    = nullptr;

// One slot per entry point; its address is the user_data we hand the
// real function, so one trampoline serves both and still recovers the
// right caller's callback.
struct Subscription {
    BcmByteCb   cb;
    void       *user_data;
    const char *what;
};

Subscription g_signal = { nullptr, nullptr, "NoSpeedRestrict_TouchDisplay" };
Subscription g_getter = { nullptr, nullptr, "GetTouchDisplayCarSpeedThrshld" };

void trampoline(void *conn, uint8_t value, void *user_data)
{
    Subscription *sub = static_cast<Subscription *>(user_data);
    if (sub == nullptr || sub->cb == nullptr) {
        // Dropping a live BCM notification is the one place the compass
        // gate silently loses its input, so this stays release-visible
        // (LOGE) — LOGW is compiled out under NDEBUG. Name the slot when
        // we have one so the log says WHICH subscription dropped.
        LOGE("callback fired with no recorded OEM callback — dropping (%s)",
             sub != nullptr ? sub->what : "unknown subscription");
        return;
    }

    const uint8_t out = (value == 0) ? kForcedValue : value;
    LOGV("%s: %u -> %u", sub->what, (unsigned)value, (unsigned)out);
    sub->cb(conn, out, sub->user_data);
}

// Unsynchronised by design: svcjcinavi issues both subscriptions from
// initializeSettings, sequentially on one thread, and the disassembly
// shows exactly one call site each. Nothing here is reentered, so the
// latch and the slot stores need no barrier. Revisit if a second caller
// ever appears.
void ensure_gate()
{
    if (g_gate_done) {
        return;
    }
    g_gate_done = true;

    void *h = dlopen(kSvcjcinaviSo, RTLD_NOW | RTLD_NOLOAD);
    const bool in_navi_pid = (h != nullptr);
    if (h != nullptr) {
        dlclose(h);   // we only wanted the "is it mapped" boolean
    }

    g_enabled = g_armed && in_navi_pid;

    // Resolve UNCONDITIONALLY, even in the wrong process or with the
    // feature off: g_enabled gates only the rewriting, and the disabled
    // path still has to forward. Same rule as merge.cpp.
    g_real_enable = reinterpret_cast<EnableFn>(resolve_real_symbol(
        "VBS_BCM_NoSpeedRestrict_TouchDisplay_enable",
        kBcmClientSoname, kBcmClientAbspath,
        reinterpret_cast<void *>(&VBS_BCM_NoSpeedRestrict_TouchDisplay_enable),
        &g_bcm_handle));
    if (g_real_enable == nullptr) {
        LOGC("could not resolve real "
             "VBS_BCM_NoSpeedRestrict_TouchDisplay_enable — the compass "
             "subscription will not be established this session");
    }

    g_real_get = reinterpret_cast<GetFn>(resolve_real_symbol(
        "VBS_BCM_GetTouchDisplayCarSpeedThrshld",
        kBcmClientSoname, kBcmClientAbspath,
        reinterpret_cast<void *>(&VBS_BCM_GetTouchDisplayCarSpeedThrshld),
        &g_bcm_handle));
    if (g_real_get == nullptr) {
        LOGC("could not resolve real VBS_BCM_GetTouchDisplayCarSpeedThrshld");
    }

    // The armed case is the only one a stock build's log must be able to
    // answer — "did the hook load and arm?" — so it alone is logged at
    // release-visible (LOGE) level. This LOGE is a deliberate status
    // line, NOT an error; do not "downgrade" it to LOGD (invisible under
    // NDEBUG) to match merge.cpp. If resolution only partially
    // succeeded, say so rather than printing ENABLED next to a null %p.
    if (g_enabled) {
        if (g_real_enable != nullptr && g_real_get != nullptr) {
            LOGE("compass gate: ENABLED — NoSpeedRestrict_enable=%p, "
                 "GetTouchDisplayCarSpeedThrshld=%p",
                 reinterpret_cast<void *>(g_real_enable),
                 reinterpret_cast<void *>(g_real_get));
        } else {
            LOGE("compass gate: armed but PARTIALLY resolved — "
                 "NoSpeedRestrict_enable=%p, GetTouchDisplayCarSpeedThrshld=%p",
                 reinterpret_cast<void *>(g_real_enable),
                 reinterpret_cast<void *>(g_real_get));
        }
    } else {
        LOGD("compass gate: transparent passthrough (armed=%d, "
             "svcjcinavi.so %s)", (int)g_armed,
             in_navi_pid ? "mapped" : "NOT in this pid");
    }
}

} // namespace

// Load-time arming; see compass.h for why nothing is resolved here.
void compass_init(void)
{
    g_armed = true;
    LOGD("armed — gate + symbol resolution deferred to the first "
         "interposed call");
}

// === Interposed entry points ==================================

extern "C" PRELOAD_EXPORT int
VBS_BCM_NoSpeedRestrict_TouchDisplay_enable(void *conn, int enable,
                                            BcmByteCb cb, void *user_data)
{
    ensure_gate();

    // Deliberately -1 (not merge.cpp's 0): these VBS_BCM_* calls return
    // an int status the OEM caller can test, so an explicit failure code
    // is the truthful answer when the real impl is unreachable. merge.cpp
    // returns 0 because its HUD setters are treated as best-effort void.
    // The one-shot LOGC in ensure_gate() already recorded the resolution
    // failure at release-visible level.
    if (g_real_enable == nullptr) {
        return -1;
    }
    // A null cb is meaningful to the callee — jcidbus_signal_enable
    // @0x2540 stores it only `strne`, i.e. keeps whatever callback the
    // record already holds. Substituting our never-null trampoline would
    // hijack that registration and then drop every notification, so pass
    // these through untouched.
    if (!g_enabled || cb == nullptr) {
        return g_real_enable(conn, enable, cb, user_data);
    }

    // enable == 0 is an unsubscribe: let the OEM cancel the registration
    // it thinks it made, then forget our record — clearing first would
    // drop any notification still in flight during the teardown.
    if (enable == 0) {
        LOGD("unsubscribe NoSpeedRestrict_TouchDisplay");
        const int rc       = g_real_enable(conn, enable, cb, user_data);
        g_signal.cb        = nullptr;
        g_signal.user_data = nullptr;
        return rc;
    }

    if (g_signal.cb != nullptr) {
        // Re-subscription violates the "exactly one call site" invariant
        // the no-lock design rests on, so keep it release-visible (LOGE):
        // the old callback is being dropped and we want that in a stock
        // build's log if it ever happens.
        LOGE("NoSpeedRestrict_TouchDisplay re-subscribed — replacing the "
             "recorded callback %p with %p",
             reinterpret_cast<void *>(g_signal.cb),
             reinterpret_cast<void *>(cb));
    }
    g_signal.cb        = cb;
    g_signal.user_data = user_data;

    LOGD("subscribing NoSpeedRestrict_TouchDisplay behind our trampoline "
         "(oem cb=%p ud=%p)", reinterpret_cast<void *>(cb), user_data);

    return g_real_enable(conn, enable, &trampoline, &g_signal);
}

extern "C" PRELOAD_EXPORT int
VBS_BCM_GetTouchDisplayCarSpeedThrshld(void *conn, int flag,
                                       BcmByteCb cb, void *user_data)
{
    ensure_gate();

    // -1 for the same reason as the enable path above; failure already
    // logged at release-visible level in ensure_gate().
    if (g_real_get == nullptr) {
        return -1;
    }
    // Null cb: the async dispatcher @0x33f4 skips the callback entirely
    // on null, so there is nothing for a trampoline to stand in for.
    if (!g_enabled || cb == nullptr) {
        return g_real_get(conn, flag, cb, user_data);
    }

    // One-shot getter, issued once from initializeSettings, so a single
    // slot cannot be clobbered by an overlapping request.
    g_getter.cb        = cb;
    g_getter.user_data = user_data;

    LOGD("GetTouchDisplayCarSpeedThrshld behind our trampoline "
         "(oem cb=%p ud=%p)", reinterpret_cast<void *>(cb), user_data);

    return g_real_get(conn, flag, &trampoline, &g_getter);
}

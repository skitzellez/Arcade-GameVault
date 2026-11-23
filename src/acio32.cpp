// Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
// Konami's libacio.dll (the e-Amusement I/O board, card reader and security plugs) for 32-bit games: every device
// is there and ready, the plugs check out, coins come from the controller's coin button. Behaviour follows
// TeknoParrot's OpenParrotKonamiLoader (libacio.cpp, GPL-3.0). Contra: Evolution imports these by their C++ names.
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "peload.h"

namespace arcade {
int CoinPresses();   // arcade_io.cpp: coin button presses so far

#define P32(p) ((uint8_t*)(uintptr_t)(uint32_t)(p))
static uint32_t g_coinsTaken = 0;
static uint64_t A_true() { return 1; }
static uint64_t A_true1(uint64_t) { return 1; }
static uint64_t A_true2(uint64_t, uint64_t) { return 1; }
static uint64_t A_true4(uint64_t, uint64_t, uint64_t, uint64_t) { return 1; }
static uint64_t A_void1(uint64_t) { return 0; }
static uint64_t A_finished2(uint64_t, uint64_t out) {   // "..._isfinished(device, int* state)": done
    if (out) *(uint32_t*)P32(out) = 1;
    return 1;
}
static uint64_t A_ids(uint64_t out) {   // software / system plug ID: "FFFF FFFF FFFF FFFF"
    if (out) memset(P32(out), 'F', 16);
    return 1;
}
static uint64_t A_uid(uint64_t, uint64_t out) {   // no card on the reader
    if (out) memset(P32(out), 0, 8);
    return 1;
}
static uint64_t A_statusBuffer(uint64_t out) {   // ac_io_hbhi_ty_input_access: nothing pressed (the game reads input itself)
    if (out) memset(P32(out), 0, 0x54);
    return 0;
}
static uint64_t A_active2(uint64_t, uint64_t out) {
    if (out) *(uint32_t*)P32(out) = 6;
    return 1;
}
static uint32_t CoinsWaiting() {
    uint32_t pressed = (uint32_t)CoinPresses();
    return pressed > g_coinsTaken ? pressed - g_coinsTaken : 0;
}
static uint64_t A_currentCoins(uint64_t, uint64_t out) {
    if (out) *(uint32_t*)P32(out) = CoinsWaiting();
    return 1;
}
static uint64_t A_consumeCoins(uint64_t, uint64_t n) {
    uint32_t have = CoinsWaiting(), take = (uint32_t)n ? (uint32_t)n : have;
    if (take > have) take = have;
    g_coinsTaken += take;
    return take ? 1 : 0;
}

struct AcioFn {
    const char* name;   // the C++ name the game imports
    int args;
    void* fn;
};
static const AcioFn kAcio[] = {
    {"?ac_io_begin@@YA_NEPBDPAHH@Z", 4, (void*)A_true4},
    {"?ac_io_begin_get_status@@YA_NH@Z", 1, (void*)A_true1},
    {"?ac_io_is_active2@@YA_NHPAH@Z", 2, (void*)A_active2},
    {"?ac_io_end@@YAXXZ", 0, (void*)A_true},
    {"?ac_io_end_get_status@@YAXXZ", 0, (void*)A_true},
    {"?ac_io_update@@YAXXZ", 0, (void*)A_true},
    {"?ac_io_enable_game_ios@@YA_N_N@Z", 1, (void*)A_true1},
    {"?ac_io_is_active_device@@YA_NW4ac_io_device_index@@@Z", 1, (void*)A_true1},
    {"?ac_io_set_soft_watch_dog@@YAXH@Z", 1, (void*)A_void1},
    {"?ac_io_soft_watch_dog_on@@YAXXZ", 0, (void*)A_true},
    {"?ac_io_soft_watch_dog_off@@YAXXZ", 0, (void*)A_true},
    {"?ac_io_hbhi_watchdog_off@@YAXXZ", 0, (void*)A_true},
    {"?ac_io_secplug_set_encodedpasswd@@YAXPBEH@Z", 2, (void*)A_true2},
    {"?ac_io_hbhi_req_secplug_check_softwareplug@@YA_NPBD@Z", 1, (void*)A_true1},
    {"?ac_io_hbhi_req_secplug_check_systemplug@@YA_NXZ", 0, (void*)A_true},
    {"?ac_io_hbhi_req_secplug_check_isfinished@@YA_NPAW4AC_IO_HBHI_SECPLUG_CHECK@@@Z", 1, (void*)A_true1},
    {"?ac_io_hbhi_get_softwareid@@YA_NPAD@Z", 1, (void*)A_ids},
    {"?ac_io_hbhi_get_systemid@@YA_NPAD@Z", 1, (void*)A_ids},
    {"?ac_io_hbhi_update_control_status_buffer@@YA_NXZ", 0, (void*)A_true},
    {"?ac_io_hbhi_get_control_status_buffer@@YAXPAUac_io_hbhi_ty_input_access@@@Z", 1, (void*)A_statusBuffer},
    {"?ac_io_hbhi_control_lamp_on@@YAXH@Z", 1, (void*)A_void1},
    {"?ac_io_hbhi_control_lamp_off@@YAXH@Z", 1, (void*)A_void1},
    {"?ac_io_hbhi_control_parallel_on@@YAXH@Z", 1, (void*)A_void1},
    {"?ac_io_hbhi_control_parallel_off@@YAXH@Z", 1, (void*)A_void1},
    {"?ac_io_hbhi_lock_coincounter@@YA_NW4AC_IO_HBHI_COIN_SLOT@@@Z", 1, (void*)A_true1},
    {"?ac_io_hbhi_unlock_coincounter@@YA_NW4AC_IO_HBHI_COIN_SLOT@@@Z", 1, (void*)A_true1},
    {"?ac_io_hbhi_control_coin_blocker_open@@YAXW4AC_IO_HBHI_COIN_SLOT@@@Z", 1, (void*)A_void1},
    {"?ac_io_hbhi_control_coin_blocker_close@@YAXW4AC_IO_HBHI_COIN_SLOT@@@Z", 1, (void*)A_void1},
    {"?ac_io_hbhi_current_coinstock@@YA_NW4AC_IO_HBHI_COIN_SLOT@@PAI@Z", 2, (void*)A_currentCoins},
    {"?ac_io_hbhi_consume_coinstock@@YA_NW4AC_IO_HBHI_COIN_SLOT@@I@Z", 2, (void*)A_consumeCoins},
    {"?ac_io_icca_cardunit_init@@YA_NW4ac_io_device_index@@@Z", 1, (void*)A_true1},
    {"?ac_io_icca_cardunit_init_isfinished@@YA_NW4ac_io_device_index@@PAH@Z", 2, (void*)A_finished2},
    {"?ac_io_icca_req_uid@@YA_NW4ac_io_device_index@@@Z", 1, (void*)A_true1},
    {"?ac_io_icca_req_uid_isfinished@@YA_NW4ac_io_device_index@@PAH@Z", 2, (void*)A_finished2},
    {"?ac_io_icca_get_uid@@YA_NW4ac_io_device_index@@PAUac_io_icca_ty_uid@@@Z", 2, (void*)A_uid},
};
// libacio.dll's functions (all __cdecl), else null
void* FindAcio32(const char* dll, const char* func, int& args) {
    if (_stricmp(dll, "libacio.dll")) return nullptr;
    for (const AcioFn& f : kAcio)
        if (!strcmp(f.name, func)) {
            args = f.args;
            return f.fn;
        }
    if (!strncmp(func, "?ac_io_", 7)) {   // one not listed: "done" (logged once each by the caller as bridged)
        args = 0;
        return (void*)A_true;
    }
    return nullptr;
}
}  // namespace arcade

// test program: reads the first controller with XInput for 2 s (like a PC arcade game's input code)
#include <windows.h>
#include <xinput.h>
#include <cstdio>
#pragma comment(lib, "xinput.lib")
int main() {
    char buf[128];
    XINPUT_STATE st = {};
    DWORD r = XInputGetState(0, &st);
    sprintf_s(buf, "XInputGetState(0): %lu, packet %lu, buttons 0x%04x", r, st.dwPacketNumber, st.Gamepad.wButtons);
    OutputDebugStringA(buf);
    XINPUT_CAPABILITIES caps = {};
    r = XInputGetCapabilities(0, 0, &caps);
    sprintf_s(buf, "XInputGetCapabilities(0): %lu, subtype %u", r, caps.SubType);
    OutputDebugStringA(buf);
    return 0;
}

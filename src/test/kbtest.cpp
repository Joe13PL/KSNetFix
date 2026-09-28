// Standalone check of the KeyToChar translation core used by KSNetFix.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
static UINT LayoutCodePage(HKL hkl) {
    char buf[16];
    if (GetLocaleInfoA(MAKELCID(LOWORD((UINT_PTR)hkl), SORT_DEFAULT), LOCALE_IDEFAULTANSICODEPAGE, buf, sizeof(buf)) > 0) return (UINT)atoi(buf);
    return CP_ACP;
}
static void Try(const char* name, UINT dik, bool shift, bool altgr, bool caps) {
    UINT scan = dik & 0x7F | (dik & 0x80) << 1;
    UINT vk = MapVirtualKeyA(scan, 1);
    BYTE ks[256] = {0};
    if (shift) { ks[VK_LSHIFT] = 0x80; ks[VK_SHIFT] = 0x80; }
    if (altgr) { ks[VK_RMENU] = 0x80; ks[VK_LCONTROL] = 0x80; ks[VK_CONTROL] = 0x80; ks[VK_MENU] = 0x80; }
    if (caps) ks[VK_CAPITAL] = 1;
    ks[vk & 0xFF] |= 0x80;
    HKL hkl = GetKeyboardLayout(0);
    WCHAR w[8]; char mb[8] = {0};
    int n = ToUnicodeEx(vk, scan, ks, w, 8, 4, hkl);
    int m = n > 0 ? WideCharToMultiByte(LayoutCodePage(hkl), 0, w, n, mb, sizeof(mb), 0, 0) : 0;
    printf("%-14s dik=%02X vk=%02X n=%d cp=%u bytes=", name, dik, vk, n, LayoutCodePage(hkl));
    for (int i = 0; i < m; i++) printf("%02X ", (unsigned char)mb[i]);
    printf(" U+%04X\n", n > 0 ? w[0] : 0);
}
int main() {
    printf("layout %p\n", GetKeyboardLayout(0));
    Try("a", 0x1E, 0, 0, 0); Try("Shift+a", 0x1E, 1, 0, 0); Try("AltGr+a", 0x1E, 0, 1, 0);
    Try("AltGr+Shift+s", 0x1F, 1, 1, 0); Try("Caps+z", 0x2C, 0, 0, 1); Try("AltGr+z", 0x2C, 0, 1, 0);
    Try("1", 0x02, 0, 0, 0); Try("Shift+1", 0x02, 1, 0, 0); Try("space", 0x39, 0, 0, 0);
    Try("Enter", 0x1C, 0, 0, 0); Try("Backspace", 0x0E, 0, 0, 0); Try("Left arrow", 0xCB, 0, 0, 0);
    Try("AltGr+o", 0x18, 0, 1, 0); Try("AltGr+l", 0x26, 0, 1, 0);
}

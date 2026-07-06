/*
 * keymap.h - translate Windows virtual-key codes to X11 keysyms for RFB.
 * TempleOS is US-ASCII and keyboard-centric; this covers the practical set.
 */
#ifndef TOSHL_KEYMAP_H
#define TOSHL_KEYMAP_H

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

/* X11 keysyms for the non-printable keys TempleOS cares about. */
#define XK_BackSpace 0xFF08
#define XK_Tab       0xFF09
#define XK_Return    0xFF0D
#define XK_Escape    0xFF1B
#define XK_Home      0xFF50
#define XK_Left      0xFF51
#define XK_Up        0xFF52
#define XK_Right     0xFF53
#define XK_Down      0xFF54
#define XK_PageUp    0xFF55
#define XK_PageDown  0xFF56
#define XK_End       0xFF57
#define XK_Delete    0xFFFF
#define XK_F1        0xFFBE   /* F1..F12 are contiguous from here */
#define XK_Shift_L   0xFFE1
#define XK_Control_L 0xFFE3
#define XK_Alt_L     0xFFE9

/*
 * Map a WM_KEY* event to a keysym. `vk` = wParam, `shift` = current shift
 * state (for producing correct ASCII).
 * Returns 0 if the key should be ignored.
 */
static uint32_t vk_to_keysym(WPARAM vk, bool shift) {
    if (vk >= 'A' && vk <= 'Z')
        return shift ? vk : (vk + 32);            /* letters */
    if (vk >= '0' && vk <= '9') {
        if (!shift) return vk;
        static const char sym[] = ")!@#$%^&*(";
        return (uint32_t)sym[vk - '0'];
    }
    if (vk >= VK_F1 && vk <= VK_F12) return XK_F1 + (vk - VK_F1);

    switch (vk) {
    case VK_SPACE:  return ' ';
    case VK_RETURN: return XK_Return;
    case VK_BACK:   return XK_BackSpace;
    case VK_TAB:    return XK_Tab;
    case VK_ESCAPE: return XK_Escape;
    case VK_DELETE: return XK_Delete;
    case VK_HOME:   return XK_Home;
    case VK_END:    return XK_End;
    case VK_PRIOR:  return XK_PageUp;
    case VK_NEXT:   return XK_PageDown;
    case VK_LEFT:   return XK_Left;
    case VK_RIGHT:  return XK_Right;
    case VK_UP:     return XK_Up;
    case VK_DOWN:   return XK_Down;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return XK_Shift_L;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return XK_Control_L;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return XK_Alt_L;

    /* punctuation (US layout) */
    case VK_OEM_1:      return shift ? ':' : ';';
    case VK_OEM_2:      return shift ? '?' : '/';
    case VK_OEM_3:      return shift ? '~' : '`';
    case VK_OEM_4:      return shift ? '{' : '[';
    case VK_OEM_5:      return shift ? '|' : '\\';
    case VK_OEM_6:      return shift ? '}' : ']';
    case VK_OEM_7:      return shift ? '"' : '\'';
    case VK_OEM_MINUS:  return shift ? '_' : '-';
    case VK_OEM_PLUS:   return shift ? '+' : '=';
    case VK_OEM_COMMA:  return shift ? '<' : ',';
    case VK_OEM_PERIOD: return shift ? '>' : '.';
    }
    return 0;
}

#endif /* TOSHL_KEYMAP_H */

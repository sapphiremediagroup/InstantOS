#pragma once

#include <stdint.h>
#include <cpu/syscall/syscall.hpp>

// Pure key translation shared by the keyboard drivers (no hardware, no state):
// PS/2 scancodes to KeyCodes, which keys repeat, the keypad, and the text keys
// type on a US keyboard.
namespace keymap {

// Scan code set 1 (what the i8042 hands over with translation on), without
// the E0 prefix, to a KeyCode; KeyNone for codes it does not know.
inline uint16_t fromPs2Set1(uint8_t scancode) {
    static constexpr uint8_t kTable[0x59] = {
        0, KeyEscape, Key1, Key2, Key3, Key4, Key5, Key6, Key7, Key8, Key9, Key0,
        KeyMinus, KeyEqual, KeyBackspace, KeyTab,
        KeyQ, KeyW, KeyE, KeyR, KeyT, KeyY, KeyU, KeyI, KeyO, KeyP,
        KeyLeftBracket, KeyRightBracket, KeyEnter, KeyLeftControl,
        KeyA, KeyS, KeyD, KeyF, KeyG, KeyH, KeyJ, KeyK, KeyL,
        KeySemicolon, KeyApostrophe, KeyGrave, KeyLeftShift, KeyBackslash,
        KeyZ, KeyX, KeyC, KeyV, KeyB, KeyN, KeyM, KeyComma, KeyPeriod, KeySlash,
        KeyRightShift, KeypadAsterisk, KeyLeftAlt, KeySpace, KeyCapsLock,
        KeyF1, KeyF2, KeyF3, KeyF4, KeyF5, KeyF6, KeyF7, KeyF8, KeyF9, KeyF10,
        KeyNumLock, KeyScrollLock,
        Keypad7, Keypad8, Keypad9, KeypadMinus, Keypad4, Keypad5, Keypad6, KeypadPlus,
        Keypad1, Keypad2, Keypad3, Keypad0, KeypadPeriod,
        0, 0, KeyNonUsBackslash, KeyF11, KeyF12,
    };
    return scancode < sizeof(kTable) ? kTable[scancode] : static_cast<uint16_t>(KeyNone);
}

// The same for the codes that follow an E0 prefix. The "fake shifts" some
// keyboards wrap around their extended keys (E0 2A, E0 36) are not keys.
inline uint16_t fromPs2Set1Extended(uint8_t scancode) {
    switch (scancode) {
    case 0x1C: return KeypadEnter;
    case 0x1D: return KeyRightControl;
    case 0x35: return KeypadSlash;
    case 0x37: return KeyPrintScreen;
    case 0x38: return KeyRightAlt;
    case 0x47: return KeyHome;
    case 0x48: return KeyUp;
    case 0x49: return KeyPageUp;
    case 0x4B: return KeyLeft;
    case 0x4D: return KeyRight;
    case 0x4F: return KeyEnd;
    case 0x50: return KeyDown;
    case 0x51: return KeyPageDown;
    case 0x52: return KeyInsert;
    case 0x53: return KeyDelete;
    case 0x5B: return KeyLeftSuper;
    case 0x5C: return KeyRightSuper;
    case 0x5D: return KeyApplication;
    default: return KeyNone;
    }
}

inline bool isModifierKey(uint16_t key) {
    return key >= KeyLeftControl && key <= KeyRightSuper;
}

// Keys that do not repeat while held: modifiers, the locks, Print Screen and
// Pause.
inline bool repeats(uint16_t key) {
    return !isModifierKey(key) && key != KeyCapsLock && key != KeyNumLock &&
           key != KeyScrollLock && key != KeyPrintScreen && key != KeyPause && key != KeyNone;
}

// With Num Lock off the keypad's digits and point are its navigation keys.
inline uint16_t keypadNavigation(uint16_t key) {
    switch (key) {
    case Keypad1: return KeyEnd;
    case Keypad2: return KeyDown;
    case Keypad3: return KeyPageDown;
    case Keypad4: return KeyLeft;
    case Keypad6: return KeyRight;
    case Keypad7: return KeyHome;
    case Keypad8: return KeyUp;
    case Keypad9: return KeyPageUp;
    case Keypad0: return KeyInsert;
    case KeypadPeriod: return KeyDelete;
    default: return key;
    }
}

// The character a keypad key types (with Num Lock on for the digits), 0 if
// none.
inline char keypadText(uint16_t key) {
    switch (key) {
    case KeypadSlash: return '/';
    case KeypadAsterisk: return '*';
    case KeypadMinus: return '-';
    case KeypadPlus: return '+';
    case Keypad0: return '0';
    case KeypadPeriod: return '.';
    default:
        return key >= Keypad1 && key <= Keypad9 ? static_cast<char>('1' + (key - Keypad1)) : 0;
    }
}

// The character a key types on a US layout, 0 if none. Caps Lock shifts the
// letters only.
inline char usText(uint16_t key, bool shift, bool capsLock) {
    if (key >= KeyA && key <= KeyZ) {
        return static_cast<char>((shift != capsLock ? 'A' : 'a') + (key - KeyA));
    }
    static constexpr char kPlain[] = "1234567890\0\0\0\0 -=[]\\\0;'`,./";
    static constexpr char kShifted[] = "!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?";
    if (key >= Key1 && key <= KeySlash) {
        return (shift ? kShifted : kPlain)[key - Key1];
    }
    if (key == KeyNonUsBackslash) {
        return shift ? '|' : '\\';
    }
    return 0;
}

// How xterm encodes a key that types nothing: ESC [ or ESC O and a final
// letter (cursor keys, Home/End, F1-F4), or ESC [ number ~ (the editing block,
// F5-F12). final is 0 for keys without a sequence.
struct VtKey {
    char final;
    uint8_t number;
    bool ss3;
};

inline VtKey vtKey(uint16_t key) {
    switch (key) {
    case KeyUp: return {'A', 0, false};
    case KeyDown: return {'B', 0, false};
    case KeyRight: return {'C', 0, false};
    case KeyLeft: return {'D', 0, false};
    case KeyHome: return {'H', 0, false};
    case KeyEnd: return {'F', 0, false};
    case KeyInsert: return {'~', 2, false};
    case KeyDelete: return {'~', 3, false};
    case KeyPageUp: return {'~', 5, false};
    case KeyPageDown: return {'~', 6, false};
    case KeyF1: return {'P', 0, true};
    case KeyF2: return {'Q', 0, true};
    case KeyF3: return {'R', 0, true};
    case KeyF4: return {'S', 0, true};
    case KeyF5: return {'~', 15, false};
    case KeyF6: return {'~', 17, false};
    case KeyF7: return {'~', 18, false};
    case KeyF8: return {'~', 19, false};
    case KeyF9: return {'~', 20, false};
    case KeyF10: return {'~', 21, false};
    case KeyF11: return {'~', 23, false};
    case KeyF12: return {'~', 24, false};
    default: return {0, 0, false};
    }
}

// The bytes a read of the console gets for a key press, as xterm sends them:
// the text, Enter/Backspace/Tab/Escape as their control characters, Ctrl+key
// as its C0 control, Alt as an ESC before the key, and the sequences above,
// with Shift/Alt/Ctrl as xterm's modifier parameter (ESC [ 1 ; 5 D is
// Ctrl+Left). Returns the length (at most 8; 0 types nothing).
inline uint32_t consoleBytes(uint16_t key, char text, uint16_t modifiers, char* out) {
    const bool shift = (modifiers & KeyModifierShift) != 0;
    const bool ctrl = (modifiers & KeyModifierControl) != 0;
    const bool alt = (modifiers & KeyModifierAlt) != 0;
    uint32_t n = 0;

    const VtKey vt = vtKey(key);
    if (vt.final != 0) {
        const char param = static_cast<char>('1' + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0));
        out[n++] = 0x1B;
        out[n++] = vt.ss3 && param == '1' ? 'O' : '[';
        uint8_t number = vt.number;
        if (number == 0 && param != '1') {
            number = 1;
        }
        if (number >= 10) {
            out[n++] = static_cast<char>('0' + number / 10);
        }
        if (number != 0) {
            out[n++] = static_cast<char>('0' + number % 10);
        }
        if (param != '1') {
            out[n++] = ';';
            out[n++] = param;
        }
        out[n++] = vt.final;
        return n;
    }

    if (alt) {
        out[n++] = 0x1B;
    }
    if (ctrl) {
        if (key >= KeyA && key <= KeyZ) {
            out[n++] = static_cast<char>(key - KeyA + 1);
            return n;
        }
        switch (key) {
        case KeySpace: case Key2: out[n++] = 0x00; return n;            // Ctrl-@
        case KeyLeftBracket: out[n++] = 0x1B; return n;                 // Ctrl-[
        case KeyBackslash: out[n++] = 0x1C; return n;                   // Ctrl-backslash
        case KeyRightBracket: out[n++] = 0x1D; return n;                // Ctrl-]
        case Key6: out[n++] = 0x1E; return n;                           // Ctrl-^
        case KeyMinus: case KeySlash: out[n++] = 0x1F; return n;        // Ctrl-_
        default: break;
        }
    }
    switch (key) {
    case KeyEnter: case KeypadEnter: out[n++] = '\n'; return n;
    case KeyBackspace: out[n++] = '\b'; return n;
    case KeyTab: out[n++] = '\t'; return n;
    case KeyEscape: out[n++] = 0x1B; return n;
    default: break;
    }
    if (text == 0) {
        return 0;
    }
    out[n++] = text;
    return n;
}

// The key (and whether Shift is needed) that types `c` on a US layout, for
// input arriving as characters (the serial console). Control characters map to
// their keys: Enter, Backspace, Tab, Escape; KeyNone if there is none.
inline uint16_t forUsChar(char c, bool* shift) {
    *shift = false;
    switch (c) {
    case 0: return KeyNone;
    case '\r': case '\n': return KeyEnter;
    case '\b': case 0x7F: return KeyBackspace;
    case '\t': return KeyTab;
    case 0x1B: return KeyEscape;
    default: break;
    }
    for (uint16_t key = KeyA; key <= KeyNonUsBackslash; ++key) {
        if (usText(key, false, false) == c) {
            return key;
        }
        if (usText(key, true, false) == c) {
            *shift = true;
            return key;
        }
    }
    return KeyNone;
}

}  // namespace keymap

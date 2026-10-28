// Host-side keymap fixture tests.
//
// Drives the pure keyboard tables the kernel uses
// (include/interrupts/keymap.hpp): PS/2 scan code set 1 (plain and E0) to
// KeyCode, which keys repeat, the keypad with Num Lock off, the US text a key
// types, character -> key (the serial console), and the bytes a console read
// gets for a key (xterm sequences and controls). A regression in any of them
// fails here without booting QEMU.
//
// Build/run with tools/run-keymap-fixtures.sh.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <interrupts/keymap.hpp>

// ---------------------------------------------------------------------------
// Tiny test harness
// ---------------------------------------------------------------------------

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__);            \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ++g_checks;                                                            \
        auto _va = (a);                                                        \
        auto _vb = (b);                                                        \
        if (!(_va == _vb)) {                                                   \
            ++g_failures;                                                      \
            std::printf("  FAIL: %s == %s (line %d): got 0x%llx vs 0x%llx\n",  \
                        #a, #b, __LINE__,                                      \
                        (unsigned long long)_va, (unsigned long long)_vb);     \
        }                                                                      \
    } while (0)

// The console bytes for a key, as a std::string (it may hold a NUL).
static std::string console(uint16_t key, char text, uint16_t modifiers) {
    char out[16];
    std::memset(out, 0x55, sizeof(out));
    const uint32_t n = keymap::consoleBytes(key, text, modifiers, out);
    if (n > 8) {
        return "<too long>";
    }
    for (uint32_t i = 8; i < sizeof(out); ++i) {   // out may be 8 bytes
        if (out[i] != 0x55) {
            return "<wrote past 8 bytes>";
        }
    }
    return std::string(out, n);
}

#define CHECK_BYTES(key, text, mods, want)                                     \
    do {                                                                       \
        ++g_checks;                                                            \
        const std::string _got = console(key, text, mods);                     \
        const std::string _want(want, sizeof(want) - 1);                       \
        if (_got != _want) {                                                   \
            ++g_failures;                                                      \
            std::printf("  FAIL: consoleBytes(%s, mods %s) (line %d): got",    \
                        #key, #mods, __LINE__);                                \
            for (unsigned char c : _got) std::printf(" %02x", c);              \
            std::printf(", want");                                             \
            for (unsigned char c : _want) std::printf(" %02x", c);             \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// PS/2
// ---------------------------------------------------------------------------

static void testPs2() {
    std::printf("-- PS/2 scan code set 1\n");
    struct { uint8_t code; uint16_t key; } plain[] = {
        {0x01, KeyEscape}, {0x02, Key1}, {0x0B, Key0}, {0x0C, KeyMinus}, {0x0D, KeyEqual},
        {0x0E, KeyBackspace}, {0x0F, KeyTab}, {0x10, KeyQ}, {0x19, KeyP},
        {0x1A, KeyLeftBracket}, {0x1B, KeyRightBracket}, {0x1C, KeyEnter},
        {0x1D, KeyLeftControl}, {0x1E, KeyA}, {0x26, KeyL}, {0x27, KeySemicolon},
        {0x28, KeyApostrophe}, {0x29, KeyGrave}, {0x2A, KeyLeftShift}, {0x2B, KeyBackslash},
        {0x2C, KeyZ}, {0x32, KeyM}, {0x35, KeySlash}, {0x36, KeyRightShift},
        {0x37, KeypadAsterisk}, {0x38, KeyLeftAlt}, {0x39, KeySpace}, {0x3A, KeyCapsLock},
        {0x3B, KeyF1}, {0x44, KeyF10}, {0x45, KeyNumLock}, {0x46, KeyScrollLock},
        {0x47, Keypad7}, {0x48, Keypad8}, {0x49, Keypad9}, {0x4A, KeypadMinus},
        {0x4B, Keypad4}, {0x4C, Keypad5}, {0x4D, Keypad6}, {0x4E, KeypadPlus},
        {0x4F, Keypad1}, {0x50, Keypad2}, {0x51, Keypad3}, {0x52, Keypad0},
        {0x53, KeypadPeriod}, {0x56, KeyNonUsBackslash}, {0x57, KeyF11}, {0x58, KeyF12},
    };
    for (const auto& p : plain) {
        CHECK_EQ(keymap::fromPs2Set1(p.code), p.key);
    }
    // Unknown codes, the break bit's range and beyond the table are no key.
    CHECK_EQ(keymap::fromPs2Set1(0x00), (uint16_t)KeyNone);
    CHECK_EQ(keymap::fromPs2Set1(0x54), (uint16_t)KeyNone);
    CHECK_EQ(keymap::fromPs2Set1(0x59), (uint16_t)KeyNone);
    CHECK_EQ(keymap::fromPs2Set1(0x7F), (uint16_t)KeyNone);
    CHECK_EQ(keymap::fromPs2Set1(0xFF), (uint16_t)KeyNone);

    // No two scan codes name the same key.
    for (int a = 0; a < 0x80; ++a) {
        for (int b = a + 1; b < 0x80; ++b) {
            const uint16_t ka = keymap::fromPs2Set1((uint8_t)a);
            if (ka != KeyNone && ka == keymap::fromPs2Set1((uint8_t)b)) {
                ++g_failures;
                std::printf("  FAIL: set 1 codes 0x%02x and 0x%02x are both key 0x%02x\n", a, b, ka);
            }
        }
    }

    struct { uint8_t code; uint16_t key; } extended[] = {
        {0x1C, KeypadEnter}, {0x1D, KeyRightControl}, {0x35, KeypadSlash},
        {0x37, KeyPrintScreen}, {0x38, KeyRightAlt}, {0x47, KeyHome}, {0x48, KeyUp},
        {0x49, KeyPageUp}, {0x4B, KeyLeft}, {0x4D, KeyRight}, {0x4F, KeyEnd},
        {0x50, KeyDown}, {0x51, KeyPageDown}, {0x52, KeyInsert}, {0x53, KeyDelete},
        {0x5B, KeyLeftSuper}, {0x5C, KeyRightSuper}, {0x5D, KeyApplication},
    };
    for (const auto& p : extended) {
        CHECK_EQ(keymap::fromPs2Set1Extended(p.code), p.key);
    }
    // The fake shifts around extended keys are not keys.
    CHECK_EQ(keymap::fromPs2Set1Extended(0x2A), (uint16_t)KeyNone);
    CHECK_EQ(keymap::fromPs2Set1Extended(0x36), (uint16_t)KeyNone);
}

// ---------------------------------------------------------------------------
// Key classes and the keypad
// ---------------------------------------------------------------------------

static void testClasses() {
    std::printf("-- modifiers, repeat, keypad\n");
    for (uint16_t k = KeyLeftControl; k <= KeyRightSuper; ++k) {
        CHECK(keymap::isModifierKey(k));
        CHECK(!keymap::repeats(k));
    }
    CHECK(!keymap::isModifierKey(KeyCapsLock));
    CHECK(!keymap::isModifierKey(KeyA));
    for (uint16_t k : {KeyCapsLock, KeyNumLock, KeyScrollLock, KeyPrintScreen, KeyPause, KeyNone}) {
        CHECK(!keymap::repeats(k));
    }
    for (uint16_t k : {KeyA, KeyZ, Key1, KeySpace, KeyBackspace, KeyDelete, KeyLeft, KeyUp,
                       KeyEnter, KeyTab, KeypadEnter, Keypad4, KeyF5}) {
        CHECK(keymap::repeats(k));
    }

    struct { uint16_t pad; uint16_t nav; } nav[] = {
        {Keypad1, KeyEnd}, {Keypad2, KeyDown}, {Keypad3, KeyPageDown}, {Keypad4, KeyLeft},
        {Keypad6, KeyRight}, {Keypad7, KeyHome}, {Keypad8, KeyUp}, {Keypad9, KeyPageUp},
        {Keypad0, KeyInsert}, {KeypadPeriod, KeyDelete},
    };
    for (const auto& n : nav) {
        CHECK_EQ(keymap::keypadNavigation(n.pad), n.nav);
    }
    // Keys with no navigation meaning stay themselves.
    for (uint16_t k : {Keypad5, KeypadPlus, KeypadMinus, KeypadSlash, KeypadAsterisk, KeypadEnter, KeyA}) {
        CHECK_EQ(keymap::keypadNavigation(k), k);
    }

    const char* digits = "1234567890";
    for (int i = 0; i < 9; ++i) {
        CHECK_EQ(keymap::keypadText((uint16_t)(Keypad1 + i)), digits[i]);
    }
    CHECK_EQ(keymap::keypadText(Keypad0), '0');
    CHECK_EQ(keymap::keypadText(KeypadPeriod), '.');
    CHECK_EQ(keymap::keypadText(KeypadSlash), '/');
    CHECK_EQ(keymap::keypadText(KeypadAsterisk), '*');
    CHECK_EQ(keymap::keypadText(KeypadMinus), '-');
    CHECK_EQ(keymap::keypadText(KeypadPlus), '+');
    CHECK_EQ(keymap::keypadText(KeypadEnter), (char)0);
}

// ---------------------------------------------------------------------------
// US text and character -> key
// ---------------------------------------------------------------------------

static void testText() {
    std::printf("-- US text\n");
    CHECK_EQ(keymap::usText(KeyA, false, false), 'a');
    CHECK_EQ(keymap::usText(KeyA, true, false), 'A');
    CHECK_EQ(keymap::usText(KeyA, false, true), 'A');   // Caps Lock
    CHECK_EQ(keymap::usText(KeyA, true, true), 'a');    // Shift undoes it
    CHECK_EQ(keymap::usText(Key1, false, true), '1');   // letters only
    CHECK_EQ(keymap::usText(Key1, true, false), '!');
    CHECK_EQ(keymap::usText(Key2, true, false), '@');
    CHECK_EQ(keymap::usText(Key3, true, false), '#');
    CHECK_EQ(keymap::usText(KeyApostrophe, true, false), '"');
    CHECK_EQ(keymap::usText(KeyGrave, true, false), '~');
    CHECK_EQ(keymap::usText(KeyBackslash, false, false), '\\');
    CHECK_EQ(keymap::usText(KeyBackslash, true, false), '|');
    CHECK_EQ(keymap::usText(KeyNonUsBackslash, false, false), '\\');
    CHECK_EQ(keymap::usText(KeySpace, true, false), ' ');
    for (uint16_t k : {KeyEnter, KeyEscape, KeyBackspace, KeyTab, (KeyCode)0x32, KeyF1, KeyLeft,
                       KeyLeftShift, KeyCapsLock, KeyDelete, Keypad1}) {
        CHECK_EQ(keymap::usText(k, false, false), (char)0);
        CHECK_EQ(keymap::usText(k, true, false), (char)0);
    }

    // Every printable character has a key, and that key (with the Shift
    // forUsChar asks for) types it back.
    for (int c = 0x20; c < 0x7F; ++c) {
        bool shift = false;
        const uint16_t key = keymap::forUsChar((char)c, &shift);
        ++g_checks;
        if (key == KeyNone || keymap::usText(key, shift, false) != (char)c) {
            ++g_failures;
            std::printf("  FAIL: forUsChar('%c') = key 0x%02x shift %d types '%c'\n",
                        c, key, shift, keymap::usText(key, shift, false));
        }
    }
    bool shift = true;
    CHECK_EQ(keymap::forUsChar('\n', &shift), (uint16_t)KeyEnter);
    CHECK(!shift);
    CHECK_EQ(keymap::forUsChar('\r', &shift), (uint16_t)KeyEnter);
    CHECK_EQ(keymap::forUsChar('\b', &shift), (uint16_t)KeyBackspace);
    CHECK_EQ(keymap::forUsChar(0x7F, &shift), (uint16_t)KeyBackspace);
    CHECK_EQ(keymap::forUsChar('\t', &shift), (uint16_t)KeyTab);
    CHECK_EQ(keymap::forUsChar(0x1B, &shift), (uint16_t)KeyEscape);
    CHECK_EQ(keymap::forUsChar(0, &shift), (uint16_t)KeyNone);
    CHECK_EQ(keymap::forUsChar((char)0xE9, &shift), (uint16_t)KeyNone);
}

// ---------------------------------------------------------------------------
// Console bytes
// ---------------------------------------------------------------------------

static void testConsole() {
    std::printf("-- console bytes\n");
    const uint16_t S = KeyModifierShift, C = KeyModifierControl, A = KeyModifierAlt;

    // Plain keys: text, and the controls of the keys that type none.
    CHECK_BYTES(KeyA, 'a', 0, "a");
    CHECK_BYTES(KeyA, 'A', S, "A");
    CHECK_BYTES(Keypad4, '4', KeyModifierNumLock, "4");
    CHECK_BYTES(KeyEnter, 0, 0, "\n");
    CHECK_BYTES(KeypadEnter, 0, 0, "\n");
    CHECK_BYTES(KeyBackspace, 0, 0, "\b");
    CHECK_BYTES(KeyTab, 0, 0, "\t");
    CHECK_BYTES(KeyEscape, 0, 0, "\x1b");
    // Keys that send nothing.
    for (uint16_t k : {KeyLeftShift, KeyRightControl, KeyLeftAlt, KeyLeftSuper, KeyCapsLock,
                       KeyNumLock, KeyScrollLock, KeyPrintScreen, KeyPause, KeyApplication}) {
        CHECK_EQ(console(k, 0, 0).size(), (size_t)0);
    }

    // The cursor and editing keys and F-keys, as xterm sends them.
    CHECK_BYTES(KeyUp, 0, 0, "\x1b[A");
    CHECK_BYTES(KeyDown, 0, 0, "\x1b[B");
    CHECK_BYTES(KeyRight, 0, 0, "\x1b[C");
    CHECK_BYTES(KeyLeft, 0, 0, "\x1b[D");
    CHECK_BYTES(KeyHome, 0, 0, "\x1b[H");
    CHECK_BYTES(KeyEnd, 0, 0, "\x1b[F");
    CHECK_BYTES(KeyInsert, 0, 0, "\x1b[2~");
    CHECK_BYTES(KeyDelete, 0, 0, "\x1b[3~");
    CHECK_BYTES(KeyPageUp, 0, 0, "\x1b[5~");
    CHECK_BYTES(KeyPageDown, 0, 0, "\x1b[6~");
    CHECK_BYTES(KeyF1, 0, 0, "\x1bOP");
    CHECK_BYTES(KeyF2, 0, 0, "\x1bOQ");
    CHECK_BYTES(KeyF3, 0, 0, "\x1bOR");
    CHECK_BYTES(KeyF4, 0, 0, "\x1bOS");
    CHECK_BYTES(KeyF5, 0, 0, "\x1b[15~");
    CHECK_BYTES(KeyF6, 0, 0, "\x1b[17~");
    CHECK_BYTES(KeyF7, 0, 0, "\x1b[18~");
    CHECK_BYTES(KeyF8, 0, 0, "\x1b[19~");
    CHECK_BYTES(KeyF9, 0, 0, "\x1b[20~");
    CHECK_BYTES(KeyF10, 0, 0, "\x1b[21~");
    CHECK_BYTES(KeyF11, 0, 0, "\x1b[23~");
    CHECK_BYTES(KeyF12, 0, 0, "\x1b[24~");
    // Caps and Num Lock do not change them.
    CHECK_BYTES(KeyUp, 0, KeyModifierCapsLock | KeyModifierNumLock, "\x1b[A");

    // With modifiers: xterm's parameter, 1 + Shift 1, Alt 2, Ctrl 4.
    CHECK_BYTES(KeyLeft, 0, C, "\x1b[1;5D");
    CHECK_BYTES(KeyRight, 0, C, "\x1b[1;5C");
    CHECK_BYTES(KeyUp, 0, S, "\x1b[1;2A");
    CHECK_BYTES(KeyHome, 0, A, "\x1b[1;3H");
    CHECK_BYTES(KeyEnd, 0, S | A | C, "\x1b[1;8F");
    CHECK_BYTES(KeyDelete, 0, C, "\x1b[3;5~");
    CHECK_BYTES(KeyPageDown, 0, S, "\x1b[6;2~");
    CHECK_BYTES(KeyF1, 0, C, "\x1b[1;5P");
    CHECK_BYTES(KeyF4, 0, S, "\x1b[1;2S");
    CHECK_BYTES(KeyF5, 0, S, "\x1b[15;2~");
    CHECK_BYTES(KeyF12, 0, S | A | C, "\x1b[24;8~");

    // Every key, every modifier combination: at most 8 bytes, nothing written
    // past 8, and a sequence always well formed with the right parameter.
    for (uint32_t key = 0; key < 0xE8; ++key) {
        for (uint16_t mods = 0; mods < 64; ++mods) {
            const std::string b = console((uint16_t)key, 0, mods);
            if (b.size() > 0 && b[0] == '<') {
                ++g_failures;
                std::printf("  FAIL: key 0x%02x mods 0x%x: %s\n", key, mods, b.c_str());
                continue;
            }
            if (keymap::vtKey((uint16_t)key).final == 0) {
                continue;
            }
            ++g_checks;
            const int want = 1 + ((mods & S) ? 1 : 0) + ((mods & A) ? 2 : 0) + ((mods & C) ? 4 : 0);
            bool good = b.size() >= 3 && b[0] == 0x1B && (b[1] == '[' || b[1] == 'O') &&
                        b.back() == keymap::vtKey((uint16_t)key).final;
            const size_t semi = b.find(';');
            if (want == 1) {
                good = good && semi == std::string::npos;
            } else {
                good = good && b[1] == '[' && semi != std::string::npos && semi + 3 == b.size() &&
                       b[semi + 1] == (char)('0' + want);
            }
            if (!good) {
                ++g_failures;
                std::printf("  FAIL: key 0x%02x mods 0x%x: malformed sequence\n", key, mods);
            }
        }
    }

    // Ctrl: the C0 controls.
    for (uint16_t k = KeyA; k <= KeyZ; ++k) {
        const char want[2] = {(char)(k - KeyA + 1), 0};
        CHECK(console(k, (char)('a' + (k - KeyA)), C) == std::string(want, 1));
    }
    CHECK_BYTES(KeyC, 'C', C | S, "\x03");
    CHECK_BYTES(KeySpace, ' ', C, "\x00");
    CHECK_BYTES(Key2, '2', C, "\x00");
    CHECK_BYTES(KeyLeftBracket, '[', C, "\x1b");
    CHECK_BYTES(KeyBackslash, '\\', C, "\x1c");
    CHECK_BYTES(KeyRightBracket, ']', C, "\x1d");
    CHECK_BYTES(Key6, '6', C, "\x1e");
    CHECK_BYTES(KeyMinus, '-', C, "\x1f");
    CHECK_BYTES(KeySlash, '/', C, "\x1f");
    CHECK_BYTES(Key1, '1', C, "1");   // no control: the text

    // Alt: ESC before the key.
    CHECK_BYTES(KeyX, 'x', A, "\x1bx");
    CHECK_BYTES(KeyX, 'X', A | S, "\x1bX");
    CHECK_BYTES(KeyB, 'b', A | C, "\x1b\x02");
    CHECK_BYTES(KeyBackspace, 0, A, "\x1b\b");
    CHECK_BYTES(KeyEnter, 0, A, "\x1b\n");
}

int main() {
    std::printf("== keymap fixture tests ==\n");
    testPs2();
    testClasses();
    testText();
    testConsole();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("KEYMAP FIXTURE TESTS PASSED\n");
        return 0;
    }
    std::printf("KEYMAP FIXTURE TESTS FAILED\n");
    return 1;
}

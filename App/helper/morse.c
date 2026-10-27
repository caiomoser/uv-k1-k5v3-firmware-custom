/* Copyright 2026 caiomoser
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include "helper/morse.h"

#ifdef ENABLE_FEAT_F4HWN_BOOT_MORSE

#include <string.h>

#include "app/keyboard_state.h"
#include "audio.h"
#include "driver/backlight.h"
#include "driver/bk4819-regs.h"
#include "driver/bk4819.h"
#include "driver/keyboard.h"
#include "driver/py25q16.h"
#include "driver/st7565.h"
#include "driver/system.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/ui.h"

// Tone pitch; the time unit comes from the selected speed (PARIS: 1200 / WPM ms).
// dah = 3 units, intra-element gap = 1 unit, inter-character gap = 3 units (2 more
// after the unit already elapsed past the last element).
#define MORSE_TONE_HZ 750

// Selectable speeds; index 0 (12 WPM, 100 ms unit) is the default and matches the
// fixed speed of earlier builds.
static const uint8_t MORSE_WPM[MORSE_WPM_COUNT] = { 12, 15, 20, 25 };

// Sentinel-prefixed patterns: after the leading 1 sentinel bit, each lower bit is
// one element read MSB-first (0 = dit, 1 = dah). Same encoding as the Fox Hunt
// beacon's table (app/foxhunt.c), kept as its own small copy here so this feature
// builds without ENABLE_FEAT_F4HWN_FOXHUNT.
static const uint8_t MORSE_LETTER[26] = {
    0x05, 0x18, 0x1A, 0x0C, 0x02, 0x12, 0x0E, 0x10, 0x04, 0x17,   // A..J
    0x0D, 0x14, 0x07, 0x06, 0x0F, 0x16, 0x1D, 0x0A, 0x08, 0x03,   // K..T
    0x09, 0x11, 0x0B, 0x19, 0x1B, 0x1C,                           // U..Z
};
static const uint8_t MORSE_DIGIT[10] = {
    0x3F, 0x2F, 0x27, 0x23, 0x21, 0x20, 0x30, 0x38, 0x3C, 0x3E,   // 0..9
};

// Characters selectable in the editor, cycled by UP/DOWN at the cursor position.
static const char MORSE_CHARSET[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

// Saved message, cached so menu redraws and boot playback don't hit the flash.
static char MorseMsg[MORSE_MSG_MAX + 1];

static uint8_t MORSE_ByteFor(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return MORSE_LETTER[c - 'A'];
    if (c >= '0' && c <= '9') return MORSE_DIGIT[c - '0'];
    return 0;
}

// Copy raw flash bytes into out[MORSE_MSG_MAX + 1], stopping at the first NUL or
// character outside the supported set (blank/corrupt flash). Returns the length.
static uint8_t MORSE_Sanitize(const char *raw, char *out)
{
    uint8_t i;
    for (i = 0; i < MORSE_MSG_MAX; i++) {
        const char c = raw[i];
        if (c != ' ' && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9'))
            break;
        out[i] = c;
    }
    out[i] = '\0';
    return i;
}

// DTMF settings block (0ED0..0EDF) as SETTINGS_InitEEPROM defaults it: side tone on,
// '*' / '#' codes, preload 300 ms, persist and interval times 100 ms, remote kill on.
static const uint8_t MORSE_DTMF_DEFAULTS[16] = {
    1, '*', '#', 0, 10, 30, 10, 10,
    10, 10, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static void MORSE_ResetDtmfSettings(void)
{
    gEeprom.DTMF_SIDE_TONE               = true;
#ifdef ENABLE_DTMF_CALLING
    gEeprom.DTMF_SEPARATE_CODE           = '*';
    gEeprom.DTMF_GROUP_CALL_CODE         = '#';
    gEeprom.DTMF_DECODE_RESPONSE         = 0;
    gEeprom.DTMF_auto_reset_time         = 10;
    gEeprom.PERMIT_REMOTE_KILL           = true;
#endif
    gEeprom.DTMF_PRELOAD_TIME            = 300;
    gEeprom.DTMF_FIRST_CODE_PERSIST_TIME = 100;
    gEeprom.DTMF_HASH_CODE_PERSIST_TIME  = 100;
    gEeprom.DTMF_CODE_PERSIST_TIME       = 100;
    gEeprom.DTMF_CODE_INTERVAL_TIME      = 100;
}

// Bytes from the old block to the end of the new one: both live in the settings
// sector, so the move and the DTMF repair go out in a single sector rewrite.
#define MORSE_MIGRATE_SPAN (MORSE_FLASH_ADDR + 16u - MORSE_OLD_FLASH_ADDR)

// First run with the new layout: initialise the block (returned in block[16]) and,
// if a message saved by an earlier build is found at the old address, carry it and
// its enabled flag over and restore the DTMF settings it had overwritten.
static void MORSE_Migrate(uint8_t *block)
{
    uint8_t buf[MORSE_MIGRATE_SPAN];
    PY25Q16_ReadBuffer(MORSE_OLD_FLASH_ADDR, buf, sizeof(buf));

    uint8_t *oldBlock = buf;
    uint8_t *newBlock = buf + (MORSE_FLASH_ADDR - MORSE_OLD_FLASH_ADDR);

    char msg[MORSE_MSG_MAX + 1];
    const uint8_t len = MORSE_Sanitize((const char *)oldBlock + 1, msg);

    // The old MORSE_SaveMessage wrote the text NUL-padded to MORSE_MSG_MAX + 1 bytes.
    // Require exactly that shape so genuine DTMF settings are never taken for one.
    bool found = len > 0;
    for (uint8_t i = 1 + len; found && i <= MORSE_MSG_MAX + 1; i++)
        if (oldBlock[i] != 0)
            found = false;

    memset(newBlock, 0xFF, 16);
    newBlock[0] = (found && oldBlock[0] == 1) ? 1 : 0;
    newBlock[1] = 0;
    memset(newBlock + 2, 0, MORSE_MSG_MAX + 1);

    if (found) {
        memcpy(newBlock + 2, msg, len);
        memcpy(oldBlock, MORSE_DTMF_DEFAULTS, sizeof(MORSE_DTMF_DEFAULTS));
        MORSE_ResetDtmfSettings();
    }

    PY25Q16_WriteBuffer(MORSE_OLD_FLASH_ADDR, buf, sizeof(buf), false);
    memcpy(block, newBlock, 16);
}

void MORSE_LoadSettings(void)
{
    uint8_t block[16];
    PY25Q16_ReadBuffer(MORSE_FLASH_ADDR, block, sizeof(block));

    if (block[0] == 0xFF)
        MORSE_Migrate(block);

    gEeprom.BOOT_MORSE     = (block[0] == 1);
    gEeprom.BOOT_MORSE_WPM = (block[1] < MORSE_WPM_COUNT) ? block[1] : 0;
    MORSE_Sanitize((const char *)block + 2, MorseMsg);
}

void MORSE_SaveSettings(void)
{
    const uint8_t state[2] = { gEeprom.BOOT_MORSE ? 1 : 0, gEeprom.BOOT_MORSE_WPM };
    PY25Q16_WriteBuffer(MORSE_FLASH_ADDR, state, sizeof(state), false);
}

const char *MORSE_GetMessage(void)
{
    return MorseMsg;
}

void MORSE_SaveMessage(const char *msg)
{
    char raw[MORSE_MSG_MAX + 1] = {0};
    strncpy(raw, msg, MORSE_MSG_MAX);
    PY25Q16_WriteBuffer(MORSE_FLASH_ADDR + 2, raw, sizeof(raw), false);
    MORSE_Sanitize(raw, MorseMsg);
}

uint8_t MORSE_GetWpm(uint8_t index)
{
    return MORSE_WPM[index < MORSE_WPM_COUNT ? index : 0];
}

// Wait ms in 10 ms steps, polling the keypad; true as soon as a key is down.
static bool MORSE_WaitAbortable(uint16_t ms)
{
    for (;;) {
        if (KEYBOARD_Poll() != KEY_INVALID)
            return true;
        if (ms == 0)
            return false;
        const uint16_t step = (ms < 10) ? ms : 10;
        SYSTEM_DelayMs(step);
        ms -= step;
    }
}

void MORSE_PlayText(const char *msg)
{
    if (msg == NULL || msg[0] == '\0')
        return;

    const uint16_t unit = 1200u / MORSE_GetWpm(gEeprom.BOOT_MORSE_WPM);

    // Same tone-generator save/restore sequence as AUDIO_PlayBeep (audio.c), so a
    // multi-character ID played at boot leaves the radio's RX state untouched.
    const uint16_t toneConfig = BK4819_ReadRegister(BK4819_REG_71);

    BK4819_PrepareToPlayTone(true);
    SYSTEM_DelayMs(2);
    AUDIO_AudioPathOn();
    SYSTEM_DelayMs(60);

    // Any keypress aborts the ID; the keypad is checked through every gap, so the
    // longest it can go unnoticed is one dah.
    bool aborted = MORSE_WaitAbortable(0);

    for (const char *p = msg; *p && !aborted; p++) {
        const uint8_t code = MORSE_ByteFor(*p);

        if (code == 0) {                      // space / unsupported -> word gap
            aborted = MORSE_WaitAbortable(unit * 4); // 3 units already elapsed after the previous char
            continue;
        }

        uint8_t bit = 0x80;
        while (!(code & bit)) bit >>= 1;      // skip to the sentinel bit
        for (bit >>= 1; bit && !aborted; bit >>= 1) {     // walk the elements, MSB first
            const uint16_t on = (code & bit) ? (unit * 3)   // dah
                                              :  unit;       // dit
            BK4819_PlayToneRaw(MORSE_TONE_HZ, on);
            aborted = MORSE_WaitAbortable(unit);            // intra-element gap
        }
        if (!aborted)
            aborted = MORSE_WaitAbortable(unit * 2);        // inter-character gap (3 units total)
    }

    AUDIO_AudioPathOff();
    SYSTEM_DelayMs(5);
    BK4819_TurnsOffTones_TurnsOnRX();
    SYSTEM_DelayMs(5);
    BK4819_WriteRegister(BK4819_REG_71, toneConfig);

    // Swallow the key that aborted playback, so it doesn't go on to trigger its
    // normal function in whatever runs next.
    if (aborted) {
        while (KEYBOARD_Poll() != KEY_INVALID)
            SYSTEM_DelayMs(10);
    }
}

bool MORSE_PlayBootId(void)
{
    if (!gEeprom.BOOT_MORSE || MorseMsg[0] == '\0')
        return false;

    MORSE_PlayText(MorseMsg);
    return true;
}

// Wrap forward (dir > 0) or backward one step through MORSE_CHARSET, both ways round.
static char MORSE_CycleChar(char c, int8_t dir)
{
    const uint8_t n = sizeof(MORSE_CHARSET) - 1;   // exclude the NUL
    uint8_t idx = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (MORSE_CHARSET[i] == c) { idx = i; break; }
    }
    idx = (uint8_t)((idx + (dir > 0 ? 1u : (unsigned)(n - 1u))) % n);
    return MORSE_CHARSET[idx];
}

// Small-font field (7 px/char) with the cursor's cell shown inverted, in the same
// XOR style ui/welcome.c uses for its version capsule.
static void MORSE_DrawEditor(const char *buf, uint8_t cursor)
{
    UI_DisplayClear();
    UI_StatusClear();
    GUI_DisplaySmallestInverse("MORSE ID", 2, 0, true, true, 34);

    UI_PrintStringSmallNormal(buf, 4, 0, 3);

    const uint8_t x0 = (uint8_t)(4 + cursor * 7);
    for (uint8_t x = x0; x < x0 + 7 && x < LCD_WIDTH; x++)
        gFrameBuffer[3][x] ^= 0xFF;

    UI_PrintStringSmallNormal("UP/DN=CHAR MENU=NEXT", 4, 0, 5);
    UI_PrintStringSmallNormal("EXIT=BACK HOLD=SAVE",  4, 0, 6);
}

// Same long-press threshold as the firmware's key-repeat delay (misc.c).
#define MORSE_HOLD_10MS 40

void MORSE_EditMessage(void)
{
    char buf[MORSE_MSG_MAX + 1];
    strcpy(buf, MorseMsg);

    // Pad the field with spaces so every cursor position shows a selectable cell.
    for (uint8_t i = (uint8_t)strlen(buf); i < MORSE_MSG_MAX; i++)
        buf[i] = ' ';
    buf[MORSE_MSG_MAX] = '\0';

    uint8_t       cursor = 0;
    bool          saved  = false;
    bool          redraw = true;
    KeyboardState kbd    = {KEY_INVALID, KEY_INVALID, 0};

    BACKLIGHT_UpdateTickless();
    BACKLIGHT_TurnOn();

    for (;;) {
        // Only repaint when the field or cursor changed, instead of pushing the
        // whole frame over SPI on every 10 ms poll.
        if (redraw) {
            MORSE_DrawEditor(buf, cursor);
            ST7565_BlitStatusLine();
            ST7565_BlitFullScreen();
            redraw = false;
        }

        kbd.prev    = kbd.current;
        kbd.current = KEYBOARD_GetKey();

        if (kbd.current == KEY_INVALID) {
            // EXIT released before reaching the long-press threshold below:
            // treat it as a short press now (step back, or cancel at cursor 0).
            if (kbd.prev == KEY_EXIT && kbd.counter > 0 && kbd.counter < MORSE_HOLD_10MS) {
                if (cursor == 0)
                    break;                  // cancel: discard and leave
                cursor--;
                redraw = true;
            }
            kbd.counter = 0;
            SYSTEM_DelayMs(10);
            BACKLIGHT_Update();
            continue;
        }

        kbd.counter = (kbd.current == kbd.prev) ? (uint8_t)(kbd.counter + 1) : 1;

        if (kbd.current == KEY_EXIT) {
            // EXIT's action depends on how long it's held (see above / below),
            // so it's never handled on the immediate press edge like the others.
            if (kbd.counter == MORSE_HOLD_10MS) {   // held long enough: confirm & save here
                saved = true;
                break;
            }
            SYSTEM_DelayMs(10);
            BACKLIGHT_Update();
            continue;
        }

        if (kbd.current == kbd.prev) {   // UP/DOWN/MENU held: no repeat, ignore
            SYSTEM_DelayMs(10);
            BACKLIGHT_Update();
            continue;
        }

        BACKLIGHT_TurnOn();
        redraw = true;

        if (kbd.current == KEY_UP) {
            buf[cursor] = MORSE_CycleChar(buf[cursor], +1);
        } else if (kbd.current == KEY_DOWN) {
            buf[cursor] = MORSE_CycleChar(buf[cursor], -1);
        } else if (kbd.current == KEY_MENU) {
            if (cursor + 1 < MORSE_MSG_MAX) {
                cursor++;
            } else {
                saved = true;               // confirmed past the last character
                break;
            }
        }
    }

    if (saved) {
        for (int8_t i = MORSE_MSG_MAX - 1; i >= 0 && buf[i] == ' '; i--)
            buf[i] = '\0';                  // trim trailing padding before saving
        MORSE_SaveMessage(buf);
        MORSE_PlayText(buf);                // preview so the user can check it right away
    }

    // We consumed EXIT/MENU presses internally above, so the caller's own key
    // dispatcher never saw them. If the long-press that just confirmed a save
    // is still physically held when we return, that dispatcher would treat it
    // as a brand-new press and start its own repeat-fire (e.g. backing out of
    // several menu levels). Wait it out before handing control back.
    while (KEYBOARD_GetKey() != KEY_INVALID)
        SYSTEM_DelayMs(10);
}

void ACTION_MorseMsgEdit(void)
{
    MORSE_EditMessage();
    GUI_SelectNextDisplay(DISPLAY_MAIN);
}

#endif // ENABLE_FEAT_F4HWN_BOOT_MORSE

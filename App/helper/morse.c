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

// One Morse time unit; dah = 3 units, intra-element gap = 1 unit, inter-character
// gap = 3 units (2 more after the unit already elapsed past the last element).
#define MORSE_UNIT_MS 100
#define MORSE_TONE_HZ 750

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

static uint8_t MORSE_ByteFor(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return MORSE_LETTER[c - 'A'];
    if (c >= '0' && c <= '9') return MORSE_DIGIT[c - '0'];
    return 0;
}

void MORSE_LoadMessage(char *out)
{
    char raw[MORSE_MSG_MAX + 1];
    PY25Q16_ReadBuffer(MORSE_FLASH_ADDR + 1, raw, sizeof(raw));

    uint8_t i;
    for (i = 0; i < MORSE_MSG_MAX; i++) {
        char c = raw[i];
        if (c == 0)
            break;
        if (c != ' ' && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9'))
            break;                          // blank/corrupt flash -> stop here
        out[i] = c;
    }
    out[i] = '\0';
}

void MORSE_SaveMessage(const char *msg)
{
    char raw[MORSE_MSG_MAX + 1] = {0};
    strncpy(raw, msg, MORSE_MSG_MAX);
    PY25Q16_WriteBuffer(MORSE_FLASH_ADDR + 1, raw, sizeof(raw), false);
}

void MORSE_PlayText(const char *msg)
{
    if (msg == NULL || msg[0] == '\0')
        return;

    // Same tone-generator save/restore sequence as AUDIO_PlayBeep (audio.c), so a
    // multi-character ID played at boot leaves the radio's RX state untouched.
    const uint16_t toneConfig = BK4819_ReadRegister(BK4819_REG_71);

    BK4819_PrepareToPlayTone(true);
    SYSTEM_DelayMs(2);
    AUDIO_AudioPathOn();
    SYSTEM_DelayMs(60);

    for (const char *p = msg; *p; p++) {
        if (KEYBOARD_Poll() != KEY_INVALID)   // any keypress aborts the ID early
            break;

        const uint8_t code = MORSE_ByteFor(*p);

        if (code == 0) {                      // space / unsupported -> word gap
            SYSTEM_DelayMs(MORSE_UNIT_MS * 4); // 3 units already elapsed after the previous char
            continue;
        }

        uint8_t bit = 0x80;
        while (!(code & bit)) bit >>= 1;      // skip to the sentinel bit
        for (bit >>= 1; bit; bit >>= 1) {      // walk the elements, MSB first
            const uint16_t on = (code & bit) ? (MORSE_UNIT_MS * 3)   // dah
                                              :  MORSE_UNIT_MS;        // dit
            BK4819_PlayToneRaw(MORSE_TONE_HZ, on);
            SYSTEM_DelayMs(MORSE_UNIT_MS);     // intra-element gap
        }
        SYSTEM_DelayMs(MORSE_UNIT_MS * 2);     // inter-character gap (3 units total)
    }

    AUDIO_AudioPathOff();
    SYSTEM_DelayMs(5);
    BK4819_TurnsOffTones_TurnsOnRX();
    SYSTEM_DelayMs(5);
    BK4819_WriteRegister(BK4819_REG_71, toneConfig);
}

void MORSE_PlayBootId(void)
{
    if (!gEeprom.BOOT_MORSE)
        return;

    char msg[MORSE_MSG_MAX + 1];
    MORSE_LoadMessage(msg);
    MORSE_PlayText(msg);
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
    UI_PrintStringSmallNormal("EXIT=BACK/CANCEL",     4, 0, 6);
}

void MORSE_EditMessage(void)
{
    char buf[MORSE_MSG_MAX + 1];
    MORSE_LoadMessage(buf);

    // Pad the field with spaces so every cursor position shows a selectable cell.
    for (uint8_t i = (uint8_t)strlen(buf); i < MORSE_MSG_MAX; i++)
        buf[i] = ' ';
    buf[MORSE_MSG_MAX] = '\0';

    uint8_t       cursor = 0;
    bool          saved  = false;
    KeyboardState kbd    = {KEY_INVALID, KEY_INVALID, 0};

    BACKLIGHT_UpdateTickless();
    BACKLIGHT_TurnOn();

    for (;;) {
        MORSE_DrawEditor(buf, cursor);
        ST7565_BlitStatusLine();
        ST7565_BlitFullScreen();

        kbd.prev    = kbd.current;
        kbd.current = KEYBOARD_GetKey();

        if (kbd.current == KEY_INVALID || kbd.current == kbd.prev) {
            SYSTEM_DelayMs(10);
            BACKLIGHT_Update();
            continue;
        }

        BACKLIGHT_TurnOn();

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
        } else if (kbd.current == KEY_EXIT) {
            if (cursor == 0)
                break;                      // cancel: discard and leave
            cursor--;
        }
    }

    if (saved) {
        for (int8_t i = MORSE_MSG_MAX - 1; i >= 0 && buf[i] == ' '; i--)
            buf[i] = '\0';                  // trim trailing padding before saving
        MORSE_SaveMessage(buf);
        MORSE_PlayText(buf);                // preview so the user can check it right away
    }
}

void ACTION_MorseMsgEdit(void)
{
    MORSE_EditMessage();
    GUI_SelectNextDisplay(DISPLAY_MAIN);
}

#endif // ENABLE_FEAT_F4HWN_BOOT_MORSE

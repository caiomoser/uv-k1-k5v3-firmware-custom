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

#ifndef HELPER_MORSE_H
#define HELPER_MORSE_H

#ifdef ENABLE_FEAT_F4HWN_BOOT_MORSE

#include <stdbool.h>
#include <stdint.h>

// Maximum length of the boot Morse message (letters/digits/space), NUL excluded.
#define MORSE_MSG_MAX 12

// 16 bytes right after the "Settings Version" block (0x00A160..0x00A16F), in the
// same settings sector, mapped through eeprom_compat.c for backups:
//   byte 0      enabled flag (0/1), loaded into gEeprom.BOOT_MORSE; 0xFF means the
//               block was never written and triggers the one-time migration below
//   byte 1      speed index into the WPM table, loaded into gEeprom.BOOT_MORSE_WPM
//   bytes 2..14 message (MORSE_MSG_MAX chars + NUL)
//   byte 15     reserved
#define MORSE_FLASH_ADDR 0x00A170u

// Earlier builds stored the block at 0x00A0E8, which is actually the DTMF settings
// (0ED0..0EDF, settings.c), corrupting DTMF side tone and timings. Only used by
// the migration in MORSE_LoadSettings.
#define MORSE_OLD_FLASH_ADDR 0x00A0E8u

// Number of selectable speeds (see MORSE_GetWpm).
#define MORSE_WPM_COUNT 4

// Load flag, speed and message from flash (called from SETTINGS_InitEEPROM, after
// the DTMF settings are loaded). Moves a message found at the old address, restoring
// the DTMF settings it overwrote, the first time it runs.
void MORSE_LoadSettings(void);

// Persist gEeprom.BOOT_MORSE / BOOT_MORSE_WPM (called from SETTINGS_SaveSettings).
void MORSE_SaveSettings(void);

// Saved boot message, cached in RAM (never NULL, "" when empty).
const char *MORSE_GetMessage(void);

// Persist msg (truncated to MORSE_MSG_MAX chars) as the boot message.
void MORSE_SaveMessage(const char *msg);

// Words per minute for a speed index (0..MORSE_WPM_COUNT-1).
uint8_t MORSE_GetWpm(uint8_t index);

// Play msg once as CW through the speaker (BK4819 sidetone), no PTT/TX involved.
// Blocks for the duration of the message; a keypress aborts it within one element,
// and the key is swallowed (waits for its release) so it doesn't act afterwards.
void MORSE_PlayText(const char *msg);

// Self-contained modal editor for the boot message: UP/DOWN cycles the character
// at the cursor, MENU advances the cursor (saving once past the last one), a
// short EXIT steps back (cancelling at position 0), and a long-press EXIT
// confirms & saves immediately from any position. Owns the screen and keypad
// until done.
void MORSE_EditMessage(void);

// Wired as an assignable side-key action (see action.c) to open the editor.
void ACTION_MorseMsgEdit(void);

// Play the saved boot message if the feature is enabled in gEeprom. Called from
// the power-on sequence in place of the classic boot beep; returns false (nothing
// played) when disabled or the message is empty, so the caller can beep instead.
bool MORSE_PlayBootId(void);

#endif // ENABLE_FEAT_F4HWN_BOOT_MORSE
#endif // HELPER_MORSE_H

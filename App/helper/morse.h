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

// 16-byte gap between the welcome message (0x00A0C8..0x00A0E7, ui/welcome.c) and
// the DTMF codes block (0x00A0F8.., settings.c). Byte 0 is the enabled flag,
// loaded into gEeprom.BOOT_MORSE by settings.c; bytes 1..13 are the message
// (MORSE_MSG_MAX chars + NUL), read/written by MORSE_LoadMessage/MORSE_SaveMessage;
// bytes 14..15 are reserved.
#define MORSE_FLASH_ADDR 0x00A0E8u

// Read the saved boot message from flash into out[MORSE_MSG_MAX + 1], sanitised
// to the supported charset (space, A-Z, 0-9). Empty/corrupt flash yields "".
void MORSE_LoadMessage(char *out);

// Persist msg (truncated to MORSE_MSG_MAX chars) as the boot message.
void MORSE_SaveMessage(const char *msg);

// Play msg once as CW through the speaker (BK4819 sidetone), no PTT/TX involved.
// Blocks for the duration of the message; a keypress aborts it early.
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
// the power-on sequence in place of (or alongside) the classic boot beep.
void MORSE_PlayBootId(void);

#endif // ENABLE_FEAT_F4HWN_BOOT_MORSE
#endif // HELPER_MORSE_H

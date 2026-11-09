/*
 * xbased — the cheats' names as the Mods & patches screen shows them
 * (cheat_text.c): the trainers' labels written properly, in the
 * interface's language where the name is a common one.
 */
#ifndef NATIVE_APP_UI_CHEAT_TEXT_H
#define NATIVE_APP_UI_CHEAT_TEXT_H

#include <stddef.h>

/* A trainer option's label (raw, as its file has it) in lang (enum UiLang):
 * abbreviations and misspellings spelled out, sentence case, its player and
 * buttons said after it, then translated when it's a name the table knows
 * (else the English, cleaned). "" when the label has nothing to show. */
void cheat_text(const char *raw, int lang, char *out, size_t n);

#endif

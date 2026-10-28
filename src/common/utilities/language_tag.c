/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/language_tag.h"
#include "common/utilities/ascii.h"
#include <string.h>

/* a tag's parts, as their lengths and where they start in it; the language at 0 */
typedef struct {
    size_t language, script_at, script, region_at, region;
} LanguageTag;

/* the scripts of languages written in more than one, for a tag that names none: CLDR's likely subtags. The first of a
   language's rows whose region is the tag's, or "" for any */
static const char* const implied[][3] = {
    {"zh", "TW", "Hant"}, {"zh", "HK", "Hant"}, {"zh", "MO", "Hant"}, {"zh", "", "Hans"}};

/* n bytes of a and b alike, but for their letters' case */
static int alike(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (snx_to_lower(a[i]) != snx_to_lower(b[i])) return 0;
    return 1;
}
/* the part at text[at] of n bytes is word, of any case */
static int part_is(const char* text, size_t at, size_t n, const char* word) {
    return strlen(word) == n && alike(text + at, word, n);
}
static void tag_read(const char* text, LanguageTag* t) {
    size_t n, letters;
    *t = (LanguageTag){0};
    while (snx_is_alpha(text[t->language])) ++t->language;
    size_t at = t->language;
    while (t->language && (text[at] == '-' || text[at] == '_')) {
        ++at;
        for (n = letters = 0; snx_is_alnum(text[at + n]); ++n)
            letters += (size_t)snx_is_alpha(text[at + n]);
        if (n == 4 && letters == 4 && !t->script && !t->region) {
            t->script_at = at;
            t->script = n;
        } else if (!t->region && ((n == 2 && letters == 2) || (n == 3 && !letters))) {
            t->region_at = at;
            t->region = n;
        } else
            break;
        at += n;
    }
}
/* where text's script is: its own, else the one its language and region imply (implied[i][2]); NULL for none */
static const char* script_of(const char* text, const LanguageTag* t) {
    size_t i;
    if (t->script) return text + t->script_at;
    for (i = 0; i < sizeof implied / sizeof *implied; ++i)
        if (part_is(text, 0, t->language, implied[i][0]) &&
            (!implied[i][1][0] || part_is(text, t->region_at, t->region, implied[i][1])))
            return implied[i][2];
    return NULL;
}
int snx_language_fit(const char* tag, const char* locale) {
    LanguageTag a, b;
    tag_read(tag, &a);
    tag_read(locale, &b);
    if (!a.language || a.language != b.language || !alike(tag, locale, a.language)) return 0;
    const char* script_a = script_of(tag, &a);
    const char* script_b = script_of(locale, &b);
    if (script_a && script_b && !alike(script_a, script_b, 4)) return 0;
    if (a.region == b.region && alike(tag + a.region_at, locale + b.region_at, a.region)) return 3;
    return a.region ? 1 : 2;
}

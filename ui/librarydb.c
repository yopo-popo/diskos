/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* librarydb - library.db: the open helper, sort keys, artist splitting and the scanner's writer (see librarydb.h).
 *
 * The writer builds a fresh library.db.new on every scan. It is a throwaway file until ldb_publish renames it over
 * library.db, so it runs with the journal in memory and without syncs (a power cut leaves only a stale .new, removed
 * by the next scan), then syncs the finished file once before the rename. /usr/data is shared with song.db and with
 * diskOS updates, so the file may never grow into the reserve the caller names (max_page_count): running out of room
 * fails the writer, never the scan. Foreign keys are on while building, so every link is checked as it is written. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "librarydb.h"
#include "listeningdb.h"   /* only its path: the history lists attach it */
#include "shelvesdb.h"     /* only its path: the Favourites list attaches it */

#define NEW_PATH LIBRARYDB_PATH ".new"

int ldb_open(const char *path, int flags, sqlite3 **out){
    *out = NULL;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, flags, NULL);
    if(rc == SQLITE_OK) rc = sqlite3_exec(db, "PRAGMA foreign_keys = ON;", 0, 0, 0);
    if(rc == SQLITE_OK){                                  /* proves this build enforces them (not compiled out) */
        sqlite3_stmt *st = NULL;
        rc = sqlite3_prepare_v2(db, "PRAGMA foreign_keys;", -1, &st, NULL);
        if(rc == SQLITE_OK) rc = (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1) ? SQLITE_OK : SQLITE_ERROR;
        sqlite3_finalize(st);
    }
    if(rc != SQLITE_OK){ sqlite3_close(db); return rc; }
    sqlite3_busy_timeout(db, 2000);
    *out = db;
    return SQLITE_OK;
}

/* ---- sort keys ---- */
/* Latin letters with accents and ligatures -> lower-case ASCII ("" = not a Latin letter: kept as it is). Generated
 * from Unicode's compatibility decompositions, plus letters that have none (ß, æ, ø, đ, ł, þ ...). */
static const char LAT_FOLD_A[400][3] = {
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",  /* U+00C0 */
    "d","n","o","o","o","o","o","","o","u","u","u","u","y","th","ss",  /* U+00D0 */
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",  /* U+00E0 */
    "d","n","o","o","o","o","o","","o","u","u","u","u","y","th","y",  /* U+00F0 */
    "a","a","a","a","a","a","c","c","c","c","c","c","c","c","d","d",  /* U+0100 */
    "d","d","e","e","e","e","e","e","e","e","e","e","g","g","g","g",  /* U+0110 */
    "g","g","g","g","h","h","h","h","i","i","i","i","i","i","i","i",  /* U+0120 */
    "i","i","ij","ij","j","j","k","k","k","l","l","l","l","l","l","l",  /* U+0130 */
    "l","l","l","n","n","n","n","n","n","n","n","n","o","o","o","o",  /* U+0140 */
    "o","o","oe","oe","r","r","r","r","r","r","s","s","s","s","s","s",  /* U+0150 */
    "s","s","t","t","t","t","t","t","u","u","u","u","u","u","u","u",  /* U+0160 */
    "u","u","u","u","w","w","y","y","y","z","z","z","z","z","z","s",  /* U+0170 */
    "b","b","","","","","","c","c","d","d","","","","","",  /* U+0180 */
    "","f","f","g","","","","i","k","k","l","","","n","n","o",  /* U+0190 */
    "o","o","","","p","p","","","","","","t","t","t","t","u",  /* U+01A0 */
    "u","","v","y","y","z","z","","","","","","","","","",  /* U+01B0 */
    "","","","","dz","dz","dz","lj","lj","lj","nj","nj","nj","a","a","i",  /* U+01C0 */
    "i","o","o","u","u","u","u","u","u","u","u","u","u","","a","a",  /* U+01D0 */
    "a","a","ae","ae","g","g","g","g","k","k","o","o","o","o","","",  /* U+01E0 */
    "j","dz","dz","dz","g","g","","","n","n","a","a","ae","ae","o","o",  /* U+01F0 */
    "a","a","a","a","e","e","e","e","i","i","i","i","o","o","o","o",  /* U+0200 */
    "r","r","r","r","u","u","u","u","s","s","t","t","","","h","h",  /* U+0210 */
    "","d","","","","","a","a","e","e","o","o","o","o","o","o",  /* U+0220 */
    "o","o","y","y","l","n","t","j","","","a","c","c","l","t","s",  /* U+0230 */
    "z","","","b","u","","e","e","j","j","q","q","r","r","y","y",  /* U+0240 */
};
static const char LAT_FOLD_B[256][3] = {
    "a","a","b","b","b","b","b","b","c","c","d","d","d","d","d","d",  /* U+1E00 */
    "d","d","d","d","e","e","e","e","e","e","e","e","e","e","f","f",  /* U+1E10 */
    "g","g","h","h","h","h","h","h","h","h","h","h","i","i","i","i",  /* U+1E20 */
    "k","k","k","k","k","k","l","l","l","l","l","l","l","l","m","m",  /* U+1E30 */
    "m","m","m","m","n","n","n","n","n","n","n","n","o","o","o","o",  /* U+1E40 */
    "o","o","o","o","p","p","p","p","r","r","r","r","r","r","r","r",  /* U+1E50 */
    "s","s","s","s","s","s","s","s","s","s","t","t","t","t","t","t",  /* U+1E60 */
    "t","t","u","u","u","u","u","u","u","u","u","u","v","v","v","v",  /* U+1E70 */
    "w","w","w","w","w","w","w","w","w","w","x","x","x","x","y","y",  /* U+1E80 */
    "z","z","z","z","z","z","h","t","w","y","","s","","","ss","",  /* U+1E90 */
    "a","a","a","a","a","a","a","a","a","a","a","a","a","a","a","a",  /* U+1EA0 */
    "a","a","a","a","a","a","a","a","e","e","e","e","e","e","e","e",  /* U+1EB0 */
    "e","e","e","e","e","e","e","e","i","i","i","i","o","o","o","o",  /* U+1EC0 */
    "o","o","o","o","o","o","o","o","o","o","o","o","o","o","o","o",  /* U+1ED0 */
    "o","o","o","o","u","u","u","u","u","u","u","u","u","u","u","u",  /* U+1EE0 */
    "u","u","y","y","y","y","y","y","y","y","","","","","","",  /* U+1EF0 */
};

/* one code point from UTF-8; a broken sequence counts as U+FFFD and moves one byte (never past the NUL) */
static unsigned u8_dec(const unsigned char **pp){
    const unsigned char *p = *pp;
    unsigned c = p[0], cp;
    int k;
    if(c < 0x80){ *pp = p + 1; return c; }
    if(c >= 0xC2 && c <= 0xDF){ k = 1; cp = c & 0x1F; }
    else if(c >= 0xE0 && c <= 0xEF){ k = 2; cp = c & 0x0F; }
    else if(c >= 0xF0 && c <= 0xF4){ k = 3; cp = c & 0x07; }
    else { *pp = p + 1; return 0xFFFD; }
    for(int i = 1; i <= k; i++){
        if((p[i] & 0xC0) != 0x80){ *pp = p + 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if((k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF))){
        *pp = p + 1; return 0xFFFD;
    }
    *pp = p + k + 1;
    return cp;
}
static int u8_enc(unsigned c, char *o){
    if(c < 0x80){ o[0] = (char)c; return 1; }
    if(c < 0x800){ o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if(c < 0x10000){ o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}
/* direction marks, zero-width characters, BOM, soft hyphen, Arabic tatweel, variation selectors and tag characters
 * (emoji styles and flags): never part of the order */
static int ignorable(unsigned c){
    return (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2069)
        || c == 0x061C || c == 0xFEFF || c == 0x00AD || c == 0x034F || c == 0x0640 || (c >= 0xFE00 && c <= 0xFE0F)
        || (c >= 0xE0000 && c <= 0xE007F) || (c >= 0xE0100 && c <= 0xE01EF);
}
/* combining marks: accents of decomposed (NFD) text, Hebrew points/cantillation (not maqaf, paseq, sof pasuq) and
 * Arabic harakat */
static int combining(unsigned c){
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0591 && c <= 0x05BD) || c == 0x05BF || c == 0x05C1 || c == 0x05C2
        || c == 0x05C4 || c == 0x05C5 || c == 0x05C7 || (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F)
        || c == 0x0670 || (c >= 0x06D6 && c <= 0x06DC) || (c >= 0x06DF && c <= 0x06E4) || c == 0x06E7 || c == 0x06E8
        || (c >= 0x06EA && c <= 0x06ED) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF)
        || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || c == 0xFB1E;
}
/* spaces, and control characters (a tag with a line break), which count as spaces */
static int space_cp(unsigned c){
    return c == ' ' || c < 0x20 || (c >= 0x7F && c <= 0xA0) || c == 0x3000 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F
        || c == 0x2028 || c == 0x2029;
}
/* Hebrew presentation forms U+FB1D..FB4F (letters with dagesh, wide letters): their letter; 0 = not a letter.
 * U+FB1F and U+FB4F are two letters (handled in fold_cp). */
static const unsigned short HEB_PRES[0x33] = {
    0x05D9,0,0,0x05E2,0x05D0,0x05D3,0x05D4,0x05DB,0x05DC,0x05DD,0x05E8,0x05EA,0,0x05E9,0x05E9,0x05E9,   /* FB1D */
    0x05E9,0x05D0,0x05D0,0x05D0,0x05D1,0x05D2,0x05D3,0x05D4,0x05D5,0x05D6,0,0x05D8,0x05D9,0x05DA,0x05DB,0x05DC, /* FB2D */
    0,0x05DE,0,0x05E0,0x05E1,0,0x05E3,0x05E4,0,0x05E6,0x05E7,0x05E8,0x05E9,0x05EA,0x05D5,0x05D1,          /* FB3D */
    0x05DB,0x05E4,0                                                                                     /* FB4D */
};
static unsigned hebrew_base(unsigned c){          /* final forms sort with their normal letter */
    switch(c){ case 0x05DA: return 0x05DB; case 0x05DD: return 0x05DE; case 0x05DF: return 0x05E0;
               case 0x05E3: return 0x05E4; case 0x05E5: return 0x05E6; }
    return c;
}
enum { CL_SKIP, CL_SPACE, CL_LATIN, CL_HEBREW, CL_OTHER };
/* one code point as it appears in a key (o gets up to 8 bytes, *n of them used) and its class */
static int fold_cp(unsigned c, char *o, int *n){
    *n = 0;
    if(ignorable(c) || combining(c)) return CL_SKIP;
    if(space_cp(c)){ o[0] = ' '; *n = 1; return CL_SPACE; }
    if(c >= 0xFF01 && c <= 0xFF5E) c -= 0xFF01 - 0x21;    /* full-width ASCII */
    /* typographic apostrophes, quotes and dashes (and Hebrew geresh, gershayim, maqaf) sort as their ASCII forms */
    if(c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x201B || c == 0x2032 || c == 0x02BC || c == 0x05F3) c = '\'';
    else if((c >= 0x201C && c <= 0x201F) || c == 0x2033 || c == 0x05F4) c = '"';
    else if((c >= 0x2010 && c <= 0x2015) || c == 0x2212 || c == 0x05BE || c == 0xFE58 || c == 0xFE63) c = '-';
    else if(c == 0x2026){ memcpy(o, "...", 3); *n = 3; return CL_OTHER; }
    if(c >= 0xFB00 && c <= 0xFB06){                        /* Latin ligatures ff fi fl ffi ffl st st */
        static const char *const LIG[7] = { "ff", "fi", "fl", "ffi", "ffl", "st", "st" };
        *n = (int)strlen(LIG[c - 0xFB00]); memcpy(o, LIG[c - 0xFB00], (size_t)*n); return CL_LATIN;
    }
    if(c == 0xFB1F) c = 0x05F2;                            /* Yiddish yod yod patah: the ligature below */
    else if(c == 0xFB4F){ *n = u8_enc(0x05D0, o); *n += u8_enc(0x05DC, o + *n); return CL_HEBREW; }   /* alef lamed */
    else if(c >= 0xFB1D && c <= 0xFB4F && HEB_PRES[c - 0xFB1D]) c = HEB_PRES[c - 0xFB1D];
    if(c < 0x80){
        if(c >= 'A' && c <= 'Z') c += 32;
        o[0] = (char)c; *n = 1;
        return (c >= 'a' && c <= 'z') ? CL_LATIN : CL_OTHER;
    }
    const char *f = NULL;
    if(c >= 0xC0 && c <= 0x24F) f = LAT_FOLD_A[c - 0xC0];
    else if(c >= 0x1E00 && c <= 0x1EFF) f = LAT_FOLD_B[c - 0x1E00];
    if(f && f[0]){ *n = (int)strlen(f); memcpy(o, f, (size_t)*n); return CL_LATIN; }
    if(c >= 0x05D0 && c <= 0x05EA){ *n = u8_enc(hebrew_base(c), o); return CL_HEBREW; }
    if(c >= 0x05F0 && c <= 0x05F2){                        /* Yiddish ligatures: their two letters */
        unsigned a = c == 0x05F2 ? 0x05D9 : 0x05D5, b = c == 0x05F0 ? 0x05D5 : 0x05D9;
        *n = u8_enc(a, o); *n += u8_enc(b, o + *n); return CL_HEBREW;
    }
    if(c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) c += 0x20;            /* Greek and Cyrillic capitals */
    else if(c >= 0x0410 && c <= 0x042F) c += 0x20;
    else if(c >= 0x0400 && c <= 0x040F) c += 0x50;
    *n = u8_enc(c, o);
    return CL_OTHER;
}

/* non-ASCII punctuation and symbols: Latin-1 signs, Hebrew and Arabic punctuation, general punctuation, currency,
 * arrows, math, shapes, dingbats, CJK punctuation, emoji (not digits or letters of any script) */
static int symbol_cp(unsigned c){
    if(c >= 0xA1 && c <= 0xBF)
        return !(c == 0xAA || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA || (c >= 0xBC && c <= 0xBE));
    return c == 0xD7 || c == 0xF7 || c == 0x05C0 || c == 0x05C3 || c == 0x05C6 || c == 0x060C || c == 0x060D
        || c == 0x061B || c == 0x061E || c == 0x061F || (c >= 0x066A && c <= 0x066D) || c == 0x06D4
        || (c >= 0x2016 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205E) || (c >= 0x20A0 && c <= 0x20CF)
        || (c >= 0x2190 && c <= 0x245F) || (c >= 0x2500 && c <= 0x2775) || (c >= 0x2794 && c <= 0x27FF)
        || (c >= 0x2900 && c <= 0x2BFF) || (c >= 0x2E00 && c <= 0x2E7F) || (c >= 0x3001 && c <= 0x3003)
        || (c >= 0x3008 && c <= 0x3011) || (c >= 0x3014 && c <= 0x301F) || c == 0x30FB || (c >= 0xFE10 && c <= 0xFE19)
        || (c >= 0xFE30 && c <= 0xFE6B) || (c >= 0x1F000 && c <= 0x1FAFF);
}
/* a folded character a name may start with that the key skips: punctuation and symbols, so '"Heroes"', '(Baby) Love',
 * '...Like Clockwork' and '!קצת שקט' sort under their first letter. Digits stay, under '#'. */
static int lead_skip(unsigned c, int cl, const char *b){
    if(cl != CL_OTHER) return 0;
    if((unsigned char)b[0] < 0x80) return !(b[0] >= '0' && b[0] <= '9');   /* ASCII after folding */
    return symbol_cp(c);
}

void ldb_sort_key(const char *name, char *out, int cap){
    if(cap <= 0) return;
    if(cap < 2){ out[0] = 0; return; }
    const unsigned char *s = (const unsigned char *)(name ? name : ""), *p = s;
    /* the key starts at the first letter or digit; a name that is only punctuation keeps it all */
    while(*p){
        const unsigned char *at = p;
        char b[8]; int n;
        unsigned c = u8_dec(&p);
        int cl = fold_cp(c, b, &n);
        if(cl == CL_SKIP || cl == CL_SPACE || lead_skip(c, cl, b)) continue;
        s = at;
        break;
    }
    p = s;
    int len = 1, started = 0;
    out[0] = '3';
    while(*p){
        char b[8]; int n;
        int cl = fold_cp(u8_dec(&p), b, &n);
        if(cl == CL_SKIP) continue;
        if(!started){
            if(cl == CL_SPACE) continue;                   /* leading spaces */
            out[0] = cl == CL_LATIN ? '1' : cl == CL_HEBREW ? '2' : '3';
            started = 1;
        }
        if(len + n >= cap) break;                          /* whole characters only */
        memcpy(out + len, b, (size_t)n); len += n;
    }
    while(len > 1 && out[len - 1] == ' ') len--;
    out[len] = 0;
}

int ldb_key_letter(const char *key){
    if(!key) return '#';
    if(key[0] == '1' && key[1] >= 'a' && key[1] <= 'z') return key[1] - 32;
    if(key[0] == '2'){
        const unsigned char *p = (const unsigned char *)key + 1;
        unsigned c = u8_dec(&p);
        /* alef..tav, finals already folded: index 0..21 */
        static const signed char IDX[27] = { 0,1,2,3,4,5,6,7,8,9, 10,10, 11, 12,12, 13,13, 14, 15, 16,16, 17,17, 18, 19, 20, 21 };
        if(c >= 0x05D0 && c <= 0x05EA) return 0x80 + IDX[c - 0x05D0];
    }
    return '#';
}

/* ---- splitting artists and genres ---- */
/* spaces and invisible marks at the ends of a name piece (a direction mark before "feat.", a no-break space) */
static int trim_cp(unsigned c){ return space_cp(c) || ignorable(c); }
static const char *skip_trim(const char *s, const char *e){
    while(s < e){
        const unsigned char *p = (const unsigned char *)s;
        if(!trim_cp(u8_dec(&p)) || (const char *)p > e) break;
        s = (const char *)p;
    }
    return s;
}
/* the start of the character that ends at e (s <= result < e), for stepping back over UTF-8 */
static const char *prev_cp(const char *s, const char *e){
    const char *q = e - 1;
    for(int k = 0; k < 3 && q > s && ((unsigned char)*q & 0xC0) == 0x80; k++) q--;
    return q;
}
static const char *trim_end(const char *s, const char *e){
    while(e > s){
        const char *q = prev_cp(s, e);
        const unsigned char *p = (const unsigned char *)q;
        if(!trim_cp(u8_dec(&p)) || (const char *)p != e) break;
        e = q;
    }
    return e;
}
static void put_name(char (*out)[LDB_NAME], int *n, int max, const char *s, size_t len){
    const char *e = trim_end(s, s + len);
    s = skip_trim(s, e);
    len = (size_t)(e - s);
    if(!len || *n >= max) return;
    if(len >= LDB_NAME){                                   /* cut on a character boundary */
        len = LDB_NAME - 1;
        while(len && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    }
    for(int i = 0; i < *n; i++) if(strlen(out[i]) == len && !memcmp(out[i], s, len)) return;   /* repeat */
    memcpy(out[*n], s, len); out[*n][len] = 0;
    (*n)++;
}
/* "feat." as a whole word: at the start, or after ASCII punctuation, a space or an invisible mark (not a letter) */
static const char *find_feat(const char *s, const char *end){
    for(const char *p = s; p + 5 <= end; p++){
        if(strncasecmp(p, "feat.", 5)) continue;
        if(p > s){
            unsigned char b = (unsigned char)p[-1];
            if((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9')) continue;
            if(b >= 0x80){
                const unsigned char *q = (const unsigned char *)prev_cp(s, p);
                if(!trim_cp(u8_dec(&q))) continue;
            }
        }
        return p;
    }
    return NULL;
}
static void split_feat(const char *s, size_t len, char (*out)[LDB_NAME], int *n, int max){
    const char *end = s + len;
    char close = 0;
    for(;;){
        const char *q = find_feat(s, end);
        const char *e = q ? q : end;
        e = trim_end(s, e); s = skip_trim(s, e);
        if(close && e > s && e[-1] == close){ e = trim_end(s, e - 1); }
        close = 0;
        if(q && e > s && (e[-1] == '(' || e[-1] == '[')){  /* "A (feat. B)": the bracket goes with the credit */
            close = e[-1] == '(' ? ')' : ']';
            e--;
        }
        put_name(out, n, max, s, (size_t)(e - s));
        if(!q) break;
        s = q + 5;
    }
}
int ldb_split_names(const char *const *vals, int nvals, int feat, char (*out)[LDB_NAME], int max){
    int n = 0;
    for(int i = 0; i < nvals; i++){
        const char *p = vals[i];
        if(!p) continue;
        for(;;){
            const char *e = strchr(p, ';');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if(feat) split_feat(p, len, out, &n, max);
            else put_name(out, &n, max, p, len);
            if(!e) break;
            p = e + 1;
        }
    }
    return n;
}

/* ---- the writer ---- */
static const char *SCHEMA_TABLES =
    "CREATE TABLE albums ("
    " id INTEGER PRIMARY KEY, album_artist TEXT NOT NULL, title TEXT NOT NULL, year INTEGER, release_type TEXT,"
    " compilation INTEGER NOT NULL DEFAULT 0, track_total INTEGER, disc_total INTEGER, rg_album_gain REAL, rg_album_peak REAL,"
    " album_artist_sort TEXT, title_sort TEXT, album_artist_key TEXT NOT NULL, title_key TEXT NOT NULL,"
    " UNIQUE (album_artist, title));"
    "CREATE TABLE tracks ("
    " id INTEGER PRIMARY KEY, path TEXT NOT NULL, start_ms INTEGER NOT NULL DEFAULT 0, end_ms INTEGER,"
    " source INTEGER NOT NULL DEFAULT 0, album_artist TEXT, album_title TEXT, title TEXT NOT NULL, disc INTEGER, track INTEGER,"
    " year INTEGER, bpm REAL, initial_key TEXT, rg_track_gain REAL, rg_track_peak REAL, duration_ms INTEGER, sample_rate INTEGER,"
    " bits INTEGER, channels INTEGER, codec TEXT, has_lyrics INTEGER NOT NULL DEFAULT 0, file_mtime INTEGER NOT NULL,"
    " file_size INTEGER NOT NULL, title_key TEXT NOT NULL,"
    " UNIQUE (path, start_ms),"
    " FOREIGN KEY (album_artist, album_title) REFERENCES albums (album_artist, title));"
    "CREATE TABLE artists ("
    " id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, sort_name TEXT, name_key TEXT NOT NULL);"
    "CREATE TABLE track_artists ("
    " id INTEGER PRIMARY KEY, track_id INTEGER NOT NULL REFERENCES tracks (id) ON DELETE CASCADE,"
    " artist TEXT NOT NULL REFERENCES artists (name), position INTEGER NOT NULL, UNIQUE (track_id, position));"
    "CREATE TABLE track_file_genres ("
    " id INTEGER PRIMARY KEY, track_id INTEGER NOT NULL REFERENCES tracks (id) ON DELETE CASCADE,"
    " genre TEXT NOT NULL, UNIQUE (track_id, genre));";
/* The secondary indexes are built once the rows are in (faster than keeping them up to date row by row). */
static const char *INDEXES_LINKS =
    "CREATE INDEX tracks_by_album ON tracks (album_artist, album_title, disc, track);"
    "CREATE INDEX track_artists_by_artist ON track_artists (artist);";
static const char *INDEXES_SORT =
    "CREATE INDEX tracks_by_title ON tracks (title_key);"
    "CREATE INDEX albums_by_title ON albums (title_key);"
    "CREATE INDEX artists_by_name ON artists (name_key);"
    "CREATE INDEX track_file_genres_by_genre ON track_file_genres (genre);";

/* An album's album-level tags merged over its songs: the first release type and sort names seen, compilation if any
 * song says so, the largest totals, and the quieter gain and higher peak when songs disagree (never louder). */
#define ALBUM_MERGE \
    "release_type=COALESCE(albums.release_type,excluded.release_type)," \
    "compilation=MAX(albums.compilation,excluded.compilation)," \
    "track_total=CASE WHEN excluded.track_total>COALESCE(albums.track_total,0) THEN excluded.track_total ELSE albums.track_total END," \
    "disc_total=CASE WHEN excluded.disc_total>COALESCE(albums.disc_total,0) THEN excluded.disc_total ELSE albums.disc_total END," \
    "rg_album_gain=MIN(COALESCE(albums.rg_album_gain,excluded.rg_album_gain),COALESCE(excluded.rg_album_gain,albums.rg_album_gain))," \
    "rg_album_peak=MAX(COALESCE(albums.rg_album_peak,excluded.rg_album_peak),COALESCE(excluded.rg_album_peak,albums.rg_album_peak))," \
    "album_artist_sort=COALESCE(albums.album_artist_sort,excluded.album_artist_sort)," \
    "title_sort=COALESCE(albums.title_sort,excluded.title_sort)"
static const char *UPSERT_ALBUM =
    "INSERT INTO albums (album_artist,title,release_type,compilation,track_total,disc_total,rg_album_gain,rg_album_peak,"
    "album_artist_sort,title_sort,album_artist_key,title_key) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,'','') "
    "ON CONFLICT (album_artist,title) DO UPDATE SET " ALBUM_MERGE ";";
static const char *UPSERT_ARTIST =
    "INSERT INTO artists (name,sort_name,name_key) VALUES (?1,?2,'') "
    "ON CONFLICT (name) DO UPDATE SET sort_name=COALESCE(artists.sort_name,excluded.sort_name);";
#define TRACK_COLS "path,start_ms,end_ms,source,album_artist,album_title,title,disc,track,year,bpm,initial_key," \
                   "rg_track_gain,rg_track_peak,duration_ms,sample_rate,bits,channels,codec,has_lyrics,file_mtime,file_size,title_key"
static const char *INS_TRACK =
    "INSERT INTO tracks (" TRACK_COLS ") VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23) "
    "ON CONFLICT (path,start_ms) DO NOTHING;";
static const char *INS_TRACK_ARTIST = "INSERT INTO track_artists (track_id,artist,position) VALUES (?1,?2,?3);";
static const char *INS_GENRE = "INSERT OR IGNORE INTO track_file_genres (track_id,genre) VALUES (?1,?2);";

/* Songs the scan could not read keep their rows from the previous library.db (same schema version only): the paths
 * asked for in temp.keep, minus .nomedia folders, replace whatever this scan wrote for them. A folder prefix is matched
 * as a range (path >= 'D/' AND path < 'D/' || U+10FFFF), so old.tracks' path index is used, not a scan per keep.
 * LDB_KEEP_CUE_TREE: CUE songs of files directly in the sheet's folder that are still on the card (this scan wrote a
 * row for them) and that no readable sheet described this time. */
#define UNDER(o, k) o ">=" k " AND " o "<" k "||char(1114111) AND substr(" o ",1,length(" k "))=" k
static const char *CARRY_PATHS_SQL =
    "CREATE TEMP TABLE carry_paths (path TEXT PRIMARY KEY);"
    "INSERT OR IGNORE INTO carry_paths SELECT o.path FROM temp.keep k JOIN old.tracks o ON o.path=k.path WHERE k.kind=0;"
    "INSERT OR IGNORE INTO carry_paths SELECT o.path FROM temp.keep k JOIN old.tracks o ON " UNDER("o.path", "k.path")
    " WHERE k.kind=1;"
    "INSERT OR IGNORE INTO carry_paths SELECT o.path FROM temp.keep k JOIN old.tracks o ON " UNDER("o.path", "k.path")
    " WHERE k.kind=2 AND o.source=1 AND instr(substr(o.path,length(k.path)+1),'/')=0"
    " AND EXISTS (SELECT 1 FROM main.tracks t WHERE t.path=o.path)"
    " AND NOT EXISTS (SELECT 1 FROM main.tracks t WHERE t.path=o.path AND t.source=1);"
    "DELETE FROM carry_paths WHERE EXISTS (SELECT 1 FROM temp.nomedia n WHERE substr(carry_paths.path,1,length(n.dir)+1)=n.dir||'/');";
/* Rows carried over plus rows they replace, at most: the carry runs in a savepoint whose undo copy is kept in memory,
 * so it is bounded (about 1 KB per row). A scan that couldn't read more than this isn't published at all. */
#define CARRY_MAX 3000
static const char *CARRY_OLD_SQL = "SELECT count(*) FROM old.tracks WHERE path IN (SELECT path FROM temp.carry_paths);";
static const char *CARRY_CUR_SQL = "SELECT count(*) FROM main.tracks WHERE path IN (SELECT path FROM temp.carry_paths);";
static const char *CARRY_SQL =
    "DELETE FROM main.tracks WHERE path IN (SELECT path FROM carry_paths);"
    "INSERT INTO main.albums (album_artist,title,release_type,compilation,track_total,disc_total,rg_album_gain,rg_album_peak,"
    " album_artist_sort,title_sort,album_artist_key,title_key)"
    " SELECT a.album_artist,a.title,a.release_type,a.compilation,a.track_total,a.disc_total,a.rg_album_gain,a.rg_album_peak,"
    " a.album_artist_sort,a.title_sort,'','' FROM old.albums a WHERE EXISTS (SELECT 1 FROM old.tracks o"
    " WHERE o.album_artist=a.album_artist AND o.album_title=a.title AND o.path IN (SELECT path FROM carry_paths))"
    " ON CONFLICT (album_artist,title) DO UPDATE SET " ALBUM_MERGE ";"
    "INSERT INTO main.artists (name,sort_name,name_key) SELECT a.name,a.sort_name,'' FROM old.artists a WHERE a.name IN"
    " (SELECT ta.artist FROM old.track_artists ta JOIN old.tracks o ON o.id=ta.track_id WHERE o.path IN (SELECT path FROM carry_paths)"
    " UNION SELECT o.album_artist FROM old.tracks o WHERE o.path IN (SELECT path FROM carry_paths))"
    " ON CONFLICT (name) DO UPDATE SET sort_name=COALESCE(artists.sort_name,excluded.sort_name);"
    "INSERT INTO main.tracks (" TRACK_COLS ") SELECT " TRACK_COLS " FROM old.tracks WHERE path IN (SELECT path FROM carry_paths);"
    "INSERT INTO main.track_artists (track_id,artist,position) SELECT n.id,ta.artist,ta.position FROM old.track_artists ta"
    " JOIN old.tracks o ON o.id=ta.track_id JOIN main.tracks n ON n.path=o.path AND n.start_ms=o.start_ms"
    " WHERE o.path IN (SELECT path FROM carry_paths);"
    "INSERT INTO main.track_file_genres (track_id,genre) SELECT n.id,g.genre FROM old.track_file_genres g"
    " JOIN old.tracks o ON o.id=g.track_id JOIN main.tracks n ON n.path=o.path AND n.start_ms=o.start_ms"
    " WHERE o.path IN (SELECT path FROM carry_paths);";
/* Albums and artists left without songs (a whole-file row replaced by CUE tracks, a .nomedia folder), each album's
 * year (the one most of its songs carry, the earliest on a tie), then the sort keys. */
static const char *FINISH_SQL =
    "DELETE FROM albums WHERE NOT EXISTS (SELECT 1 FROM tracks t WHERE t.album_artist=albums.album_artist AND t.album_title=albums.title);"
    "DELETE FROM artists WHERE NOT EXISTS (SELECT 1 FROM track_artists ta WHERE ta.artist=artists.name)"
    " AND NOT EXISTS (SELECT 1 FROM albums a WHERE a.album_artist=artists.name);"
    "UPDATE albums SET year=(SELECT t.year FROM tracks t WHERE t.album_artist=albums.album_artist AND t.album_title=albums.title"
    " AND t.year IS NOT NULL GROUP BY t.year ORDER BY count(*) DESC, t.year LIMIT 1),"
    " album_artist_key=diskos_sort_key(COALESCE(album_artist_sort,album_artist)),"
    " title_key=diskos_sort_key(COALESCE(title_sort,title));"
    "UPDATE artists SET name_key=diskos_sort_key(COALESCE(sort_name,name));";

struct ldb_writer {
    sqlite3 *db;
    sqlite3_stmt *album, *artist, *track, *tartist, *genre, *drop, *keep;
    int failed, have_old, finished, published, in_txn;
    char err[200];
    char **nomedia; int n_nomedia, cap_nomedia;
    int (*cancel)(void *); void *cancel_ctx;
    long long (*reserve)(void);                         /* bytes of /usr/data to leave free, asked as the file grows */
    sqlite3_stmt *unkeep;
    int adds;
    ldb_stats st;
};

static void fail(ldb_writer *w, int rc, const char *what){
    if(w->failed) return;
    w->failed = 1;
    if(rc == SQLITE_FULL) snprintf(w->err, sizeof w->err, "not enough free space on /usr/data (%s)", what);
    else if(rc == SQLITE_INTERRUPT) snprintf(w->err, sizeof w->err, "cancelled");
    else snprintf(w->err, sizeof w->err, "%s: %s", what, w->db ? sqlite3_errmsg(w->db) : "no database");
    fprintf(stderr, "librarydb: %s\n", w->err);
}
static int exec(ldb_writer *w, const char *sql, const char *what){
    if(w->failed) return -1;
    int rc = sqlite3_exec(w->db, sql, 0, 0, 0);
    if(rc != SQLITE_OK){ fail(w, rc, what); return -1; }
    return 0;
}
static int progress_cb(void *p){
    ldb_writer *w = p;
    return w->cancel && w->cancel(w->cancel_ctx);
}
static void sort_key_fn(sqlite3_context *c, int argc, sqlite3_value **v){
    (void)argc;
    char key[LDB_NAME];
    ldb_sort_key((const char *)sqlite3_value_text(v[0]), key, sizeof key);
    sqlite3_result_text(c, key, -1, SQLITE_TRANSIENT);
}
static long long query_int(sqlite3 *db, const char *sql){
    sqlite3_stmt *s = NULL; long long v = -1;
    if(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}
static int mkdir_owner(const char *dir, mode_t mode){
    struct stat st;
    if(mkdir(dir, mode) != 0 && errno != EEXIST) return -1;
    return (lstat(dir, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;   /* a real directory, not a symlink */
}

static long long free_bytes(void){
    struct statvfs vf;
    if(statvfs(LIBRARYDB_DIR, &vf) != 0) return -1;
    return (long long)vf.f_bavail * (long long)vf.f_frsize;
}
/* library.db.new may grow only into what song.db doesn't need: max_page_count = its pages now + what is free beyond
 * the reserve, less the pages still in memory that will reach the disk. Asked again every 256 songs, so a growing
 * song.db (or its journal) always wins and library.db is the one that stops. */
static void recap(ldb_writer *w){
    long long avail = free_bytes(), pages = query_int(w->db, "PRAGMA page_count;");
    struct stat st;
    if(avail < 0 || pages < 0) return;
    long long unwritten = stat(NEW_PATH, &st) == 0 ? pages * 4096 - (long long)st.st_size : 0;
    long long room = avail - w->reserve() - (unwritten > 0 ? unwritten : 0);
    char q[64];
    snprintf(q, sizeof q, "PRAGMA max_page_count=%lld;", pages + (room > 0 ? room / 4096 : 0));
    sqlite3_exec(w->db, q, 0, 0, 0);
}
int ldb_room_ok(ldb_writer *w){
    long long avail = free_bytes();
    return w && avail >= 0 && avail >= w->reserve();
}

#define WHY(...) do{ if(why && whycap > 0) snprintf(why, (size_t)whycap, __VA_ARGS__); }while(0)
int ldb_make_dir(void){
    char parent[sizeof LIBRARYDB_DIR];
    snprintf(parent, sizeof parent, "%s", LIBRARYDB_DIR);
    char *sl = strrchr(parent, '/');
    if(sl && sl != parent){ *sl = 0; mkdir_owner(parent, 0755); }
    return mkdir_owner(LIBRARYDB_DIR, 0700);
}
ldb_writer *ldb_begin(long long (*reserve)(void), char *why, int whycap){
    WHY("%s", "");
    if(ldb_make_dir() != 0){
        WHY("can't create %s (%s)", LIBRARYDB_DIR, strerror(errno));
        return NULL;
    }
    unlink(NEW_PATH); unlink(NEW_PATH "-journal");                 /* left by a scan that was cut off */
    long long avail = free_bytes(), keep_free = reserve();
    if(avail < 0){ WHY("can't read free space (%s)", strerror(errno)); return NULL; }
    long long room = avail - keep_free;
    if(room < 2LL * 1024 * 1024){
        WHY("only %lld MB free on /usr/data, %lld MB must stay free", avail >> 20, keep_free >> 20);
        return NULL;
    }
    ldb_writer *w = calloc(1, sizeof *w);
    if(!w){ WHY("out of memory"); return NULL; }
    w->reserve = reserve;
    int rc = ldb_open(NEW_PATH, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX, &w->db);
    if(rc != SQLITE_OK){
        WHY("can't create library.db.new (%s)", sqlite3_errstr(rc));
        free(w); unlink(NEW_PATH);
        return NULL;
    }
    sqlite3_busy_timeout(w->db, 0);                    /* nobody else opens the .new file */
    char pragmas[256];
    snprintf(pragmas, sizeof pragmas, "PRAGMA page_size=4096; PRAGMA journal_mode=MEMORY; PRAGMA synchronous=OFF;"
             "PRAGMA temp_store=MEMORY; PRAGMA cache_size=-2048; PRAGMA max_page_count=%lld;", room / 4096);
    exec(w, pragmas, "setting up");
    if(!w->failed && sqlite3_create_function(w->db, "diskos_sort_key", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                                             sort_key_fn, NULL, NULL) != SQLITE_OK) fail(w, SQLITE_ERROR, "sort keys");
    /* the previous library.db, read only, for carrying over songs this scan can't read */
    struct stat ost;
    if(!w->failed && lstat(LIBRARYDB_PATH, &ost) == 0 && S_ISREG(ost.st_mode)){
        if(sqlite3_exec(w->db, "ATTACH 'file:" LIBRARYDB_PATH "?mode=ro' AS old;", 0, 0, 0) == SQLITE_OK){
            if(query_int(w->db, "PRAGMA old.user_version;") == LIBRARYDB_SCHEMA) w->have_old = 1;
            else sqlite3_exec(w->db, "DETACH old;", 0, 0, 0);   /* another schema: rebuilt from the card alone */
        }
    }
    exec(w, "BEGIN;", "starting");
    if(!w->failed) w->in_txn = 1;
    exec(w, SCHEMA_TABLES, "creating tables");
    exec(w, "CREATE TEMP TABLE keep (path TEXT NOT NULL, kind INTEGER NOT NULL, PRIMARY KEY (path, kind));"
            "CREATE TEMP TABLE nomedia (dir TEXT PRIMARY KEY);", "creating tables");
    struct { sqlite3_stmt **s; const char *sql; } prep[] = {
        { &w->album, UPSERT_ALBUM }, { &w->artist, UPSERT_ARTIST }, { &w->track, INS_TRACK },
        { &w->tartist, INS_TRACK_ARTIST }, { &w->genre, INS_GENRE },
        { &w->drop, "DELETE FROM tracks WHERE path=?1;" },
        { &w->keep, "INSERT OR IGNORE INTO temp.keep (path, kind) VALUES (?1, ?2);" },
        { &w->unkeep, "DELETE FROM temp.keep WHERE path=?1 AND kind=0;" },
    };
    for(unsigned i = 0; i < sizeof prep / sizeof prep[0] && !w->failed; i++){
        rc = sqlite3_prepare_v2(w->db, prep[i].sql, -1, prep[i].s, NULL);
        if(rc != SQLITE_OK) fail(w, rc, "preparing");
    }
    if(w->failed){
        WHY("%s", w->err);
        ldb_end(w);
        return NULL;
    }
    return w;
}

int ldb_failed(const ldb_writer *w){ return !w || w->failed; }
const char *ldb_error(const ldb_writer *w){ return w ? w->err : "not started"; }
void ldb_get_stats(const ldb_writer *w, ldb_stats *st){ if(w) *st = w->st; else memset(st, 0, sizeof *st); }

int ldb_in_nomedia(const ldb_writer *w, const char *path){
    if(!w) return 0;
    for(int i = 0; i < w->n_nomedia; i++){
        size_t n = strlen(w->nomedia[i]);
        if(!strncmp(path, w->nomedia[i], n) && (path[n] == '/' || path[n] == 0)) return 1;
    }
    return 0;
}
void ldb_nomedia(ldb_writer *w, const char *dir){
    if(!w || w->failed || ldb_in_nomedia(w, dir)) return;
    if(w->n_nomedia == w->cap_nomedia){
        int nc = w->cap_nomedia ? w->cap_nomedia * 2 : 8;
        void *p = realloc(w->nomedia, (size_t)nc * sizeof *w->nomedia);
        if(!p){ fail(w, SQLITE_NOMEM, ".nomedia list"); return; }
        w->nomedia = p; w->cap_nomedia = nc;
    }
    char *d = strdup(dir);
    if(!d){ fail(w, SQLITE_NOMEM, ".nomedia list"); return; }
    w->nomedia[w->n_nomedia++] = d;
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_prepare_v2(w->db, "INSERT OR IGNORE INTO temp.nomedia (dir) VALUES (?1);", -1, &s, NULL);
    if(rc == SQLITE_OK){ sqlite3_bind_text(s, 1, dir, -1, SQLITE_TRANSIENT); rc = sqlite3_step(s) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(w->db); }
    sqlite3_finalize(s);
    if(rc != SQLITE_OK) fail(w, rc, ".nomedia list");
}

static void bind_text_or_null(sqlite3_stmt *s, int i, const char *v){
    if(v && v[0]) sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(s, i);
}
static void bind_int_or_null(sqlite3_stmt *s, int i, long long v){
    if(v > 0) sqlite3_bind_int64(s, i, v); else sqlite3_bind_null(s, i);
}
static int step_done(ldb_writer *w, sqlite3_stmt *s, const char *what){
    int rc = sqlite3_step(s);
    sqlite3_reset(s);
    if(rc != SQLITE_DONE){ fail(w, rc == SQLITE_ROW ? SQLITE_ERROR : rc, what); return -1; }
    return 0;
}
static int upsert_artist(ldb_writer *w, const char *name, const char *sort){
    sqlite3_clear_bindings(w->artist);
    sqlite3_bind_text(w->artist, 1, name, -1, SQLITE_TRANSIENT);
    bind_text_or_null(w->artist, 2, sort);
    return step_done(w, w->artist, "adding an artist");
}

int ldb_add(ldb_writer *w, const ldb_track *t){
    if(!w || w->failed) return -1;
    if(++w->adds % 256 == 0) recap(w);
    if(!t->path || !t->title || !t->title[0]) return 0;
    if(ldb_in_nomedia(w, t->path)) return 0;
    const char *album = t->album_title && t->album_title[0] ? t->album_title : NULL;
    const char *aa = album ? (t->album_artist ? t->album_artist : "") : NULL;
    if(album){
        sqlite3_stmt *s = w->album;
        sqlite3_clear_bindings(s);
        sqlite3_bind_text(s, 1, aa, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, album, -1, SQLITE_TRANSIENT);
        bind_text_or_null(s, 3, t->release_type);
        sqlite3_bind_int(s, 4, t->compilation ? 1 : 0);
        bind_int_or_null(s, 5, t->track_total);
        bind_int_or_null(s, 6, t->disc_total);
        if(t->rg_flags & LDB_RG_AG) sqlite3_bind_double(s, 7, t->rg_album_gain); else sqlite3_bind_null(s, 7);
        if(t->rg_flags & LDB_RG_AP) sqlite3_bind_double(s, 8, t->rg_album_peak); else sqlite3_bind_null(s, 8);
        bind_text_or_null(s, 9, t->album_artist_sort);
        bind_text_or_null(s, 10, t->album_sort);
        if(step_done(w, s, "adding an album")) return -1;
        if(aa[0] && upsert_artist(w, aa, t->album_artist_sort)) return -1;
    }
    for(int i = 0; i < t->n_artists; i++)
        if(upsert_artist(w, t->artists[i], t->artist_sorts ? t->artist_sorts[i] : NULL)) return -1;
    char key[LDB_NAME];
    ldb_sort_key(t->title_sort && t->title_sort[0] ? t->title_sort : t->title, key, sizeof key);
    sqlite3_stmt *s = w->track;
    sqlite3_clear_bindings(s);
    sqlite3_bind_text(s, 1, t->path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, t->start_ms > 0 ? t->start_ms : 0);
    if(t->end_ms > 0) sqlite3_bind_int64(s, 3, t->end_ms); else sqlite3_bind_null(s, 3);
    sqlite3_bind_int(s, 4, t->source);
    if(album){ sqlite3_bind_text(s, 5, aa, -1, SQLITE_TRANSIENT); sqlite3_bind_text(s, 6, album, -1, SQLITE_TRANSIENT); }
    else { sqlite3_bind_null(s, 5); sqlite3_bind_null(s, 6); }
    sqlite3_bind_text(s, 7, t->title, -1, SQLITE_TRANSIENT);
    bind_int_or_null(s, 8, t->disc);
    bind_int_or_null(s, 9, t->track);
    bind_int_or_null(s, 10, t->year);
    if(t->bpm > 0) sqlite3_bind_double(s, 11, t->bpm); else sqlite3_bind_null(s, 11);
    bind_text_or_null(s, 12, t->initial_key);
    if(t->rg_flags & LDB_RG_TG) sqlite3_bind_double(s, 13, t->rg_track_gain); else sqlite3_bind_null(s, 13);
    if(t->rg_flags & LDB_RG_TP) sqlite3_bind_double(s, 14, t->rg_track_peak); else sqlite3_bind_null(s, 14);
    bind_int_or_null(s, 15, t->duration_ms);
    bind_int_or_null(s, 16, t->sample_rate);
    bind_int_or_null(s, 17, t->bits);
    bind_int_or_null(s, 18, t->channels);
    bind_text_or_null(s, 19, t->codec);
    sqlite3_bind_int(s, 20, t->has_lyrics);
    sqlite3_bind_int64(s, 21, t->file_mtime);
    sqlite3_bind_int64(s, 22, t->file_size);
    sqlite3_bind_text(s, 23, key, -1, SQLITE_TRANSIENT);
    if(step_done(w, s, "adding a song")) return -1;
    if(sqlite3_changes(w->db) == 0) return 0;          /* the same path and start twice: the first one stays */
    sqlite3_int64 id = sqlite3_last_insert_rowid(w->db);
    for(int i = 0; i < t->n_artists; i++){
        sqlite3_clear_bindings(w->tartist);
        sqlite3_bind_int64(w->tartist, 1, id);
        sqlite3_bind_text(w->tartist, 2, t->artists[i], -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(w->tartist, 3, i);
        if(step_done(w, w->tartist, "adding a song's artist")) return -1;
    }
    for(int i = 0; i < t->n_genres; i++){
        sqlite3_clear_bindings(w->genre);
        sqlite3_bind_int64(w->genre, 1, id);
        sqlite3_bind_text(w->genre, 2, t->genres[i], -1, SQLITE_TRANSIENT);
        if(step_done(w, w->genre, "adding a genre")) return -1;
    }
    return 0;
}

void ldb_drop_path(ldb_writer *w, const char *path){
    if(!w || w->failed) return;
    sqlite3_clear_bindings(w->drop);
    sqlite3_bind_text(w->drop, 1, path, -1, SQLITE_TRANSIENT);
    if(step_done(w, w->drop, "replacing a file's row")) return;
    sqlite3_clear_bindings(w->unkeep);                 /* the sheet's songs replace an unreadable file's old row too */
    sqlite3_bind_text(w->unkeep, 1, path, -1, SQLITE_TRANSIENT);
    step_done(w, w->unkeep, "replacing a file's row");
}
void ldb_keep(ldb_writer *w, const char *path, int kind){
    if(!w || w->failed || !w->have_old) return;        /* nothing to carry over from */
    sqlite3_clear_bindings(w->keep);
    sqlite3_bind_text(w->keep, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(w->keep, 2, kind);
    step_done(w, w->keep, "noting an unreadable file");
}

static void finalize_all(ldb_writer *w){
    sqlite3_stmt **all[] = { &w->album, &w->artist, &w->track, &w->tartist, &w->genre, &w->drop, &w->keep, &w->unkeep };
    for(unsigned i = 0; i < sizeof all / sizeof all[0]; i++){ sqlite3_finalize(*all[i]); *all[i] = NULL; }
}

/* Carry the songs in temp.keep over from the previous file. Inside a savepoint: if the previous file can't be read,
 * nothing is carried (the songs stay as this scan read them) and the scan report says so. */
static void carry_over(ldb_writer *w){
    if(!w->have_old || query_int(w->db, "SELECT count(*) FROM temp.keep;") <= 0) return;
    if(exec(w, "SAVEPOINT carry;", "carrying over")) return;
    int rc = sqlite3_exec(w->db, CARRY_PATHS_SQL, 0, 0, 0);
    if(rc == SQLITE_OK){
        long long n_old = query_int(w->db, CARRY_OLD_SQL), n_cur = n_old < 0 ? -1 : query_int(w->db, CARRY_CUR_SQL);
        long long n = n_cur < 0 ? -1 : n_old + n_cur;
        if(n > CARRY_MAX){
            exec(w, "ROLLBACK TO carry; RELEASE carry;", "carrying over");
            if(!w->failed){
                w->failed = 1;
                snprintf(w->err, sizeof w->err, "%lld songs couldn't be read this scan, too many to carry over", n_old);
                fprintf(stderr, "librarydb: %s\n", w->err);
            }
            return;
        }
        if(n >= 0) rc = sqlite3_exec(w->db, CARRY_SQL, 0, 0, 0);
        else rc = sqlite3_errcode(w->db);
    }
    if(rc == SQLITE_OK){
        w->st.carried = query_int(w->db, "SELECT count(*) FROM main.tracks WHERE path IN (SELECT path FROM carry_paths);");
        exec(w, "RELEASE carry;", "carrying over");
        return;
    }
    if(rc == SQLITE_INTERRUPT || rc == SQLITE_FULL || rc == SQLITE_NOMEM){ fail(w, rc, "carrying over"); return; }
    fprintf(stderr, "librarydb: previous library.db unreadable (%s): nothing carried over\n", sqlite3_errmsg(w->db));
    w->st.carry_failed = 1;
    exec(w, "ROLLBACK TO carry; RELEASE carry;", "carrying over");
}

static int fsync_path(const char *path, int flags){
    int fd = open(path, flags);
    if(fd < 0) return -1;
    int r = fsync(fd);
    close(fd);
    return r;
}

int ldb_finish(ldb_writer *w, int (*cancel)(void *), void *ctx){
    if(!w || w->failed) return -1;
    w->cancel = cancel; w->cancel_ctx = ctx;
    sqlite3_progress_handler(w->db, 4000, progress_cb, w);
    finalize_all(w);
    recap(w);
    carry_over(w);
    exec(w, INDEXES_LINKS, "building indexes");
    exec(w, FINISH_SQL, "finishing albums and artists");
    exec(w, INDEXES_SORT, "building indexes");
    char ver[64]; snprintf(ver, sizeof ver, "PRAGMA user_version=%d;", LIBRARYDB_SCHEMA);
    exec(w, ver, "writing the version");
    if(exec(w, "COMMIT;", "saving") == 0) w->in_txn = 0;
    if(!w->failed){                                    /* every link resolves (enforced as written; checked once more) */
        sqlite3_stmt *s = NULL;
        int rc = sqlite3_prepare_v2(w->db, "PRAGMA main.foreign_key_check;", -1, &s, NULL);
        if(rc == SQLITE_OK){ rc = sqlite3_step(s); if(rc == SQLITE_DONE) rc = SQLITE_OK; else if(rc == SQLITE_ROW) rc = SQLITE_CONSTRAINT; }
        sqlite3_finalize(s);
        if(rc != SQLITE_OK) fail(w, rc, "checking links");
    }
    if(!w->failed){
        w->st.tracks = query_int(w->db, "SELECT count(*) FROM tracks;");
        w->st.albums = query_int(w->db, "SELECT count(*) FROM albums;");
        w->st.artists = query_int(w->db, "SELECT count(*) FROM artists;");
    }
    sqlite3_progress_handler(w->db, 0, NULL, NULL);
    if(w->have_old) sqlite3_exec(w->db, "DETACH old;", 0, 0, 0);
    int crc = sqlite3_close(w->db);
    w->db = NULL;
    if(w->failed) return -1;
    if(crc != SQLITE_OK){ fail(w, crc, "closing"); return -1; }
    struct stat st;
    if(fsync_path(NEW_PATH, O_RDONLY) != 0 || stat(NEW_PATH, &st) != 0){
        w->failed = 1; snprintf(w->err, sizeof w->err, "saving library.db.new: %s", strerror(errno));
        return -1;
    }
    w->st.bytes = (long long)st.st_size;
    w->finished = 1;
    return 0;
}

int ldb_publish(ldb_writer *w){
    if(!w || w->failed || !w->finished) return -1;
    if(rename(NEW_PATH, LIBRARYDB_PATH) != 0){
        w->failed = 1; snprintf(w->err, sizeof w->err, "replacing library.db: %s", strerror(errno));
        return -1;
    }
    w->published = 1;
    fsync_path(LIBRARYDB_DIR, O_RDONLY | O_DIRECTORY);  /* the rename itself survives a power cut */
    return 0;
}

void ldb_end(ldb_writer *w){
    if(!w) return;
    finalize_all(w);
    if(w->db){
        sqlite3_progress_handler(w->db, 0, NULL, NULL);
        if(w->in_txn) sqlite3_exec(w->db, "ROLLBACK;", 0, 0, 0);
        sqlite3_close(w->db);
    }
    if(!w->published){ unlink(NEW_PATH); unlink(NEW_PATH "-journal"); }
    for(int i = 0; i < w->n_nomedia; i++) free(w->nomedia[i]);
    free(w->nomedia);
    free(w);
}

/* ---- the reader ---- */
/* The columns every list query returns, in this order: title, artist, path, album, album_artist, start_ms, source,
 * disc, track, duration_ms, count, sort key. */
#define TRACK_ARTISTS "(SELECT group_concat(artist, ', ') FROM (SELECT artist FROM track_artists WHERE track_id=t.id ORDER BY position))"
#define SONG_COLS "t.title," TRACK_ARTISTS ",t.path,t.album_title,t.album_artist,t.start_ms,t.source,t.disc,t.track,t.duration_ms,NULL,t.title_key"
/* songs by title, then their first artist */
#define SONG_FROM "FROM tracks t LEFT JOIN track_artists ta ON ta.track_id=t.id AND ta.position=0 LEFT JOIN artists a ON a.name=ta.artist"
#define SONG_ORDER "ORDER BY t.title_key, a.name_key, t.title, t.album_title, t.disc, t.track, t.path, t.start_ms"
static const char *READ_SONGS = "SELECT " SONG_COLS " " SONG_FROM " " SONG_ORDER ";";
#define ALBUM_ORDER "ORDER BY t.disc, t.track, t.title_key, t.path, t.start_ms"
static const char *READ_ALBUM_SONGS =
    "SELECT " SONG_COLS " FROM tracks t WHERE t.album_artist=?1 AND t.album_title=?2 " ALBUM_ORDER ";";
static const char *READ_ALBUMS =
    "SELECT a.title, a.album_artist,"
    " (SELECT t.path FROM tracks t WHERE t.album_artist=a.album_artist AND t.album_title=a.title " ALBUM_ORDER " LIMIT 1),"
    " NULL, NULL, 0, 0, 0, 0, 0,"
    " (SELECT count(*) FROM tracks t WHERE t.album_artist=a.album_artist AND t.album_title=a.title), a.title_key, a.year"
    " FROM albums a ORDER BY a.title_key, a.album_artist_key, a.title, a.album_artist;";

/* The Artists view's key of song t under "By Album Artist": its album artist, else (no album) its first artist. */
#define GROUP_KEY "COALESCE(t.album_artist, (SELECT x.artist FROM track_artists x WHERE x.track_id=t.id AND x.position=0), '')"
/* Artists: the names k, by their sort key; "" (Unknown artist) last */
#define ARTIST_ROWS(k) "SELECT k.name, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0," \
    " CASE WHEN k.name='' THEN '4' ELSE COALESCE(a.name_key, '3') END AS key" \
    " FROM (" k ") k LEFT JOIN artists a ON a.name=k.name ORDER BY key, k.name;"
static const char *READ_ARTISTS = ARTIST_ROWS("SELECT artist AS name FROM track_artists UNION"
    " SELECT '' WHERE EXISTS (SELECT 1 FROM tracks t WHERE t.id NOT IN (SELECT track_id FROM track_artists))");
static const char *READ_GROUP_ARTISTS = ARTIST_ROWS("SELECT DISTINCT " GROUP_KEY " AS name FROM tracks t");
/* genres have no stored sort key: the reader's diskos_sort_key makes it, with the scanner's rule */
static const char *READ_GENRES =
    "SELECT g.genre, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, count(*), diskos_sort_key(g.genre) AS key"
    " FROM track_file_genres g GROUP BY g.genre ORDER BY key, g.genre;";
/* A scope's lists, with %s = the scope's condition on song t (scope_cond, its key in ?1). */
static const char *READ_SCOPE_SONGS = "SELECT " SONG_COLS " " SONG_FROM " WHERE %s " SONG_ORDER ";";
static const char *READ_SCOPE_ALBUM_SONGS =
    "SELECT " SONG_COLS " FROM tracks t WHERE t.album_artist=?2 AND t.album_title=?3 AND %s " ALBUM_ORDER ";";
static const char *READ_SCOPE_COUNT = "SELECT count(*) FROM tracks t WHERE %s;";
#define SCOPE_ALBUMS(s, order) "SELECT a.title, a.album_artist," \
    " (SELECT t.path FROM tracks t WHERE t.album_artist=a.album_artist AND t.album_title=a.title " ALBUM_ORDER " LIMIT 1)," \
    " NULL, NULL, 0, 0, 0, 0, 0, s.n, a.title_key, a.year FROM (" s ") s JOIN albums a ON a.album_artist=s.aa AND a.title=s.at" \
    " ORDER BY " order ";"
#define ALBUMS_BY_TITLE "a.title_key, a.album_artist_key, a.title, a.album_artist"
#define ALBUMS_BY_YEAR  "IFNULL(a.year, 0)=0, a.year, " ALBUMS_BY_TITLE   /* oldest first, undated last */
/* an artist's albums: those with a song of the scope, counting those songs; by year (yopo, 2026-10-20) */
static const char *READ_SCOPE_ALBUMS = SCOPE_ALBUMS("SELECT t.album_artist AS aa, t.album_title AS at, count(*) AS n"
    " FROM tracks t WHERE t.album_title IS NOT NULL AND %s GROUP BY t.album_artist, t.album_title", ALBUMS_BY_YEAR);
/* a genre's albums: only those whose every song has the genre (yopo, 2026-10-17) */
static const char *READ_GENRE_ALBUMS = SCOPE_ALBUMS("SELECT t.album_artist AS aa, t.album_title AS at, count(*) AS n"
    " FROM tracks t WHERE (t.album_artist, t.album_title) IN (SELECT t2.album_artist, t2.album_title"
    " FROM track_file_genres g JOIN tracks t2 ON t2.id=g.track_id WHERE g.genre=?1 AND t2.album_title IS NOT NULL)"
    " GROUP BY t.album_artist, t.album_title HAVING min(%s)=1", ALBUMS_BY_TITLE);
/* Most Played and Recent: the songs of listening.db's play counts that are in the library (attached as ls) */
#define HISTORY_FROM "FROM ls.listened_songs h JOIN tracks t ON t.path=h.path AND t.start_ms=h.start_ms"
static const char *const READ_HISTORY[2] = {
    "SELECT " SONG_COLS " " HISTORY_FROM " ORDER BY h.play_count DESC, h.last_play_seq DESC;",
    "SELECT " SONG_COLS " " HISTORY_FROM " ORDER BY h.last_play_seq DESC;" };
/* A shelves.db song (track_refs r, attached as sh) with its library row t. A song not in the library comes from its
 * snapshot (its title, else its file name; no artist) with count 1. */
#define SHELF_SONG_COLS \
    "COALESCE(t.title, r.title, replace(r.path, rtrim(r.path, replace(r.path, '/', '')), ''))," \
    " CASE WHEN t.id IS NULL THEN NULL ELSE " TRACK_ARTISTS " END," \
    " r.path, COALESCE(t.album_title, r.album_title), CASE WHEN t.id IS NULL THEN IFNULL(r.album_artist, '') ELSE t.album_artist END," \
    " r.start_ms, IFNULL(t.source, 0), IFNULL(t.disc, 0), IFNULL(t.track, 0), IFNULL(t.duration_ms, 0), t.id IS NULL, NULL"
#define SHELF_SONG_JOIN "JOIN sh.track_refs r ON r.id=x.track_ref_id LEFT JOIN tracks t ON t.path=r.path AND t.start_ms=r.start_ms"
/* Favourites, newest first */
static const char *READ_FAVOURITES =
    "SELECT " SHELF_SONG_COLS " FROM sh.favourites x " SHELF_SONG_JOIN " ORDER BY x.id DESC;";
/* A playlist's songs in order (?1 = its id), each with its entry's id */
static const char *READ_PLAYLIST =
    "SELECT " SHELF_SONG_COLS ", NULL, x.id FROM sh.playlist_items x " SHELF_SONG_JOIN
    " WHERE x.playlist_id=?1 ORDER BY x.position, x.id;";
/* An album a's row, as READ_ALBUMS reads it (its first song's file, its song count) */
#define ALBUM_COLS "a.title, a.album_artist," \
    " (SELECT t.path FROM tracks t WHERE t.album_artist=a.album_artist AND t.album_title=a.title " ALBUM_ORDER " LIMIT 1)," \
    " NULL, NULL, 0, 0, 0, 0, 0," \
    " (SELECT count(*) FROM tracks t WHERE t.album_artist=a.album_artist AND t.album_title=a.title), a.title_key, a.year"
/* A collection's albums (?1 = its id) that are in the library, as the Albums list sorts them */
static const char *READ_COLLECTION =
    "SELECT " ALBUM_COLS " FROM sh.collection_albums c JOIN albums a ON a.album_artist=c.album_artist AND a.title=c.album_title"
    " WHERE c.collection_id=?1 ORDER BY " ALBUMS_BY_TITLE ";";
/* The albums in no collection (Unsorted), as the Albums list sorts them */
static const char *READ_UNCOLLECTED =
    "SELECT " ALBUM_COLS " FROM albums a WHERE NOT EXISTS (SELECT 1 FROM sh.collection_albums c"
    " WHERE c.album_artist=a.album_artist AND c.album_title=a.title) ORDER BY " ALBUMS_BY_TITLE ";";
static const char *scope_cond(int kind, const char *key){
    switch(kind){
    case LDB_SCOPE_GENRE: return "t.id IN (SELECT g.track_id FROM track_file_genres g WHERE g.genre=?1)";
    case LDB_SCOPE_GROUP: return GROUP_KEY "=?1";
    default: return key[0] ? "t.id IN (SELECT x.track_id FROM track_artists x WHERE x.artist=?1)"
                           : "(?1='' AND t.id NOT IN (SELECT x.track_id FROM track_artists x))";   /* Unknown artist */
    }
}

static void file_stamp(long long s[4]){
    struct stat st;
    if(stat(LIBRARYDB_PATH, &st) != 0){ s[0] = s[1] = s[2] = s[3] = 0; return; }
    s[0] = (long long)st.st_ino; s[1] = (long long)st.st_size;
    s[2] = (long long)st.st_mtim.tv_sec; s[3] = (long long)st.st_mtim.tv_nsec;
}
int ldb_list_current(const ldb_list *l){
    long long s[4];
    file_stamp(s);
    return l && l->stamp[0] && !memcmp(s, l->stamp, sizeof s);
}
void ldb_list_free(ldb_list *l){
    if(!l) return;
    free(l->v); free(l->pool);
    memset(l, 0, sizeof *l);
}
/* library.db, read-only, only when it carries this schema version */
static sqlite3 *reader_open(void){
    sqlite3 *db = NULL;
    if(ldb_open(LIBRARYDB_PATH, SQLITE_OPEN_READONLY, &db) != SQLITE_OK) return NULL;
    if(query_int(db, "PRAGMA user_version;") != LIBRARYDB_SCHEMA ||
       sqlite3_create_function(db, "diskos_sort_key", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sort_key_fn, NULL, NULL) != SQLITE_OK){
        sqlite3_close(db);
        return NULL;
    }
    return db;
}
/* Strings go into one pool as offsets while it grows; they become pointers once it is final. */
#define NOSTR ((size_t)-1)
typedef struct { ldb_item it; size_t off[5]; } row_build;
static size_t pool_add(char **pool, size_t *len, size_t *cap, const unsigned char *s, int null_ok){
    if(!s){ if(null_ok) return NOSTR; s = (const unsigned char *)""; }
    size_t n = strlen((const char *)s) + 1;
    if(*len + n > *cap){
        size_t nc = *cap ? *cap * 2 : 65536;
        while(nc < *len + n) nc *= 2;
        char *p = realloc(*pool, nc);
        if(!p) return NOSTR - 1;
        *pool = p; *cap = nc;
    }
    memcpy(*pool + *len, s, n);
    size_t at = *len; *len += n;
    return at;
}
static int read_list(sqlite3_stmt *st, ldb_list *out){
    row_build *rows = NULL; int n = 0, cap = 0, ok = 1, rc = SQLITE_DONE;
    char *pool = NULL; size_t plen = 0, pcap = 0;
    while(ok && (rc = sqlite3_step(st)) == SQLITE_ROW){
        if(n == cap){
            int nc = cap ? cap * 2 : 256;
            row_build *p = realloc(rows, (size_t)nc * sizeof *rows);
            if(!p){ ok = 0; break; }
            rows = p; cap = nc;
        }
        row_build *r = &rows[n];
        memset(r, 0, sizeof *r);
        for(int c = 0; c < 5; c++){
            r->off[c] = pool_add(&pool, &plen, &pcap, sqlite3_column_text(st, c), c >= 3);
            if(r->off[c] == NOSTR - 1) ok = 0;
        }
        r->it.start_ms = sqlite3_column_int64(st, 5);
        r->it.source = sqlite3_column_int(st, 6);
        r->it.disc = sqlite3_column_int(st, 7);
        r->it.track = sqlite3_column_int(st, 8);
        r->it.duration_ms = sqlite3_column_int(st, 9);
        r->it.count = sqlite3_column_int(st, 10);
        r->it.letter = ldb_key_letter((const char *)sqlite3_column_text(st, 11));
        r->it.year = sqlite3_column_count(st) > 12 ? sqlite3_column_int(st, 12) : 0;   /* album lists: their year */
        r->it.row_id = sqlite3_column_count(st) > 13 ? sqlite3_column_int64(st, 13) : 0;   /* playlists: the entry */
        n++;
    }
    if(ok && rc != SQLITE_DONE) ok = 0;
    ldb_item *v = ok ? malloc((size_t)(n ? n : 1) * sizeof *v) : NULL;
    if(!v){ free(rows); free(pool); return -1; }
    for(int i = 0; i < n; i++){
        const char **f[5] = { &rows[i].it.title, &rows[i].it.artist, &rows[i].it.path, &rows[i].it.album, &rows[i].it.album_artist };
        for(int c = 0; c < 5; c++) *f[c] = rows[i].off[c] == NOSTR ? NULL : pool + rows[i].off[c];
        v[i] = rows[i].it;
    }
    free(rows);
    out->v = v; out->n = n; out->pool = pool;
    return n;
}
static int read_query(const char *sql, const char *const *args, int nargs, ldb_list *out){
    memset(out, 0, sizeof *out);
    long long stamp[4];
    file_stamp(stamp);                                 /* before opening: a newer file read under an older stamp only reloads */
    sqlite3 *db = reader_open();
    if(!db) return -1;
    sqlite3_stmt *st = NULL;
    int n = -1;
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK){
        for(int i = 0; i < nargs; i++) sqlite3_bind_text(st, i + 1, args[i], -1, SQLITE_STATIC);
        n = read_list(st, out);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if(n < 0){ ldb_list_free(out); return -1; }
    memcpy(out->stamp, stamp, sizeof stamp);
    return n;
}
int ldb_read_songs(ldb_list *out){ return read_query(READ_SONGS, NULL, 0, out); }
int ldb_read_albums(ldb_list *out){ return read_query(READ_ALBUMS, NULL, 0, out); }
int ldb_read_album_songs(const char *album_artist, const char *album, ldb_list *out){
    if(!album_artist || !album){ memset(out, 0, sizeof *out); return -1; }
    const char *args[2] = { album_artist, album };
    return read_query(READ_ALBUM_SONGS, args, 2, out);
}
int ldb_read_artists(int by_album_artist, ldb_list *out){
    return read_query(by_album_artist ? READ_GROUP_ARTISTS : READ_ARTISTS, NULL, 0, out);
}
int ldb_read_genres(ldb_list *out){ return read_query(READ_GENRES, NULL, 0, out); }
/* a scope list: fmt with the scope's condition put in, args = the key, then any others */
static int scope_query(const char *fmt, int kind, const char *const *args, int nargs, ldb_list *out){
    char sql[2048];
    memset(out, 0, sizeof *out);
    for(int i = 0; i < nargs; i++) if(!args[i]) return -1;
    int w = snprintf(sql, sizeof sql, fmt, scope_cond(kind, args[0]));
    if(w < 0 || w >= (int)sizeof sql) return -1;
    return read_query(sql, args, nargs, out);
}
int ldb_read_scope_albums(int kind, const char *key, ldb_list *out){
    return scope_query(kind == LDB_SCOPE_GENRE ? READ_GENRE_ALBUMS : READ_SCOPE_ALBUMS, kind, &key, 1, out);
}
int ldb_read_scope_songs(int kind, const char *key, ldb_list *out){ return scope_query(READ_SCOPE_SONGS, kind, &key, 1, out); }
int ldb_read_scope_album_songs(int kind, const char *key, const char *album_artist, const char *album, ldb_list *out){
    const char *args[3] = { key, album_artist, album };
    return scope_query(READ_SCOPE_ALBUM_SONGS, kind, args, 3, out);
}
int ldb_scope_count(int kind, const char *key){
    char sql[512];
    if(!key || snprintf(sql, sizeof sql, READ_SCOPE_COUNT, scope_cond(kind, key)) >= (int)sizeof sql) return -1;
    sqlite3 *db = reader_open();
    if(!db) return -1;
    sqlite3_stmt *st = NULL;
    int n = -1;
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}
int ldb_album_paths(const char *album_artist, const char *album, char (*paths)[512], int max){
    if(!album_artist || !album || max <= 0) return 0;
    sqlite3 *db = reader_open();
    if(!db) return 0;
    sqlite3_stmt *st = NULL;
    int n = 0;
    if(sqlite3_prepare_v2(db, "SELECT t.path FROM tracks t WHERE t.album_artist=?1 AND t.album_title=?2 " ALBUM_ORDER ";",
                          -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, album_artist, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, album, -1, SQLITE_STATIC);
        while(n < max && sqlite3_step(st) == SQLITE_ROW){
            const char *p = (const char *)sqlite3_column_text(st, 0);
            if(!p || strlen(p) >= 512) continue;
            int seen = 0;
            for(int i = 0; i < n && !seen; i++) seen = !strcmp(paths[i], p);
            if(!seen) snprintf(paths[n++], 512, "%s", p);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}
int ldb_song_artist(const char *path, long long start_ms, int by_album_artist, char *out, int cap){
    if(!path || cap <= 0) return 0;
    out[0] = 0;
    sqlite3 *db = reader_open();
    if(!db) return 0;
    sqlite3_stmt *st = NULL;
    const char *sql = by_album_artist
        ? "SELECT " GROUP_KEY " FROM tracks t WHERE t.path=?1 AND (?2 < 0 OR t.start_ms=?2) ORDER BY t.start_ms LIMIT 1;"
        : "SELECT x.artist FROM tracks t JOIN track_artists x ON x.track_id=t.id AND x.position=0"
          " WHERE t.path=?1 AND (?2 < 0 OR t.start_ms=?2) ORDER BY t.start_ms LIMIT 1;";
    if(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, start_ms);
        if(sqlite3_step(st) == SQLITE_ROW){
            const char *a = (const char *)sqlite3_column_text(st, 0);
            snprintf(out, (size_t)cap, "%s", a ? a : "");
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out[0] != 0;
}
int ldb_song_album(const char *path, long long start_ms, char *album_artist, int aacap, char *album, int acap){
    if(!path || aacap <= 0 || acap <= 0) return 0;
    album_artist[0] = album[0] = 0;
    sqlite3 *db = reader_open();
    if(!db) return 0;
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if(sqlite3_prepare_v2(db, "SELECT album_artist, album_title FROM tracks WHERE path=?1 AND album_title IS NOT NULL"
                              " AND (?2 < 0 OR start_ms=?2) ORDER BY start_ms LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, start_ms);
        if(sqlite3_step(st) == SQLITE_ROW){
            const char *aa = (const char *)sqlite3_column_text(st, 0), *al = (const char *)sqlite3_column_text(st, 1);
            snprintf(album_artist, (size_t)aacap, "%s", aa ? aa : "");
            snprintf(album, (size_t)acap, "%s", al ? al : "");
            ok = 1;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return ok;
}
int ldb_track_start(const char *path, const char *title, long long *start_ms){
    *start_ms = 0;
    if(!path) return 0;
    sqlite3 *db = reader_open();
    if(!db) return 0;
    sqlite3_stmt *st = NULL;
    int found = 0;
    /* a plain file has one track; a CUE/ISO file's track is the one with this title, else its first */
    if(sqlite3_prepare_v2(db, "SELECT start_ms FROM tracks WHERE path=?1 ORDER BY (title IS ?2) DESC, start_ms LIMIT 1;",
                          -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, title ? title : "", -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW){ *start_ms = sqlite3_column_int64(st, 0); found = 1; }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return found;
}
int ldb_track_start_at(const char *path, int track, long long *start_ms){
    *start_ms = 0;
    if(!path) return 0;
    sqlite3 *db = reader_open();
    if(!db) return 0;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if(sqlite3_prepare_v2(db, "SELECT start_ms FROM tracks WHERE path=?1 AND track=?2 AND source<>0 LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, track);
        if(sqlite3_step(st) == SQLITE_ROW){ *start_ms = sqlite3_column_int64(st, 0); found = 1; }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return found;
}
/* A list of library.db's songs joined with the user file at file (attached by attach_sql): 0 rows when file doesn't
 * exist yet. arg is ?1, when sql has it. */
static int read_with(const char *file, const char *attach_sql, const char *sql, long long arg, ldb_list *out){
    memset(out, 0, sizeof *out);
    if(access(file, F_OK) != 0) return errno == ENOENT ? 0 : -1;
    long long stamp[4];
    file_stamp(stamp);
    sqlite3 *db = reader_open();
    if(!db) return -1;
    sqlite3_stmt *st = NULL;
    int n = -1;
    if(sqlite3_prepare_v2(db, attach_sql, -1, &st, NULL) == SQLITE_OK){   /* read-only, like the library */
        sqlite3_bind_text(st, 1, file, -1, SQLITE_STATIC);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        st = NULL;
        if(rc == SQLITE_DONE && sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK){
            if(sqlite3_bind_parameter_count(st) > 0) sqlite3_bind_int64(st, 1, arg);
            n = read_list(st, out);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if(n < 0){ ldb_list_free(out); return -1; }
    memcpy(out->stamp, stamp, sizeof stamp);
    return n;
}
int ldb_read_history(int by_recent, ldb_list *out){
    return read_with(LISTENINGDB_PATH, "ATTACH DATABASE ?1 AS ls;", READ_HISTORY[by_recent != 0], 0, out);
}
int ldb_read_favourites(ldb_list *out){
    return read_with(SHELVESDB_PATH, "ATTACH DATABASE ?1 AS sh;", READ_FAVOURITES, 0, out);
}
int ldb_read_playlist(long long id, ldb_list *out){
    return read_with(SHELVESDB_PATH, "ATTACH DATABASE ?1 AS sh;", READ_PLAYLIST, id, out);
}
int ldb_read_collection(long long id, ldb_list *out){
    return read_with(SHELVESDB_PATH, "ATTACH DATABASE ?1 AS sh;", READ_COLLECTION, id, out);
}
int ldb_read_uncollected(ldb_list *out){
    if(access(SHELVESDB_PATH, F_OK) != 0 && errno == ENOENT) return ldb_read_albums(out);   /* no shelves.db: no collections */
    return read_with(SHELVESDB_PATH, "ATTACH DATABASE ?1 AS sh;", READ_UNCOLLECTED, 0, out);
}

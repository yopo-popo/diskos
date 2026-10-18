/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdio.h"
#include "ipc.h"
#include <stdatomic.h>
/* First-run / rescan music scanner for diskOS.
 *
 * The stock V2.09 scanner (tag 0622) is a no-op - it never populates song.db from the SD.
 * So diskOS scans itself: walk the SD for audio files, read their tags, and rebuild the
 * SONG table of /usr/data/fiio/db/song.db (the same DB the library UI reads). Playlists,
 * favourites and resume state (CUSTOM_PLAYLIST/PLAYLIST_INFO/MY_LOVE/MEMORY_PLAY) are keyed
 * by PATH and left intact, so a rescan doesn't lose them.
 *
 * Tag support: see is_audio() - MP3 (ID3v2 + ID3v1), FLAC/OGG (Vorbis comments), M4A/M4B (MP4 atoms), APE (APEv2),
 * AIFF (ID3 chunk, NAME/AUTH), DSF (ID3 block), WMA (ASF), AAC (leading ID3v2); WAV/DFF/DTS by filename. Anything
 * without usable tags falls back to the filename (minus extension) as the title.
 * External .cue sheets and SACD .iso images are expanded into one row per track, the way stock V2.57 writes them (see
 * cue_finish, iso_finish).
 *
 * The same walk also rebuilds diskOS's own library.db (librarydb.c): every value of multi-valued tags, the fields only
 * it stores, the format, and no .nomedia folders. It is written beside song.db, never instead of it, and a library.db
 * failure never stops the SONG scan. Text that isn't Unicode is listed in a scan report on the SD card (report_write).
 *
 * Runs on a detached worker thread; progress is published under a mutex for the UI to poll.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <iconv.h>
#include <unistd.h>   /* usleep (used before the later include) */
#include <dirent.h>
#include <errno.h>
#include "audiodur.h"   /* exact song lengths for SONG.DURATION (header-authoritative only) */
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <sys/statvfs.h>
#include "sqlite3.h"
#include "scanner.h"
#include "musicdb.h"   /* DISKOS_AUDIOBOOKS gate: whether .m4b files are indexed */
#include "librarydb.h" /* diskOS's own library.db, written from the same walk */

#ifndef DB_PATH
#define DB_PATH   "/usr/data/fiio/db/song.db"
#endif
#ifndef SCAN_ROOT
#define SCAN_ROOT "/tmp/sdcard"
#endif
#define MAXPATH   1024
#define TAGLEN    256

/* ---- progress (published to the LVGL thread) ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_active;          /* a scan is running */
static int g_done;            /* files inserted so far */
static int g_total;           /* files found (0 until the walk completes) */
static int g_finished_seq;    /* bumped when a scan finishes (UI edge-detect) */
static int g_committed_pending; /* a scan committed since the UI last took it; a start never clears it (under g_mu) */
static int g_no_sd;           /* last scan aborted because the SD wasn't mounted */
static int g_commit_decided;  /* worker passed the commit decision: a cancel is no longer accepted (under g_mu) */
#define OUT_NONE SCAN_OUT_NONE
#define OUT_COMMITTED SCAN_OUT_COMMITTED
#define OUT_CANCELLED SCAN_OUT_CANCELLED
#define OUT_FAILED SCAN_OUT_FAILED
#define OUT_NO_SD SCAN_OUT_NO_SD
static int g_outcome;         /* the finished scan's one terminal outcome (under g_mu) */
static int g_cancelled;       /* the user (not an SD revoke) asked to stop; under g_mu
                               * so the completion poll can say "Scan cancelled" */
static _Atomic int g_abort;   /* set from the UI thread to stop a running walk (SD revoked); atomic, not volatile: two threads */
static int g_scan_err;        /* worker-thread only: any I/O or DB-insert failure during the walk
                               * -> the rebuild is incomplete and must NOT commit over the library */
static int g_skipped_result; /* completed scan snapshot under g_mu */
static int g_skipped;         /* files skipped this scan (unstat-able junk / overlong path) - logged, not fatal */
static int g_unsupported;     /* audio-ish files present but not indexable (AAC/M4A/OGG/...) - drives a UI notice */
static int g_prune_blocked;   /* an OVERLONG (unknowable) path was skipped -> its row can't be preserved in
                               * `seen`, so the vanished-row prune must not run this scan. ENOENT/unreadable
                               * audio files ARE preserved (added to seen), so they don't block the prune. */

int scanner_active(void){ pthread_mutex_lock(&g_mu); int a=g_active; pthread_mutex_unlock(&g_mu); return a; }
void scanner_progress(int *done, int *total){
    pthread_mutex_lock(&g_mu);
    if(done)*done=g_done; if(total)*total=g_total;
    pthread_mutex_unlock(&g_mu);
}
int scanner_take_finished(void){
    pthread_mutex_lock(&g_mu);
    static int last=0; int f = (g_finished_seq!=last); last=g_finished_seq;
    pthread_mutex_unlock(&g_mu);
    return f;
}
int scanner_take_committed(void){
    pthread_mutex_lock(&g_mu);
    int changed = g_committed_pending; g_committed_pending = 0;
    pthread_mutex_unlock(&g_mu);
    return changed;
}
/* 1 if the most recent scan did nothing because no SD was mounted (library was kept). */
int scanner_outcome(void){ pthread_mutex_lock(&g_mu); int v=g_outcome; pthread_mutex_unlock(&g_mu); return v; }
int scanner_no_sd(void){ pthread_mutex_lock(&g_mu); int v=g_no_sd; pthread_mutex_unlock(&g_mu); return v; }
int scanner_skipped(void){ pthread_mutex_lock(&g_mu); int v=g_skipped_result; pthread_mutex_unlock(&g_mu); return v; }
int scanner_unsupported(void){ pthread_mutex_lock(&g_mu); int v=g_unsupported; pthread_mutex_unlock(&g_mu); return v; }

/* ---- helpers ---- */
static void str_trim(char *s){
    char *p=s; while(*p==' '||*p=='\t') p++;
    if(p!=s) memmove(s,p,strlen(p)+1);
    int n=(int)strlen(s);
    while(n>0 && (unsigned char)s[n-1]<=' ') s[--n]=0;
}
/* NAME_CODE/TITLE_CODE/... : first 4 chars packed big-endian, 31-bit (matches the stock DB) */
static int code4(const char *s){
    char b[5]="\0\0\0\0"; int j=0;
    for(const char *p=s?s:""; *p && j<4; p++) b[j++]=(char)tolower((unsigned char)*p);
    unsigned v=0; for(int i=0;i<4;i++) v=(v<<8)|((unsigned char)b[i]);
    return (int)(v & 0x7fffffff);
}
static int has_ext(const char *name, const char *ext){
    size_t nl=strlen(name), el=strlen(ext);
    return nl>el && !strcasecmp(name+nl-el, ext);
}
/* Audio files diskOS indexes: stock V2.57's list plus Ogg Opus. Real tags from MP3 (ID3), FLAC and OGG (Vorbis comments),
 * M4A/M4B (MP4 atoms), APE (APEv2, ID3v1 fallback), AIFF (ID3 chunk, NAME/AUTH), DSF (ID3 block), WMA (ASF) and a
 * leading ID3v2 on AAC; WAV/DFF/DTS fall back to the filename. External .cue sheets become per-track rows after the
 * walk (cue_finish); SACD .iso images likewise become per-track rows (iso_finish). */
static int is_audio(const char *name){
    if(has_ext(name,".m4b")) return DISKOS_AUDIOBOOKS;   /* books indexed only when the audiobook feature is enabled */
    return has_ext(name,".mp3") || has_ext(name,".flac") || has_ext(name,".wav")
        || has_ext(name,".m4a")
        /* the rest of stock V2.57's scanner list (P257 extension table @0x6cb6ec..): the stock player decodes them */
        || has_ext(name,".aac") || has_ext(name,".ogg") || has_ext(name,".ape") || has_ext(name,".aif")
        || has_ext(name,".aiff")|| has_ext(name,".wma") || has_ext(name,".dsf") || has_ext(name,".dff")
        || has_ext(name,".dts") || has_ext(name,".opus") || has_ext(name,".oga");
}
/* Audio-ish files we do NOT index yet (no parser). Counted during the walk so the UI can tell a user
 * whose library is all AAC/ALAC/etc WHY it looks empty, instead of a bare "No music found". */
static int is_unsupported_audio(const char *name){
    /* ALAC extension and WavPack are not indexed. */
    return has_ext(name,".alac")
        || has_ext(name,".wv");
}

/* ---- text encoding -> UTF-8 (bounded) ----
 * Text is decoded only as its format declares it: UTF-8 and UTF-16 as Unicode, Latin-1 where the format says Latin-1
 * (ID3 encoding 0, ID3v1). Old code pages are never guessed. Broken UTF-8 or UTF-16 becomes U+FFFD, and output is cut
 * only on a character boundary. Each decoder says what it met (ENC_*), so the scan report can list files whose text
 * isn't Unicode for their owner to re-save on a computer. */
enum { ENC_OK = 0, ENC_LATIN1 = 1, ENC_BROKEN = 2 };
/* one code point; once one doesn't fit, nothing after it is written either (no holes in a cut value) */
static void put_u8(char **o, char *end, unsigned cp){
    char *p = *o;
    int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if(p + n > end){ *o = end; return; }
    if(n == 1) *p++ = (char)cp;
    else if(n == 2){ *p++ = (char)(0xC0|(cp>>6)); *p++ = (char)(0x80|(cp&0x3F)); }
    else if(n == 3){ *p++ = (char)(0xE0|(cp>>12)); *p++ = (char)(0x80|((cp>>6)&0x3F)); *p++ = (char)(0x80|(cp&0x3F)); }
    else { *p++ = (char)(0xF0|(cp>>18)); *p++ = (char)(0x80|((cp>>12)&0x3F)); *p++ = (char)(0x80|((cp>>6)&0x3F)); *p++ = (char)(0x80|(cp&0x3F)); }
    *o = p;
}
/* UTF-8 from in[0..n) or to a NUL: well-formed sequences copied, every broken byte becomes U+FFFD; trimmed */
static int u8_copy(const unsigned char *in, size_t n, char *out, int cap){
    char *o = out, *end = out + cap - 1;
    int e = ENC_OK;
    for(size_t i = 0; i < n && in[i] && o < end; ){        /* stops once full: what is cut off isn't judged */
        unsigned c = in[i];
        if(c < 0x80){ put_u8(&o, end, c); i++; continue; }
        int k = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
        int ok = k > 0 && i + (size_t)k < n;
        unsigned cp = 0;
        if(ok){
            cp = c & (0x3Fu >> k);
            for(int j = 1; j <= k; j++){ if((in[i + j] & 0xC0) != 0x80){ ok = 0; break; } cp = (cp << 6) | (in[i + j] & 0x3F); }
        }
        if(ok && ((k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF)))) ok = 0;
        if(!ok){ put_u8(&o, end, 0xFFFD); e = ENC_BROKEN; i++; continue; }
        put_u8(&o, end, cp);
        i += (size_t)k + 1;
    }
    *o = 0; str_trim(out);
    return e;
}
static int latin1_copy(const unsigned char *in, size_t n, char *out, int cap){
    char *o = out, *end = out + cap - 1;
    int e = ENC_OK;
    for(size_t i = 0; i < n && in[i] && o < end; i++){ if(in[i] >= 0x80) e = ENC_LATIN1; put_u8(&o, end, in[i]); }
    *o = 0; str_trim(out);
    return e;
}
/* A string that snprintf (or a copy loop) cut may end in part of a character: drop that part. */
static void u8_fix_tail(char *s){
    size_t n = strlen(s), i = n;
    for(int k = 0; k < 3 && i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80; k++) i--;
    if(i == 0) return;
    unsigned char c = (unsigned char)s[i - 1];
    size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if(n - (i - 1) < need) s[i - 1] = 0;
}
/* UTF-16 from in[0..n) to a 0000 unit; *used = bytes taken, the terminator included */
static int utf16_copy(const unsigned char *in, size_t n, int be, char *out, int cap, size_t *used){
    char *o = out, *end = out + cap - 1;
    int e = ENC_OK;
    size_t i = 0, u = n;
    for(; i + 1 < n; i += 2){
        unsigned c = be ? (unsigned)(in[i] << 8 | in[i + 1]) : (unsigned)(in[i + 1] << 8 | in[i]);
        if(c == 0){ u = i + 2; break; }
        if(c >= 0xD800 && c <= 0xDBFF && i + 3 < n){
            unsigned lo = be ? (unsigned)(in[i + 2] << 8 | in[i + 3]) : (unsigned)(in[i + 3] << 8 | in[i + 2]);
            if(lo >= 0xDC00 && lo <= 0xDFFF){ put_u8(&o, end, 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00)); i += 2; continue; }
        }
        if(o >= end) continue;                                                                  /* full: just find the end */
        if(c >= 0xD800 && c <= 0xDFFF){ put_u8(&o, end, 0xFFFD); e = ENC_BROKEN; continue; }   /* lone surrogate */
        if(c == 0xFEFF && o == out) continue;                                                   /* a repeated BOM */
        put_u8(&o, end, c);
    }
    if(u == n && (n & 1) && o < end) e = ENC_BROKEN;      /* an odd last byte: half a character */
    if(used) *used = u;
    *o = 0; str_trim(out);
    return e;
}
/* One ID3 string of encoding enc (0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8) from in[0..n); *used = bytes
 * taken with its terminator. A UTF-16 string without its BOM keeps the byte order *be already has (little-endian
 * unless it plainly starts big-endian), instead of losing its first letter. */
static int id3_str(int enc, const unsigned char *in, size_t n, char *out, int cap, size_t *used, int *be){
    out[0] = 0;
    if(enc == 0 || enc == 3){
        size_t len = 0; while(len < n && in[len]) len++;
        *used = len < n ? len + 1 : len;
        return enc == 0 ? latin1_copy(in, len, out, cap) : u8_copy(in, len, out, cap);
    }
    size_t off = 0, u = 0;
    if(enc == 1){
        if(n >= 2 && in[0] == 0xFF && in[1] == 0xFE){ *be = 0; off = 2; }
        else if(n >= 2 && in[0] == 0xFE && in[1] == 0xFF){ *be = 1; off = 2; }
        else if(n >= 2 && in[0] == 0 && in[1] != 0){        /* big-endian only if it plainly is (every high byte 00) */
            int all = 1;
            for(size_t i = 0; i + 1 < n && (in[i] || in[i + 1]); i += 2) if(in[i]){ all = 0; break; }
            if(all) *be = 1;                                  /* else the order it already has (little-endian) */
        }
    } else *be = 1;
    int e = utf16_copy(in + off, n - off, *be, out, cap, &u);
    *used = off + u;
    return e;
}
static const char *const ID3_GENRES[192] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age",
    "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
    "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alt. Rock",
    "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy",
    "Cult", "Gangsta Rap", "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro",
    "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast-Fusion", "Bebop",
    "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock",
    "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson",
    "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam",
    "Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock",
    "Drum Solo", "A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
    "Indie", "BritPop", "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal",
    "Crossover", "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop",
    "Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub",
    "EBM", "Eclectic", "Electro", "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient",
    "Industro-Goth", "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz",
    "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music", "Neoclassical",
    "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep", "Garage Rock",
    "Psybient",
};
static int utf8_ok(const unsigned char *s, size_t n);

/* unsigned shifts: p[0]<<24 with the byte >=128 would shift into the int sign bit (signed-overflow UB). */
static uint32_t be32(const unsigned char *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3]; }
static uint32_t synch32(const unsigned char *p){ return ((uint32_t)p[0]<<21)|((uint32_t)p[1]<<14)|((uint32_t)p[2]<<7)|(uint32_t)p[3]; }
static uint32_t le32(const unsigned char *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* leading positive integer of an ID3/Vorbis track/disc value ("5" or "5/12" -> 5, "01" -> 1). Returns 0
 * for junk, overflow, or a non-positive value, so a bogus tag can never store garbage or clobber a good
 * existing value (the UPDATE keeps the old value when this is 0). */
static int tag_int(const char *s){
    if(!s) return 0;
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if(end == s || errno == ERANGE || v <= 0 || v > INT_MAX) return 0;
    return (int)v;
}

/* ---- what the tag readers collect ----
 * One scan_meta per file. SONG (for mq_player) takes the first value of a field, as before; library.db takes every
 * value (artists, genres) and the fields only it stores. Allocated once per scan: it is too big for the scan
 * thread's stack. */
enum { MF_NONE, MF_TITLE, MF_ARTIST, MF_ARTISTS, MF_ALBUM, MF_ALBUMARTIST, MF_GENRE, MF_TRACK, MF_TRACKTOTAL, MF_DISC,
       MF_DISCTOTAL, MF_DATE, MF_YEAR, MF_RELEASETYPE, MF_COMPILATION, MF_RG_TG, MF_RG_TP, MF_RG_AG, MF_RG_AP,
       MF_R128_TG, MF_R128_AG, MF_BPM, MF_KEY, MF_LYRICS, MF_SYNCLYRICS, MF_SORT_AA, MF_SORT_A, MF_SORT_AL, MF_SORT_T,
       MF_CUSTOM, MF_GNRE };
#define MAXV LDB_MAXV
typedef struct {
    char title[TAGLEN], album[TAGLEN], album_artist[TAGLEN];
    char artists[MAXV][TAGLEN]; int n_artists;            /* every ARTIST value, as tagged */
    char artists_pic[MAXV][TAGLEN]; int n_artists_pic;    /* Picard's ARTISTS: used instead of ARTIST when present */
    char artist_sorts[MAXV][TAGLEN]; int n_artist_sorts;
    char genres[MAXV][TAGLEN]; int n_genres;
    char release_type[TAGLEN], title_sort[TAGLEN], album_sort[TAGLEN], album_artist_sort[TAGLEN];
    char initial_key[32], codec[12];
    int track, disc, track_total, disc_total, year_date, year_year, gnre;
    int compilation, has_lyrics, rg_flags, r128_flags;
    double rg_tg, rg_tp, rg_ag, rg_ap, r128_tg, r128_ag, bpm;
    int sample_rate, bits, channels;
    int title_tag;                                        /* a title tag was read (SONG.TITLE_TAG) */
    int n_enc; struct { char tag[32]; int kind; char value[72]; } enc[3];   /* for the scan report */
} scan_meta;
static scan_meta *g_meta, *g_meta_save;                   /* the scan thread's (see scan_thread) */

/* A reader that streams its tag saves the fields first and puts them back if the read fails part way, so a failed
 * read leaves them untouched, as the old whole-tag read did. */
static void meta_save(const scan_meta *m){ if(g_meta_save && m != g_meta_save) memcpy(g_meta_save, m, sizeof *m); }
static void meta_restore(scan_meta *m){ if(g_meta_save && m != g_meta_save) memcpy(m, g_meta_save, sizeof *m); }

static void enc_note(scan_meta *m, const char *tag, int kind, const char *value){
    if(!kind || m->n_enc >= 3) return;
    for(int i = 0; i < m->n_enc; i++) if(!strcmp(m->enc[i].tag, tag)) return;
    snprintf(m->enc[m->n_enc].tag, sizeof m->enc[0].tag, "%s", tag);
    m->enc[m->n_enc].kind = kind;
    char *o = m->enc[m->n_enc].value, *end = o + sizeof m->enc[0].value - 1;   /* a short, whole-character excerpt */
    u8_copy((const unsigned char *)value, strlen(value), o, (int)(end - o + 1));
    m->n_enc++;
}
static void add_value(char (*list)[TAGLEN], int *n, const char *v){
    if(!v[0] || *n >= MAXV) return;
    for(int i = 0; i < *n; i++) if(!strcmp(list[i], v)) return;
    snprintf(list[(*n)++], TAGLEN, "%s", v);
}
/* "1999", "1999-05-03", "03/05/1999": the first four digits in a row */
static int year4(const char *s){
    for(const char *p = s; *p; p++){
        if(p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9'){
            int y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
            return y > 0 ? y : 0;
        }
    }
    return 0;
}
/* a lyrics text with "[mm:ss" time stamps is synced */
static int lyrics_synced(const char *s){
    for(const char *p = strchr(s, '['); p; p = strchr(p + 1, '[')){
        const char *q = p + 1; int d = 0;
        while(*q >= '0' && *q <= '9' && d < 4){ q++; d++; }
        if(d && q[0] == ':' && q[1] >= '0' && q[1] <= '5' && q[2] >= '0' && q[2] <= '9') return 1;
    }
    return 0;
}
static int parse_real(const char *s, double lo, double hi, double *out){
    char *end;
    double v = strtod(s, &end);
    if(end == s || !(v >= lo && v <= hi)) return 0;
    *out = v;
    return 1;
}
static int truthy(const char *v){ return v[0] == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "yes"); }
static void take_num_pair(const char *v, int *num, int *total){
    if(!*num) *num = tag_int(v);
    const char *sl = strchr(v, '/');
    if(sl && !*total) *total = tag_int(sl + 1);
}
/* One decoded value of field f. Single-valued fields keep the first value read; multi-valued ones collect. */
static void meta_take(scan_meta *m, int f, const char *v){
    if(f == MF_LYRICS){ int s = lyrics_synced(v) ? 2 : 1; if(s > m->has_lyrics) m->has_lyrics = s; return; }
    if(f == MF_SYNCLYRICS){ m->has_lyrics = 2; return; }
    if(!v[0]) return;
    switch(f){
    case MF_TITLE:       if(!m->title[0]) snprintf(m->title, TAGLEN, "%s", v); break;
    case MF_ARTIST:      add_value(m->artists, &m->n_artists, v); break;
    case MF_ARTISTS:     add_value(m->artists_pic, &m->n_artists_pic, v); break;
    case MF_ALBUM:       if(!m->album[0]) snprintf(m->album, TAGLEN, "%s", v); break;
    case MF_ALBUMARTIST: if(!m->album_artist[0]) snprintf(m->album_artist, TAGLEN, "%s", v); break;
    case MF_GENRE:       add_value(m->genres, &m->n_genres, v); break;
    case MF_TRACK:       take_num_pair(v, &m->track, &m->track_total); break;
    case MF_DISC:        take_num_pair(v, &m->disc, &m->disc_total); break;
    case MF_TRACKTOTAL:  if(!m->track_total) m->track_total = tag_int(v); break;
    case MF_DISCTOTAL:   if(!m->disc_total) m->disc_total = tag_int(v); break;
    case MF_DATE:        if(!m->year_date) m->year_date = year4(v); break;
    case MF_YEAR:        if(!m->year_year) m->year_year = year4(v); break;
    case MF_RELEASETYPE:                                  /* several values ("album", "live") -> "album; live" */
        if(!strstr(m->release_type, v)){
            size_t l = strlen(m->release_type);
            if(l + strlen(v) + 3 < TAGLEN) snprintf(m->release_type + l, TAGLEN - l, "%s%s", l ? "; " : "", v);
        }
        break;
    case MF_COMPILATION: if(truthy(v)) m->compilation = 1; break;
    case MF_RG_TG: if(!(m->rg_flags & LDB_RG_TG) && parse_real(v, -64, 64, &m->rg_tg)) m->rg_flags |= LDB_RG_TG; break;
    case MF_RG_TP: if(!(m->rg_flags & LDB_RG_TP) && parse_real(v, 0, 16, &m->rg_tp)) m->rg_flags |= LDB_RG_TP; break;
    case MF_RG_AG: if(!(m->rg_flags & LDB_RG_AG) && parse_real(v, -64, 64, &m->rg_ag)) m->rg_flags |= LDB_RG_AG; break;
    case MF_RG_AP: if(!(m->rg_flags & LDB_RG_AP) && parse_real(v, 0, 16, &m->rg_ap)) m->rg_flags |= LDB_RG_AP; break;
    case MF_R128_TG: if(!(m->r128_flags & LDB_RG_TG) && parse_real(v, -32768, 32767, &m->r128_tg)) m->r128_flags |= LDB_RG_TG; break;
    case MF_R128_AG: if(!(m->r128_flags & LDB_RG_AG) && parse_real(v, -32768, 32767, &m->r128_ag)) m->r128_flags |= LDB_RG_AG; break;
    case MF_BPM:   if(m->bpm <= 0) parse_real(v, 0.01, 999, &m->bpm); break;
    case MF_KEY:   if(!m->initial_key[0]) u8_copy((const unsigned char *)v, strlen(v), m->initial_key, sizeof m->initial_key); break;
    case MF_SORT_AA: if(!m->album_artist_sort[0]) snprintf(m->album_artist_sort, TAGLEN, "%s", v); break;
    case MF_SORT_A:  add_value(m->artist_sorts, &m->n_artist_sorts, v); break;
    case MF_SORT_AL: if(!m->album_sort[0]) snprintf(m->album_sort, TAGLEN, "%s", v); break;
    case MF_SORT_T:  if(!m->title_sort[0]) snprintf(m->title_sort, TAGLEN, "%s", v); break;
    }
}
/* Tag names in Vorbis comments, APEv2, WMA attributes, ID3 TXXX descriptions and MP4 "----" items (any case) */
static const struct { const char *k; unsigned char f; } TAG_NAMES[] = {
    {"TITLE",MF_TITLE}, {"ARTIST",MF_ARTIST}, {"ARTISTS",MF_ARTISTS}, {"ALBUM",MF_ALBUM}, {"ALBUMARTIST",MF_ALBUMARTIST},
    {"ALBUM ARTIST",MF_ALBUMARTIST}, {"GENRE",MF_GENRE}, {"TRACKNUMBER",MF_TRACK}, {"TRACK",MF_TRACK},
    {"TRACKTOTAL",MF_TRACKTOTAL}, {"TOTALTRACKS",MF_TRACKTOTAL}, {"DISCNUMBER",MF_DISC}, {"DISC",MF_DISC},
    {"DISCTOTAL",MF_DISCTOTAL}, {"TOTALDISCS",MF_DISCTOTAL}, {"DATE",MF_DATE}, {"YEAR",MF_YEAR},
    {"RELEASETYPE",MF_RELEASETYPE}, {"MUSICBRAINZ_ALBUMTYPE",MF_RELEASETYPE}, {"MusicBrainz Album Type",MF_RELEASETYPE},
    {"COMPILATION",MF_COMPILATION}, {"REPLAYGAIN_TRACK_GAIN",MF_RG_TG}, {"REPLAYGAIN_TRACK_PEAK",MF_RG_TP},
    {"REPLAYGAIN_ALBUM_GAIN",MF_RG_AG}, {"REPLAYGAIN_ALBUM_PEAK",MF_RG_AP}, {"R128_TRACK_GAIN",MF_R128_TG},
    {"R128_ALBUM_GAIN",MF_R128_AG}, {"BPM",MF_BPM}, {"INITIALKEY",MF_KEY}, {"KEY",MF_KEY}, {"LYRICS",MF_LYRICS},
    {"UNSYNCEDLYRICS",MF_LYRICS}, {"UNSYNCED LYRICS",MF_LYRICS}, {"SYNCEDLYRICS",MF_SYNCLYRICS},
    {"ALBUMARTISTSORT",MF_SORT_AA}, {"ARTISTSORT",MF_SORT_A}, {"ALBUMSORT",MF_SORT_AL}, {"TITLESORT",MF_SORT_T},
    {"WM/AlbumTitle",MF_ALBUM}, {"WM/AlbumArtist",MF_ALBUMARTIST}, {"WM/Genre",MF_GENRE}, {"WM/TrackNumber",MF_TRACK},
    {"WM/PartOfSet",MF_DISC}, {"WM/Year",MF_YEAR}, {"WM/IsCompilation",MF_COMPILATION}, {"WM/BeatsPerMinute",MF_BPM},
    {"WM/InitialKey",MF_KEY}, {"WM/AlbumArtistSortOrder",MF_SORT_AA}, {"WM/ArtistSortOrder",MF_SORT_A},
    {"WM/AlbumSortOrder",MF_SORT_AL}, {"WM/TitleSortOrder",MF_SORT_T}, {"WM/Lyrics",MF_LYRICS},
    {"WM/Lyrics_Synchronised",MF_SYNCLYRICS}, {"WM/ARTISTS",MF_ARTISTS}, {"MusicBrainz/Album Type",MF_RELEASETYPE},
};
static int tag_field(const char *k){
    for(unsigned i = 0; i < sizeof TAG_NAMES / sizeof TAG_NAMES[0]; i++) if(!strcasecmp(k, TAG_NAMES[i].k)) return TAG_NAMES[i].f;
    return MF_NONE;
}
/* ID3 TXXX and MP4 "----" carry only what their formats have no own frame or atom for */
static int custom_field(const char *k){
    int f = tag_field(k);
    switch(f){
    case MF_ARTISTS: case MF_RELEASETYPE: case MF_RG_TG: case MF_RG_TP: case MF_RG_AG: case MF_RG_AP: case MF_SORT_AA:
    case MF_TRACKTOTAL: case MF_DISCTOTAL: case MF_KEY: return f;
    }
    return MF_NONE;
}
/* an ID3 genre number: 1 to 3 digits and nothing else, below 192; else -1 */
static int genre_num(const char *p, size_t len){
    if(len < 1 || len > 3) return -1;
    for(size_t i = 0; i < len; i++) if(p[i] < '0' || p[i] > '9') return -1;
    int n = atoi(p);
    return n < 192 ? n : -1;
}
/* "(13)" or "13" style ID3 numeric genre -> its name; "(13)Pop" keeps the text (a refinement). Also used, without
 * the bare-number form, for APE/WMA values. Only number groups (and RX/CR) are read: "(Live)" stays as written. */
static void genre_take(scan_meta *m, const char *v, int numeric){
    const char *p = v;
    if(numeric){
        int n = genre_num(p, strlen(p));
        if(n >= 0){ meta_take(m, MF_GENRE, ID3_GENRES[n]); return; }
    }
    const char *names[8]; int nn = 0;
    while(*p == '(' && p[1] != '('){                       /* "((" starts literal text */
        const char *e = strchr(p, ')');
        if(!e) break;
        int g = genre_num(p + 1, (size_t)(e - p - 1));
        const char *nm = g >= 0 ? ID3_GENRES[g] : (e - p == 3 && !strncmp(p + 1, "RX", 2)) ? "Remix"
                       : (e - p == 3 && !strncmp(p + 1, "CR", 2)) ? "Cover" : NULL;
        if(!nm) break;                                     /* not a number: text from here on */
        if(nn < 8) names[nn++] = nm;
        p = e + 1;
    }
    if(*p == '(' && p[1] == '(') p++;
    char rest[TAGLEN]; snprintf(rest, sizeof rest, "%s", p); str_trim(rest);
    if(rest[0]) meta_take(m, MF_GENRE, rest);
    else for(int i = 0; i < nn; i++) meta_take(m, MF_GENRE, names[i]);
}

/* control characters or U+FFFD: what is left in a frame after its text by a tagger that didn't clear it */
static int junk_text(const char *v){
    for(const unsigned char *p = (const unsigned char *)v; *p; p++){
        if(*p < 0x20 || *p == 0x7F) return 1;
        if(p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) return 1;            /* U+0080..009F */
        if(p[0] == 0xEF && p[1] == 0xBF && p[2] == 0xBD) return 1;            /* U+FFFD */
    }
    return 0;
}
/* Move a forward tag walk from file offset *at to `to`. A short gap is read through: a seek drops stdio's buffer, so
 * the next read would fetch the same block again. A long one (artwork) is seeked past, unread. *at < 0 = unknown.
 * 0 = done, -1 = short read, -2 = seek error (ferror won't catch it). */
static int skip_to(FILE *f, off_t *at, off_t to){
    if(*at < 0 || to < *at || to - *at > 4096){
        if(fseeko(f, to, SEEK_SET) != 0) return -2;
    } else while(*at < to){
        unsigned char junk[1024];
        size_t n = to - *at > (off_t)sizeof junk ? sizeof junk : (size_t)(to - *at);
        if(fread(junk,1,n,f) != n) return -1;
        *at += (off_t)n;
    }
    *at = to;
    return 0;
}

/* ---- ID3v2 ----
 * The frames read; every other frame body (APIC artwork above all) is never read. v2.2 ids beside v2.3/v2.4 ones. */
static const struct { char id[5]; unsigned char f; } ID3_FRAMES[] = {
    {"TIT2",MF_TITLE}, {"TT2",MF_TITLE}, {"TPE1",MF_ARTIST}, {"TP1",MF_ARTIST}, {"TALB",MF_ALBUM}, {"TAL",MF_ALBUM},
    {"TCON",MF_GENRE}, {"TCO",MF_GENRE}, {"TRCK",MF_TRACK}, {"TRK",MF_TRACK}, {"TPOS",MF_DISC}, {"TPA",MF_DISC},
    {"TPE2",MF_ALBUMARTIST}, {"TP2",MF_ALBUMARTIST}, {"TDRC",MF_DATE}, {"TYER",MF_YEAR}, {"TYE",MF_YEAR},
    {"TCMP",MF_COMPILATION}, {"TCP",MF_COMPILATION}, {"TBPM",MF_BPM}, {"TBP",MF_BPM}, {"TKEY",MF_KEY}, {"TKE",MF_KEY},
    {"TSO2",MF_SORT_AA}, {"TS2",MF_SORT_AA}, {"TSOP",MF_SORT_A}, {"TSP",MF_SORT_A}, {"TSOA",MF_SORT_AL},
    {"TSA",MF_SORT_AL}, {"TSOT",MF_SORT_T}, {"TST",MF_SORT_T}, {"TXXX",MF_CUSTOM}, {"TXX",MF_CUSTOM},
    {"USLT",MF_LYRICS}, {"ULT",MF_LYRICS}, {"SYLT",MF_SYNCLYRICS}, {"SLT",MF_SYNCLYRICS},
};
static int id3_frame_field(const char *id){
    for(unsigned i = 0; i < sizeof ID3_FRAMES / sizeof ID3_FRAMES[0]; i++) if(!strcmp(id, ID3_FRAMES[i].id)) return ID3_FRAMES[i].f;
    return MF_NONE;
}
#define ID3_BODY_MAX (16 * 1024)   /* the part of a frame body read: values past it are never used */
/* undo unsynchronisation (FF 00 -> FF) in place; returns the new length */
static size_t id3_unsync(unsigned char *b, size_t n){
    size_t o = 0;
    for(size_t i = 0; i < n; i++){ b[o++] = b[i]; if(b[i] == 0xFF && i + 1 < n && b[i + 1] == 0x00) i++; }
    return o;
}
/* One frame body: its values, NUL-separated (ID3v2.4, and v2.3 as Mp3tag writes it), each with its own BOM in UTF-16.
 * TXXX: a description, then the values. USLT: a language, a description, then the text. */
static void id3_frame(scan_meta *m, const char *id, int f, const unsigned char *b, size_t n){
    if(n < 1) return;
    int enc = b[0], be = 0;
    if(enc > 3) return;
    size_t p = 1, u;
    char tag[32], v[TAGLEN];
    snprintf(tag, sizeof tag, "%s", id);
    if(f == MF_CUSTOM){
        char d[64];
        int e = id3_str(enc, b + p, n - p, d, sizeof d, &u, &be);
        p += u;
        f = custom_field(d);
        if(f == MF_NONE) return;
        snprintf(tag, sizeof tag, "%s:%.20s", id, d);
        if(e) enc_note(m, tag, e, d);
    } else if(f == MF_LYRICS){
        if(n - p < 3) return;
        p += 3;
        id3_str(enc, b + p, n - p, v, 8, &u, &be);        /* content descriptor */
        p += u;
        int e = id3_str(enc, b + p, n - p, v, sizeof v, &u, &be);
        if(e) enc_note(m, tag, e, v);
        meta_take(m, MF_LYRICS, v);
        return;
    }
    for(int nv = 0; p < n && nv < MAXV; nv++){
        int e = id3_str(enc, b + p, n - p, v, sizeof v, &u, &be);
        if(!u) break;
        p += u;
        if(nv && (!v[0] || junk_text(v))) break;          /* old bytes after the terminator, not another value */
        if(e) enc_note(m, tag, e, v);
        if(f == MF_GENRE) genre_take(m, v, 1);
        else meta_take(m, f, v);
    }
}
/* Walk the frames of a tag whose body starts at the stream's position `base` (tagsize bytes). Only the bodies of the
 * frames above are read; the rest are seeked past. unsync_all: every frame is unsynchronised (v2.4 header flag).
 * 1 = walked (or stopped at padding or a bad frame), 0 = a read failed. */
static int id3_frames(FILE *f, scan_meta *m, int ver, off_t base, long tagsize, int unsync_all, int exthdr, int *rerr){
    int ok = 1;
    long p = 0;
    off_t at = base;                              /* the stream position */
    if(exthdr && ver>=3 && tagsize>=6){           /* skip the extended header if present */
        unsigned char e[4] = {0};
        if(fread(e,1,4,f)!=4) ok=0;
        at += 4;
        if(ver>=4){ long es=synch32(e);      if(es>0 && es<=tagsize) p+=es; }        /* v2.4: size incl itself */
        else      { long es=be32(e);         if(es>0 && es<=tagsize-4) p+=4+es; }    /* v2.3: excl the 4 size bytes */
    }
    int idlen = (ver==2)?3:4, fhdr=(ver==2)?6:10;
    while(ok && p + fhdr <= tagsize){
        unsigned char fh[10];
        int sk = skip_to(f, &at, base + p);
        if(sk){ if(sk == -2 && rerr) *rerr=1; ok=0; break; }
        if(fread(fh,1,(size_t)fhdr,f) != (size_t)fhdr){ ok=0; break; }
        at += fhdr;
        char id[5]={0}; memcpy(id, fh, idlen);
        if(id[0]==0) break;                       /* padding */
        long fsize;
        if(ver==2) fsize = (fh[3]<<16)|(fh[4]<<8)|fh[5];
        else if(ver==4) fsize = synch32(fh+4);
        else fsize = be32(fh+4);
        if(fsize<=0 || fsize > tagsize - p - fhdr) break;   /* overflow-safe (tagsize-p-fhdr >= 0) */
        int fld = id3_frame_field(id);
        /* frame format flags: compressed or encrypted bodies are skipped; a group id byte and (v2.4) a data length
         * indicator come before the data, which is then unsynchronised when the frame says so */
        int fl = ver >= 3 ? fh[9] : 0, skip = 0, pre = 0, fun = unsync_all;
        if(ver == 3){ if(fl & 0xC0) skip = 1; if(fl & 0x20) pre = 1; }
        else if(ver == 4){ if(fl & 0x0C) skip = 1; if(fl & 0x40) pre += 1; if(fl & 0x01) pre += 4; if(fl & 0x02) fun = 1; }
        if(fld == MF_SYNCLYRICS){ if(!skip) meta_take(m, MF_SYNCLYRICS, ""); }
        else if(fld != MF_NONE && !skip){
            size_t want = fsize > ID3_BODY_MAX ? ID3_BODY_MAX : (size_t)fsize;
            unsigned char *body = malloc(want);
            if(!body){ if(rerr)*rerr=1; ok=0; break; }                             /* OOM: a real read failure (ferror won't catch it) */
            if(fread(body,1,want,f) != want){ free(body); ok=0; break; }
            at += (off_t)want;
            if(want > (size_t)pre){
                size_t dn = want - (size_t)pre;
                if(fun) dn = id3_unsync(body + pre, dn);
                id3_frame(m, id, fld, body + pre, dn);
            }
            free(body);
        }
        p += fhdr + fsize;
    }
    return ok;
}
/* Read an ID3v2 tag at the stream's position. Returns 1 if the header was present. Fields it doesn't find are left
 * untouched. A tag that runs past the end of the file is ignored whole (the whole-tag read it replaces got a short
 * read there). A v2.2/v2.3 tag unsynchronised as a whole is undone in memory first (up to 4 MiB; a bigger one is
 * walked as it is). */
static int id3v2_read(FILE *f, scan_meta *m, int *rerr){
    unsigned char h[10];
    if(fread(h,1,10,f)!=10) return 0;
    if(memcmp(h,"ID3",3)!=0) return 0;
    int ver=h[3];
    if(ver < 2 || ver > 4) return 1;                                          /* an unknown major version */
    int unsync=(h[5]&0x80)!=0, exthdr=(h[5]&0x40)!=0;
    if(ver == 2 && exthdr) return 1;                                          /* v2.2: that bit means compressed */
    long tagsize = synch32(h+6);
    if(tagsize<=0 || tagsize>20*1024*1024) return 1;
    off_t base = ftello(f);
    struct stat st;
    if(base < 0 || fstat(fileno(f), &st) != 0){ if(rerr)*rerr=1; return 1; }
    if(st.st_size - base < tagsize) return 1;                                  /* truncated tag: nothing read */
    meta_save(m);
    int err0 = ferror(f), ok;
    if(unsync && ver < 4 && tagsize <= 4L*1024*1024){
        unsigned char *buf = malloc((size_t)tagsize);
        if(!buf){ if(rerr)*rerr=1; return 1; }
        ok = fread(buf,1,(size_t)tagsize,f) == (size_t)tagsize;
        size_t len = ok ? id3_unsync(buf, (size_t)tagsize) : 0;
        FILE *mf = ok ? fmemopen(buf, len, "rb") : NULL;
        if(ok && !mf){ if(rerr)*rerr=1; ok = 0; }
        if(mf){ ok = id3_frames(mf, m, ver, 0, (long)len, 0, exthdr, rerr); fclose(mf); }
        free(buf);
    } else ok = id3_frames(f, m, ver, base, tagsize, ver == 4 && unsync, exthdr, rerr);
    if(!ok || (!err0 && ferror(f))) meta_restore(m);
    return 1;
}
/* ID3v1: last 128 bytes "TAG" + 30+30+30 title/artist/album, year, v1.1 track (Latin-1). Fallback only: it fills
 * what the ID3v2 tag didn't have. Its genre byte isn't used: many encoders write 0 (Blues) when there is none. */
static void id3v1_read(FILE *f, scan_meta *m, int *rerr){
    /* fseek does NOT set the stream error indicator, so ferror() won't catch a seek IO failure.
     * A file smaller than 128 bytes fails this seek with EINVAL (benign -> just no ID3v1); only a
     * real media error (EIO) should preserve the existing row. */
    if(fseek(f,-128,SEEK_END)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
    unsigned char t[128];
    if(fread(t,1,128,f)!=128) return;
    if(memcmp(t,"TAG",3)!=0) return;
    static const struct { int off, f; const char *name; } F[] = { {3, MF_TITLE, "ID3v1 title"}, {33, MF_ARTIST, "ID3v1 artist"}, {63, MF_ALBUM, "ID3v1 album"} };
    for(unsigned i = 0; i < 3; i++){
        if((F[i].f == MF_TITLE && m->title[0]) || (F[i].f == MF_ARTIST && m->n_artists) || (F[i].f == MF_ALBUM && m->album[0])) continue;
        char tmp[96];
        int e = latin1_copy(t + F[i].off, 30, tmp, sizeof tmp);
        if(!tmp[0]) continue;
        if(e) enc_note(m, F[i].name, e, tmp);
        meta_take(m, F[i].f, tmp);
    }
    if(!m->year_date && !m->year_year){ char y[5]; memcpy(y, t + 93, 4); y[4] = 0; m->year_year = year4(y); }
    if(!m->track && t[125] == 0 && t[126]) m->track = t[126];
}

/* A Vorbis comment body (vendor length + vendor + count + "KEY=value" items, little-endian lengths), as in a FLAC
 * VORBIS_COMMENT block and an Ogg comment packet. Repeated keys (several ARTIST or GENRE) are all kept. A long value
 * (lyrics) is read only as far as it is used. ogg: an empty TITLE= still counts as a title tag (SONG.TITLE_TAG). */
static void vorbis_comments(const unsigned char *b, uint32_t len, scan_meta *m, int ogg){
    /* overflow-safe bounds: need 4(vendor-len field) + vlen(vendor) + 4(comment count) <= len, i.e. vlen <= len-8.
     * Use subtraction so a hostile vlen like 0xFFFFFFFA can't wrap an addition past the guard. */
    if(len<8) return;
    uint32_t vlen=le32(b);
    if(vlen > len-8) return;
    uint32_t off=4+vlen;                          /* off <= len-4, so le32(b+off) is in-bounds */
    uint32_t cnt=le32(b+off); off+=4;
    for(uint32_t i=0;i<cnt && off+4<=len;i++){
        uint32_t clen=le32(b+off); off+=4;
        if(clen>len-off) break;                  /* overflow-safe (len-off >= 0) */
        const unsigned char *kv = b + off, *eq = memchr(kv, '=', clen);
        off += clen;
        if(!eq || eq == kv || eq - kv >= 40) continue;
        char key[40]; memcpy(key, kv, (size_t)(eq - kv)); key[eq - kv] = 0;
        int f = tag_field(key);
        if(f == MF_NONE || !strncasecmp(key, "WM/", 3)) continue;
        if(f == MF_TITLE && ogg) m->title_tag = 1;
        char v[TAGLEN];
        size_t vn = clen - (size_t)(eq - kv) - 1;
        int e = u8_copy(eq + 1, vn > 3 * TAGLEN ? 3 * TAGLEN : vn, v, sizeof v);
        if(e) enc_note(m, key, e, v);
        meta_take(m, f, v);
    }
}

/* FLAC: "fLaC" magic + metadata blocks; parse the VORBIS_COMMENT block (type 4). An ID3v2 tag some taggers put in
 * front of the magic is stepped over (it used to hide every tag of the file). Bounded like the ID3 path. */
static void flac_read(FILE *f, scan_meta *m, int *rerr){
    off_t fsz = ad_size(f);
    if(fsz < 0){ if(rerr) *rerr = 1; return; }
    off_t start = ad_skip_id3(f, fsz);
    if(fseeko(f, start, SEEK_SET) != 0){ if(rerr && errno==EIO) *rerr=1; return; }
    unsigned char magic[4];
    if(fread(magic,1,4,f)!=4 || memcmp(magic,"fLaC",4)!=0) return;
    for(int guard=0; guard<256; guard++){
        unsigned char h[4];
        if(fread(h,1,4,f)!=4) return;
        int last = h[0]&0x80, type = h[0]&0x7f;
        uint32_t len = ((uint32_t)h[1]<<16)|((uint32_t)h[2]<<8)|h[3];
        if(type==4){                                    /* VORBIS_COMMENT */
            if(len<8 || len>1024*1024) return;
            unsigned char *b=malloc(len); if(!b){ if(rerr)*rerr=1; return; }   /* OOM: signal a real read failure (ferror won't catch it) */
            if(fread(b,1,len,f)!=len){ free(b); return; }                      /* short read: ferror() in tags_with_provenance distinguishes IO error from EOF */
            vorbis_comments(b,len,m,0);
            free(b);
            return;                                      /* got the comment block */
        }
        if(last) return;
        if(fseek(f,(long)len,SEEK_CUR)!=0){ if(rerr && errno==EIO) *rerr=1; return; }   /* seek IO error (ferror won't catch it) */
    }
}

/* ---- MP4 / M4A (iTunes-style metadata) ----
 * Walk the atom tree moov -> udta -> meta -> ilst and read the text/number tag atoms. Fully bounded:
 * every atom size is validated against its parent before use, so a malformed/hostile file can't read
 * out of the buffer. The song length comes separately from audiodur.h (the audio track's mdhd). */

/* Find the first child atom of `type` in buf[pos..end). On success set cs/ce to its PAYLOAD range and
 * return 1. Handles 32-bit, 64-bit (size==1) and to-end (size==0) atoms; rejects anything that overruns. */
static int mp4_child(const unsigned char *buf, long pos, long end, const char *type, long *cs, long *ce){
    while(pos + 8 <= end){
        uint64_t sz = be32(buf + pos);
        long hdr = 8;
        if(sz == 1){                                    /* 64-bit extended size */
            if(pos + 16 > end) break;
            sz = ((uint64_t)be32(buf + pos + 8) << 32) | be32(buf + pos + 12);
            hdr = 16;
        } else if(sz == 0){                             /* runs to the end of the parent */
            sz = (uint64_t)(end - pos);
        }
        if(sz < (uint64_t)hdr) break;                   /* smaller than its own header */
        if(sz > (uint64_t)(end - pos)) break;           /* overruns the parent */
        if(memcmp(buf + pos + 4, type, 4) == 0){ *cs = pos + hdr; *ce = pos + (long)sz; return 1; }
        pos += (long)sz;
    }
    return 0;
}

/* Stream-walk the atoms in the file range [start,end) looking for a child of `type`, WITHOUT reading
 * any atom body: siblings (a multi-MB mdat or a long book's trak sample tables) are seek-skipped, so
 * this costs a handful of 8-byte header reads no matter how big the file is. On success set *cpay (the
 * child's PAYLOAD file offset) + *cpsz (payload size) and return 1. Handles 32-bit, 64-bit (size==1)
 * and to-end (size==0) atoms; guards every offset against overflow/rewind so a hostile size can't loop
 * forever or rewind. fseek/ftell failures set rerr (ferror never catches those). */
/* off_t (64-bit on musl for every arch) + fseeko/ftello so a book >=2GiB is walked correctly instead of
 * failing the whole tag/chapter read on a 32-bit `long` file offset. */
static int mp4_find_child(FILE *f, off_t start, off_t end, const char *type,
                          off_t *cpay, uint64_t *cpsz, int *rerr){
    if(fseeko(f, start, SEEK_SET) != 0){ if(rerr) *rerr = 1; return 0; }
    for(;;){
        off_t here = ftello(f);
        if(here < 0){ if(rerr) *rerr = 1; return 0; }
        if(here >= end || end - here < 8) break;          /* no room for another header (end-here avoids here+8 overflow near the max) */
        unsigned char h[16];
        if(fread(h,1,8,f) != 8) break;                   /* clean EOF/short (ferror catches real IO) */
        uint64_t sz = be32(h); int hdr = 8;
        if(sz == 1){ if(end - here < 16 || fread(h+8,1,8,f)!=8) break; sz = ((uint64_t)be32(h+8)<<32)|be32(h+12); hdr = 16; }
        else if(sz == 0) sz = (uint64_t)(end - here);    /* runs to the end of the parent */
        if(sz < (uint64_t)hdr) break;                    /* smaller than its own header */
        if(sz > (uint64_t)(end - here)) break;           /* overruns the parent */
        if(memcmp(h+4,type,4)==0){ *cpay = here + hdr; *cpsz = sz - (uint64_t)hdr; return 1; }
        if(sz > (uint64_t)(INT64_MAX - here)) break;     /* next offset would overflow/rewind */
        if(fseeko(f, here + (off_t)sz, SEEK_SET) != 0){ if(rerr) *rerr = 1; return 0; }
    }
    return 0;
}

/* Locate moov/udta: its payload's file offset + size. All the tags + chapters + cover live under udta; the
 * multi-MB sample tables (trak) are never read. Returns 1 with upay + usz, else 0. */
static int mp4_find_udta(FILE *f, off_t *upay, uint64_t *usz, int *rerr){
    if(fseeko(f, 0, SEEK_END) != 0){ if(rerr) *rerr = 1; return 0; }
    off_t fsz = ftello(f);
    if(fsz < 0){ if(rerr) *rerr = 1; return 0; }   /* ftello error -> a real IO failure, not "no metadata" */
    if(fsz < 8) return 0;
    off_t moov_pay; uint64_t moov_psz;
    if(!mp4_find_child(f, 0, fsz, "moov", &moov_pay, &moov_psz, rerr)) return 0;
    off_t moov_end = moov_pay + (off_t)moov_psz;         /* find_child already bounded moov to the file */
    off_t udta_pay; uint64_t udta_sz;
    if(!mp4_find_child(f, moov_pay, moov_end, "udta", &udta_pay, &udta_sz, rerr)) return 0;
    if(udta_sz == 0) return 0;                           /* genuinely no udta -> no tags (filename fallback is correct) */
    if(udta_sz > 4u*1024*1024){ if(rerr) *rerr = 1; return 0; }  /* too big to load (e.g. a music m4a with multi-MB cover art) -> a READ failure, not "no tags": preserve the existing row instead of clobbering good title/artist/album with the filename fallback */
    *upay = udta_pay; *usz = udta_sz;
    return 1;
}

/* Locate moov/udta and load ONLY the udta payload into a fresh malloc'd buffer (caller frees). This keeps
 * the allocation tiny (a few KB, plus an embedded cover if present) even for a 20-hour book, instead of
 * loading a whole many-MB moov on a ~19MB-free device. Returns 1 with out_buf + out_sz, else 0. */
static int mp4_load_udta(FILE *f, unsigned char **out_buf, uint64_t *out_sz, int *rerr){
    off_t udta_pay; uint64_t udta_sz;
    if(!mp4_find_udta(f, &udta_pay, &udta_sz, rerr)) return 0;
    unsigned char *buf = malloc((size_t)udta_sz);
    if(!buf){ if(rerr) *rerr = 1; return 0; }            /* OOM: a real read failure (don't clobber the row) */
    if(fseeko(f, udta_pay, SEEK_SET) != 0){ if(rerr) *rerr = 1; free(buf); return 0; }
    if(fread(buf,1,(size_t)udta_sz,f) != (size_t)udta_sz){ free(buf); return 0; }
    *out_buf = buf; *out_sz = udta_sz;
    return 1;
}
/* The ilst items mp4_read uses; every other item (the covr artwork above all) is seeked past, never read. */
static int mp4_field(const unsigned char *k){
    if(k[0] == 0xA9){
        if(!memcmp(k + 1, "nam", 3)) return MF_TITLE;
        if(!memcmp(k + 1, "ART", 3)) return MF_ARTIST;
        if(!memcmp(k + 1, "alb", 3)) return MF_ALBUM;
        if(!memcmp(k + 1, "gen", 3)) return MF_GENRE;
        if(!memcmp(k + 1, "day", 3)) return MF_DATE;
        if(!memcmp(k + 1, "lyr", 3)) return MF_LYRICS;
        return MF_NONE;
    }
    static const struct { char k[5]; unsigned char f; } M[] = {
        {"aART",MF_ALBUMARTIST}, {"trkn",MF_TRACK}, {"disk",MF_DISC}, {"cpil",MF_COMPILATION}, {"tmpo",MF_BPM},
        {"soaa",MF_SORT_AA}, {"soar",MF_SORT_A}, {"soal",MF_SORT_AL}, {"sonm",MF_SORT_T}, {"gnre",MF_GNRE}, {"----",MF_CUSTOM},
    };
    for(unsigned i = 0; i < sizeof M / sizeof M[0]; i++) if(!memcmp(k, M[i].k, 4)) return M[i].f;
    return MF_NONE;
}
static uint64_t mp4_uint(const unsigned char *v, long vn){
    uint64_t x = 0;
    for(long i = 0; i < vn && i < 8; i++) x = x << 8 | v[i];
    return x;
}
/* One ilst item's payload b[0..n): its "data" atoms (an item may hold several values, each in its own data atom),
 * and for a "----" item the "name" atom that says what it is. */
static void mp4_item(scan_meta *m, const unsigned char *key, int f, const unsigned char *b, long n){
    char tag[40], name[64] = "";
    if(key[0] == 0xA9) snprintf(tag, sizeof tag, "\xC2\xA9%.3s", key + 1); else snprintf(tag, sizeof tag, "%.4s", key);
    if(f == MF_CUSTOM){                                   /* the name comes before the data */
        long cs, ce;
        if(!mp4_child(b, 0, n, "name", &cs, &ce) || ce - cs < 4) return;
        u8_copy(b + cs + 4, (size_t)(ce - cs - 4), name, sizeof name);
        f = custom_field(name);
        if(f == MF_NONE) return;
        snprintf(tag, sizeof tag, "----:%.30s", name);
    }
    for(long pos = 0, nv = 0; pos + 8 <= n && nv < MAXV; ){
        long cs, ce;
        if(!mp4_child(b, pos, n, "data", &cs, &ce)) break;
        pos = ce;
        if(ce - cs < 8) continue;
        unsigned type = be32(b + cs) & 0xFFFFFF;
        const unsigned char *v = b + cs + 8; long vn = ce - cs - 8;
        nv++;
        if(f == MF_TRACK || f == MF_DISC){                /* binary: [2 reserved][2 number][2 total] */
            int *num = f == MF_TRACK ? &m->track : &m->disc, *tot = f == MF_TRACK ? &m->track_total : &m->disc_total;
            if(vn >= 4 && !*num) *num = v[2] << 8 | v[3];
            if(vn >= 6 && !*tot) *tot = v[4] << 8 | v[5];
            continue;
        }
        if(f == MF_GNRE){ if(vn >= 2 && !m->gnre) m->gnre = v[0] << 8 | v[1]; continue; }   /* ID3 genre number + 1 */
        if(f == MF_COMPILATION || (f == MF_BPM && type != 1)){
            if(vn < 1 || vn > 8) continue;
            uint64_t x = mp4_uint(v, vn);
            if(f == MF_COMPILATION){ if(x) m->compilation = 1; }
            else if(m->bpm <= 0 && x > 0 && x < 1000) m->bpm = (double)x;
            continue;
        }
        char s[TAGLEN]; int e;
        if(type == 2){ size_t u; e = utf16_copy(v, (size_t)vn, 1, s, sizeof s, &u); }
        else if(type == 1 || type == 0) e = u8_copy(v, (size_t)vn, s, sizeof s);
        else continue;
        if(e) enc_note(m, tag, e, s);
        meta_take(m, f, s);
    }
}
/* udta -> meta -> ilst, walked in the file with mp4_child's bounds; only the items above are read. The udta size
 * rule in mp4_find_udta still applies. A read that fails part way leaves the fields untouched (see meta_save). */
static void mp4_read(FILE *f, scan_meta *m, int *rerr){
    off_t up; uint64_t usz;
    if(!mp4_find_udta(f, &up, &usz, rerr)) return;
    meta_save(m);
    int err0 = ferror(f), ok = 1;
    off_t mts, is; uint64_t msz, isz;
    if(mp4_find_child(f, up, up + (off_t)usz, "meta", &mts, &msz, rerr) &&
       /* the iTunes `meta` atom carries 4 version/flags bytes before its children; some writers omit them */
       (mp4_find_child(f, mts+4, mts + (off_t)msz, "ilst", &is, &isz, rerr) ||
        mp4_find_child(f, mts, mts + (off_t)msz, "ilst", &is, &isz, rerr))){
        off_t p = is, ie = is + (off_t)isz, at = ftello(f);   /* at = the stream position */
        while(ie - p >= 8){
            unsigned char h[16];
            int sk = skip_to(f, &at, p);
            if(sk){ if(sk == -2 && rerr) *rerr = 1; ok = 0; break; }
            if(fread(h,1,8,f) != 8){ ok = 0; break; }
            uint64_t sz = be32(h); int hdr = 8;
            if(sz == 1){ if(ie - p < 16) break; if(fread(h+8,1,8,f) != 8){ ok = 0; break; } sz = ((uint64_t)be32(h+8)<<32)|be32(h+12); hdr = 16; }
            else if(sz == 0) sz = (uint64_t)(ie - p);
            if(sz < (uint64_t)hdr || sz > (uint64_t)(ie - p)) break;
            at = p + hdr;
            int fld = mp4_field(h+4);
            if(fld != MF_NONE){
                size_t n = (size_t)(sz - (uint64_t)hdr);
                unsigned char *b = malloc(n ? n : 1);
                if(!b){ if(rerr) *rerr = 1; ok = 0; break; }                       /* OOM: a real read failure (don't clobber the row) */
                if(fread(b,1,n,f) != n){ free(b); ok = 0; break; }
                at += (off_t)n;
                mp4_item(m, h+4, fld, b, (long)n);
                free(b);
            }
            p += (off_t)sz;
        }
    }
    if(!ok || (!err0 && ferror(f))) meta_restore(m);
    else if(!m->n_genres && m->gnre >= 1 && m->gnre <= 192) meta_take(m, MF_GENRE, ID3_GENRES[m->gnre - 1]);
}

/* Read Nero 'chpl' chapters (moov/udta/chpl) from an M4B/M4A into out[0..max); returns the count (0 if
 * none/unsupported/malformed). Handles chpl version 0 (count at payload+4) and version 1 (count at
 * payload+8); every field is bounds-checked against the atom. Start times are 100ns ticks -> ms. Titles
 * are length-prefixed UTF-8 (no NUL), truncated only at a code-point boundary; an empty title becomes
 * "Chapter N". QuickTime chapter-track chapters are NOT read here (a later stage). */
static int scan_read_chapters_leased(const char *path, chapter_t *out, int max){
    if(!path || !out || max <= 0) return 0;
    FILE *f = fopen(path, "rb"); if(!f) return 0;
    unsigned char *buf; uint64_t udta_sz; int rerr = 0;
    int ok = mp4_load_udta(f, &buf, &udta_sz, &rerr);
    fclose(f);
    if(!ok) return 0;
    int n = 0;
    long cs, ce;
    if(mp4_child(buf, 0, (long)udta_sz, "chpl", &cs, &ce) && ce - cs >= 5){   /* buf IS the udta payload */
        int ver = buf[cs];
        long cnt_off = (ver == 1) ? 8 : (ver == 0) ? 4 : -1;   /* count position by version; reject others */
        if(cnt_off >= 0 && ce - cs > cnt_off){
            int count = buf[cs + cnt_off];
            long q = cs + cnt_off + 1;
            int bad = 0;
            long prev_ms = -1;                                 /* chapters must be chronological */
            for(int i = 0; i < count; i++){                    /* validate EVERY declared entry */
                if(q + 9 > ce){ bad = 1; break; }              /* 8-byte start + 1-byte length */
                uint64_t ticks = ((uint64_t)be32(buf+q) << 32) | be32(buf+q+4); q += 8;
                int tl = buf[q]; q += 1;
                if(q + tl > ce){ bad = 1; break; }
                uint64_t ms64 = ticks / 10000;                 /* 100ns ticks -> ms */
                if(ms64 > (uint64_t)LONG_MAX){ bad = 1; break; }  /* unrepresentable on a 32-bit long */
                if((long)ms64 < prev_ms){ bad = 1; break; }    /* out-of-order start -> reject the whole list (the UI assumes ascending) */
                prev_ms = (long)ms64;
                if(n < max){                                   /* store up to max, but keep validating the rest */
                    out[n].start_ms = (long)ms64;
                    int cl = tl;
                    if(cl > CHAP_TITLE - 1){ cl = CHAP_TITLE - 1; while(cl > 0 && (buf[q+cl] & 0xC0) == 0x80) cl--; }  /* don't split UTF-8 */
                    memcpy(out[n].title, buf+q, (size_t)cl); out[n].title[cl] = 0;
                    str_trim(out[n].title);
                    if(!out[n].title[0]) snprintf(out[n].title, CHAP_TITLE, "Chapter %d", n+1);
                    n++;
                }
                q += tl;
            }
            if(bad) n = 0;                                     /* a malformed/truncated list -> report none, not a partial one */
        }
    }
    free(buf);
    return n;
}

/* Read the narrator of an m4b into out[0..cap): the iTunes composer atom (0xA9 + "wrt"), the near-universal
 * convention for an audiobook's narrator. Returns 1 if a non-empty value was found. Reuses the udta-only
 * loader + bounded ilst walk. */
static int scan_read_narrator_leased(const char *path, char *out, int cap){
    if(!path || !out || cap <= 0) return 0;
    out[0] = 0;
    FILE *f = fopen(path, "rb"); if(!f) return 0;
    unsigned char *buf; uint64_t udta_sz; int rerr = 0;
    int ok = mp4_load_udta(f, &buf, &udta_sz, &rerr);
    fclose(f);
    if(!ok) return 0;
    long mts, mte, is, ie; int found = 0;
    if(mp4_child(buf, 0, (long)udta_sz, "meta", &mts, &mte) &&
       (mp4_child(buf, mts+4, mte, "ilst", &is, &ie) || mp4_child(buf, mts, mte, "ilst", &is, &ie))){
        long p = is;
        while(p + 8 <= ie){
            uint64_t sz = be32(buf+p); long hdr = 8;
            if(sz == 1){ if(p+16 > ie) break; sz = ((uint64_t)be32(buf+p+8)<<32)|be32(buf+p+12); hdr = 16; }
            else if(sz == 0) sz = (uint64_t)(ie - p);
            if(sz < (uint64_t)hdr || sz > (uint64_t)(ie - p)) break;
            const unsigned char *key = buf + p + 4;
            if(key[0]==0xA9 && key[1]=='w' && key[2]=='r' && key[3]=='t'){   /* 0xA9 + "wrt" = composer = narrator */
                long dts, dte;
                if(mp4_child(buf, p+hdr, p+(long)sz, "data", &dts, &dte) && dte - dts >= 8){
                    long vn = dte - dts - 8; if(vn > cap-1) vn = cap-1; if(vn < 0) vn = 0;
                    memcpy(out, buf+dts+8, (size_t)vn); out[vn] = 0; str_trim(out);
                    found = out[0] != 0;
                }
                break;
            }
            p += (long)sz;
        }
    }
    free(buf);
    return found;
}

static int scan_cancelled(void *ctx){
    (void)ctx;
    return atomic_load(&g_abort);
}

/* ---- Ogg Vorbis ----
 * Pages ("OggS", 27-byte header + segment table). The comment header is the stream's SECOND packet
 * ("\x03vorbis" + a Vorbis comment body); it may span pages, so packets are reassembled from the lacing values.
 * Bounded: at most 64 pages and 1 MiB of packet data; only the first logical stream is read. */
static void ogg_read(FILE *f, scan_meta *m, int *rerr){
    unsigned char id[8];
    if(ad_read_at(f,0,id,8) && !memcmp(id,"OggS",4)){
        unsigned char first[8], ns;
        if(ad_read_at(f,26,&ns,1) && ad_read_at(f,27+ns,first,8) && !memcmp(first,"OpusHead",8)){
            unsigned char *tags=NULL; uint32_t len=0;
            long long rc=ad_opus(f,&tags,&len,scan_cancelled,NULL);
            if(rc==AD_CANCELLED){ if(rerr) *rerr=AD_CANCELLED; }
            else if(rc<0){ if(rerr) *rerr=1; }
            if(rc>0){
                vorbis_comments(tags+8,len-8,m,1);
                free(tags);
            }
            return;
        }
    }
    if(fseek(f,0,SEEK_SET)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
    enum { OGG_MAX = 1024*1024 };
    unsigned char *pkt = malloc(OGG_MAX);
    if(!pkt){ if(rerr)*rerr=1; return; }
    uint32_t plen = 0, serial = 0; int npkt = 0, have_serial = 0;
    for(int page = 0; page < 64 && npkt < 2; page++){
        unsigned char h[27], seg[255];
        if(fread(h,1,27,f)!=27 || memcmp(h,"OggS",4)!=0 || h[4]!=0) break;
        uint32_t ser = le32(h+14);
        if(!have_serial){ serial = ser; have_serial = 1; }
        int nseg = h[26];
        if(fread(seg,1,(size_t)nseg,f)!=(size_t)nseg) break;
        if(ser != serial){                                    /* another logical stream: skip its page body */
            long body = 0; for(int i=0;i<nseg;i++) body += seg[i];
            if(fseek(f,body,SEEK_CUR)!=0){ if(rerr && errno==EIO) *rerr=1; break; }
            continue;
        }
        for(int i = 0; i < nseg && npkt < 2; i++){
            uint32_t n = seg[i];
            if(n > OGG_MAX - plen){ npkt = 99; break; }      /* packet too large for the bound: give up */
            if(n && fread(pkt+plen,1,n,f)!=n){ npkt = 99; break; }
            plen += n;
            if(seg[i] < 255){                                 /* a lacing value < 255 ends the packet */
                npkt++;
                if(npkt == 2){
                    if(plen > 7 && pkt[0]==3 && !memcmp(pkt+1,"vorbis",6))
                        vorbis_comments(pkt+7, plen-7, m, 1);
                    break;
                }
                plen = 0;                                     /* packet 1 (identification) done: start packet 2 */
            }
        }
        if(npkt >= 2) break;
    }
    free(pkt);
}

/* ---- APE (APEv2 tag) ----
 * A 32-byte footer "APETAGEX" at the end of the file (or just before a 128-byte ID3v1 tag): version, tag size
 * (items + footer), item count. Items: value size (LE), flags (LE), NUL-terminated key, UTF-8 value (several values
 * are NUL-separated). Bounded: tag <= 1 MiB, <= 512 items; binary items (flags bits 1-2 != 0) are skipped. */
static void ape_read(FILE *f, scan_meta *m, int *rerr){
    unsigned char ft[32];
    long ends[2] = { -32, -32-128 };
    for(int k = 0; k < 2; k++){
        if(fseek(f,ends[k],SEEK_END)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
        if(fread(ft,1,32,f)!=32) return;
        if(memcmp(ft,"APETAGEX",8)!=0) continue;
        uint32_t size = le32(ft+12), items = le32(ft+16);
        if(size < 32 || size > 1024*1024 || items > 512) return;
        uint32_t blen = size - 32;                                  /* the items, before the footer */
        if(fseek(f, ends[k] - (long)blen, SEEK_END)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
        unsigned char *b = malloc(blen ? blen : 1);
        if(!b){ if(rerr)*rerr=1; return; }
        if(fread(b,1,blen,f)!=blen){ free(b); return; }
        uint32_t off = 0;
        for(uint32_t i = 0; i < items && off + 8 <= blen; i++){
            uint32_t vlen = le32(b+off), flags = le32(b+off+4); off += 8;
            uint32_t kend = off; while(kend < blen && b[kend]) kend++;
            if(kend >= blen) break;                                  /* key must be NUL-terminated inside */
            const char *key = (const char *)b+off; off = kend + 1;
            if(vlen > blen - off) break;                             /* overflow-safe */
            int fld = ((flags >> 1) & 3) == 0 ? tag_field(key) : MF_NONE;   /* UTF-8 text items only */
            if(fld != MF_NONE && strncasecmp(key, "WM/", 3)){
                for(uint32_t q = off, nv = 0; q < off + vlen && nv < MAXV; nv++){
                    uint32_t e = q; while(e < off + vlen && b[e]) e++;
                    char v[TAGLEN];
                    int enc = u8_copy(b + q, e - q, v, sizeof v);
                    if(enc) enc_note(m, key, enc, v);
                    if(fld == MF_GENRE) genre_take(m, v, 0); else meta_take(m, fld, v);
                    q = e + 1;
                }
            }
            off += vlen;
        }
        free(b);
        return;
    }
}

/* the file's size (64-bit off_t on the Disc's musl and on hosts), -1 if it can't be read */
static off_t file_size(FILE *f){
    off_t here = ftello(f);
    if(here < 0 || fseeko(f, 0, SEEK_END) != 0) return -1;
    off_t sz = ftello(f);
    if(fseeko(f, here, SEEK_SET) != 0) return -1;
    return sz;
}
/* An ID3v2 tag starting at the current position that fits entirely in `room` bytes (so an embedded tag can never read
 * into the chunks after it): read it with id3v2_read; else skip it. */
static void id3v2_read_bounded(FILE *f, off_t room, scan_meta *m, int *rerr){
    off_t at = ftello(f);
    unsigned char h[10];
    if(at < 0 || room < 10 || fread(h,1,10,f)!=10) return;
    if(memcmp(h,"ID3",3)!=0 || ((h[6]|h[7]|h[8]|h[9]) & 0x80)) return;
    off_t need = 10 + (off_t)synch32(h+6);
    if(need > room) return;                                   /* claims more than its chunk: not read */
    if(fseeko(f, at, SEEK_SET)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
    id3v2_read(f,m,rerr);
}
/* AIFF NAME/AUTH text: the spec says ASCII; UTF-8 when it is valid UTF-8, else Latin-1 (and reported) */
static void aiff_text(scan_meta *m, const char *tag, const unsigned char *t, size_t n, char *dst){
    size_t len = 0; while(len < n && t[len]) len++;
    int e = utf8_ok(t, len) ? u8_copy(t, len, dst, TAGLEN) : latin1_copy(t, len, dst, TAGLEN);
    if(e) enc_note(m, tag, e, dst);
}
static void aiff_read(FILE *f, scan_meta *m, int *rerr){
    unsigned char h[12];
    if(fread(h,1,12,f)!=12 || memcmp(h,"FORM",4)!=0 || (memcmp(h+8,"AIFF",4)!=0 && memcmp(h+8,"AIFC",4)!=0)) return;
    off_t fsz = file_size(f);
    if(fsz < 0){ if(rerr) *rerr = 1; return; }
    char name[TAGLEN] = "", auth[TAGLEN] = "";
    for(int guard = 0; guard < 256; guard++){
        unsigned char c[8];
        if(fread(c,1,8,f)!=8) break;
        off_t here = ftello(f);
        if(here < 0){ if(rerr) *rerr = 1; break; }
        off_t len = (off_t)be32(c+4);
        if(len > fsz - here) break;                           /* checked BEFORE any arithmetic: a chunk inside the file */
        if(!memcmp(c,"ID3 ",4) || !memcmp(c,"id3 ",4)){
            id3v2_read_bounded(f,len,m,rerr);
        } else if((!memcmp(c,"NAME",4) || !memcmp(c,"AUTH",4)) && len > 0){
            int is_name = !memcmp(c,"NAME",4);
            size_t n = len < TAGLEN - 1 ? (size_t)len : TAGLEN - 1;
            unsigned char t[TAGLEN];
            if(fread(t,1,n,f)!=n) break;
            aiff_text(m, is_name ? "NAME" : "AUTH", t, n, is_name ? name : auth);
        }
        if(len >= fsz - here) break;                          /* this chunk (pad byte aside) runs to the end: nothing follows */
        off_t next = here + len + (len & 1);                  /* no overflow: len < fsz - here, so here+len+1 <= fsz */
        if(next >= fsz) break;
        if(fseeko(f,next,SEEK_SET)!=0){ if(rerr && errno==EIO) *rerr=1; break; }
    }
    if(!m->title[0] && name[0]) meta_take(m, MF_TITLE, name);
    if(!m->n_artists && auth[0]) meta_take(m, MF_ARTIST, auth);
}

/* ---- DSF (DSD stream file) ----
 * "DSD " chunk: size (8 LE, = 28), total file size (8 LE), pointer to the metadata chunk (8 LE, 0 = none). The
 * metadata chunk is an ID3v2 tag, read by id3v2_read at that offset (only when it lies inside the file). */
static void dsf_read(FILE *f, scan_meta *m, int *rerr){
    unsigned char h[28];
    if(fread(h,1,28,f)!=28 || memcmp(h,"DSD ",4)!=0) return;
    uint64_t meta  = (uint64_t)le32(h+20) | (uint64_t)le32(h+24) << 32;
    off_t fsz = file_size(f);                                /* the REAL size, not the header's claim */
    if(fsz < 0){ if(rerr) *rerr = 1; return; }
    if(meta < 28 || meta >= (uint64_t)fsz) return;           /* 0 = no metadata; must lie inside the file */
    if(fseeko(f,(off_t)meta,SEEK_SET)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
    id3v2_read_bounded(f,fsz-(off_t)meta,m,rerr);
}

/* ---- WMA (ASF) ----
 * Header Object (GUID 75B22630-...) holds sub-objects: Content Description (75B22633-...: title, author as
 * UTF-16LE with a 5 x u16 length table), Extended Content Description (D2D0A440-...: named attributes such
 * as WM/AlbumTitle, WM/AlbumArtist, WM/Genre, WM/TrackNumber, WM/PartOfSet; a repeated attribute is several values),
 * the Header Extension (5FBF03B5-...) whose Metadata (C5F8CBEA-...) and Metadata Library (44231C94-...) objects hold
 * more values of the same names (taggers put a second Author or WM/Genre there), and the audio Stream Properties
 * (B7DC0791-...: the format). Values are taken in that order, whatever order the objects are in. The header is read
 * whole (bounded to 1 MiB) and every object/field length is checked against it. */
static const unsigned char ASF_HDR[16]  = {0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
static const unsigned char ASF_CD[16]   = {0x33,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
static const unsigned char ASF_ECD[16]  = {0x40,0xA4,0xD0,0xD2,0x07,0xE3,0xD2,0x11,0x97,0xF0,0x00,0xA0,0xC9,0x5E,0xA8,0x50};
static const unsigned char ASF_HEO[16]  = {0xB5,0x03,0xBF,0x5F,0x2E,0xA9,0xCF,0x11,0x8E,0xE3,0x00,0xC0,0x0C,0x20,0x53,0x65};
static const unsigned char ASF_MO[16]   = {0xEA,0xCB,0xF8,0xC5,0xAF,0x5B,0x77,0x48,0x84,0x67,0xAA,0x8C,0x44,0xFA,0x4C,0xCA};
static const unsigned char ASF_MLO[16]  = {0x94,0x1C,0x23,0x44,0x98,0x94,0xD1,0x49,0xA1,0x41,0x1D,0x13,0x4E,0x45,0x70,0x54};
static const unsigned char ASF_SPO[16]  = {0x91,0x07,0xDC,0xB7,0xB7,0xA9,0xCF,0x11,0x8E,0xE6,0x00,0xC0,0x0C,0x20,0x53,0x65};
static const unsigned char ASF_AUDIO[16]= {0x40,0x9E,0x69,0xF8,0x4D,0x5B,0xCF,0x11,0xA8,0xFD,0x00,0x80,0x5F,0x5C,0x44,0x2B};
static void wma_value(scan_meta *m, const char *tag, int fld, const unsigned char *v, uint32_t vl){
    char s[TAGLEN];
    size_t u;
    int e = utf16_copy(v, vl, 0, s, sizeof s, &u);
    if(e) enc_note(m, tag, e, s);
    if(fld == MF_GENRE) genre_take(m, s, 0); else meta_take(m, fld, s);
}
/* one named attribute: vt 0 string, 2 BOOL (4 bytes in the ECD, 2 in the metadata objects), 3 DWORD, 4 QWORD, 5 WORD */
static void wma_attr(scan_meta *m, const char *nm, uint32_t vt, const unsigned char *v, uint32_t vl){
    int fld = strcmp(nm, "Author") ? tag_field(nm) : MF_ARTIST;   /* more values of the Content Description's artist */
    if(fld == MF_NONE) return;
    char val[24] = "";
    if(vt == 0) wma_value(m, nm, fld, v, vl);
    else if((vt == 2 || vt == 3) && vl >= 4) snprintf(val, sizeof val, "%u", (unsigned)le32(v));
    else if((vt == 2 || vt == 5) && vl >= 2) snprintf(val, sizeof val, "%u", (unsigned)(v[0]|v[1]<<8));
    else if(vt == 4 && vl >= 8) snprintf(val, sizeof val, "%llu", (unsigned long long)le32(v) | (unsigned long long)le32(v+4) << 32);
    if(val[0]) meta_take(m, fld, val);
}
/* a Metadata / Metadata Library object: u16 count; records of u16 language/reserved, u16 stream, u16 name length,
 * u16 type, u32 data length, the name, the data */
static void wma_metadata(scan_meta *m, const unsigned char *d, uint32_t dl){
    if(dl < 2) return;
    uint32_t cnt = d[0]|d[1]<<8, p = 2;
    for(uint32_t i = 0; i < cnt && i < 512 && p + 12 <= dl; i++){
        uint32_t nl = d[p+4]|d[p+5]<<8, vt = d[p+6]|d[p+7]<<8, vl = le32(d+p+8);
        p += 12;
        if(nl > dl - p) break;
        char nm[64]; size_t u; utf16_copy(d+p, nl, 0, nm, sizeof nm, &u); p += nl;
        if(vl > dl - p) break;
        wma_attr(m, nm, vt, d+p, vl);
        p += vl;
    }
}
static void wma_read(FILE *f, scan_meta *m, int *rerr){
    unsigned char h[30];
    if(fread(h,1,30,f)!=30 || memcmp(h,ASF_HDR,16)!=0) return;
    uint64_t hsize = (uint64_t)le32(h+16) | (uint64_t)le32(h+20) << 32;
    uint32_t nobj = le32(h+24);
    if(hsize <= 30 || hsize > 1024*1024 || nobj > 1024) return;
    uint32_t blen = (uint32_t)hsize - 30;
    unsigned char *b = malloc(blen);
    if(!b){ if(rerr)*rerr=1; return; }
    if(fread(b,1,blen,f)!=blen){ free(b); return; }
    for(int pass = 0; pass < 3; pass++){      /* 0: Content Description + format, 1: Extended Content, 2: Header Extension */
    uint32_t off = 0;
    for(uint32_t o = 0; o < nobj && off + 24 <= blen; o++){
        uint64_t osz = (uint64_t)le32(b+off+16) | (uint64_t)le32(b+off+20) << 32;
        if(osz < 24 || osz > blen - off) break;
        const unsigned char *d = b + off + 24; uint32_t dl = (uint32_t)osz - 24;
        if(pass == 0 && !memcmp(b+off,ASF_CD,16) && dl >= 10){
            uint32_t tl = d[0]|d[1]<<8, al = d[2]|d[3]<<8;
            uint32_t p = 10;
            if(tl <= dl - p){ wma_value(m, "Title", MF_TITLE, d+p, tl); p += tl; } else p = dl;
            if(al <= dl - p) wma_value(m, "Author", MF_ARTIST, d+p, al);
        } else if(pass == 1 && !memcmp(b+off,ASF_ECD,16) && dl >= 2){
            uint32_t cnt = d[0]|d[1]<<8, p = 2;
            for(uint32_t i = 0; i < cnt && i < 512 && p + 2 <= dl; i++){
                uint32_t nl = d[p]|d[p+1]<<8; p += 2;
                if(nl > dl - p) break;
                char nm[64]; size_t u; utf16_copy(d+p, nl, 0, nm, sizeof nm, &u); p += nl;
                if(p + 4 > dl) break;
                uint32_t vt = d[p]|d[p+1]<<8, vl = d[p+2]|d[p+3]<<8; p += 4;
                if(vl > dl - p) break;
                wma_attr(m, nm, vt, d+p, vl);
                p += vl;
            }
        } else if(pass == 2 && !memcmp(b+off,ASF_HEO,16) && dl >= 22){
            uint32_t xl = le32(d+18), q = 0;                  /* reserved GUID + u16, then the inner objects */
            const unsigned char *x = d + 22;
            if(xl > dl - 22) xl = dl - 22;
            while(q + 24 <= xl){
                uint64_t isz = (uint64_t)le32(x+q+16) | (uint64_t)le32(x+q+20) << 32;
                if(isz < 24 || isz > xl - q) break;
                if(!memcmp(x+q,ASF_MO,16) || !memcmp(x+q,ASF_MLO,16)) wma_metadata(m, x+q+24, (uint32_t)isz - 24);
                q += (uint32_t)isz;
            }
        } else if(pass == 0 && !memcmp(b+off,ASF_SPO,16) && dl >= 54 + 16 && !memcmp(d,ASF_AUDIO,16)){
            const unsigned char *wf = d + 54;                    /* WAVEFORMATEX: codec, channels, rate, bytes/s, align, bits */
            unsigned codec = wf[0] | wf[1] << 8;
            if(!m->sample_rate){
                m->channels = wf[2] | wf[3] << 8;
                m->sample_rate = (int)le32(wf + 4);
                if(codec == 0x163 || codec == 0x0001) m->bits = wf[14] | wf[15] << 8;   /* lossless / PCM: real depth */
                snprintf(m->codec, sizeof m->codec, "%s", codec == 0x163 ? "wmal" : codec == 0x0001 ? "pcm" : "wma");
            }
        }
        off += (uint32_t)osz;
    }
    }
    free(b);
}

/* ---- the format: sample rate, bit depth (lossless only), channels, codec (library.db; 0 / "" = unknown) ----
 * Read from the same headers audiodur.h reads the length from; every read is bounded and a failure just leaves the
 * field unknown. */
static void set_codec(scan_meta *m, const char *c){ if(!m->codec[0]) snprintf(m->codec, sizeof m->codec, "%s", c); }
static int aiff_rate(const unsigned char *x){             /* 80-bit IEEE extended, integer rates only */
    int exp = ((x[0] & 0x7f) << 8) | x[1];
    unsigned long long mant = 0;
    for(int i = 0; i < 8; i++) mant = mant << 8 | x[2 + i];
    int shift = 16383 + 63 - exp;
    if((x[0] & 0x80) || shift < 0 || shift > 63) return 0;
    unsigned long long r = mant >> shift;
    return (r && (r << shift) == mant && r < 100000000ULL) ? (int)r : 0;
}
static void fmt_m4a(FILE *f, off_t fsz, scan_meta *m){
    off_t mv, mve;
    ad_mp4_budget = 4096;
    if(ad_mp4_find(f, 0, fsz, "moov", &mv, &mve) != 1) return;
    off_t pos = mv;
    for(int guard = 0; guard < 256; guard++){                               /* the first audio trak */
        char t[5]; off_t tp, te;
        if(ad_mp4_atom(f, pos, mve, t, &tp, &te) != 1) return;
        pos = te;
        if(memcmp(t, "trak", 4)) continue;
        off_t md, mde, hd, hde, mi, mie, sb, sbe, sd, sde, dh, dhe, ep, ee;
        unsigned char hb[12];
        if(ad_mp4_find(f, tp, te, "mdia", &md, &mde) != 1) continue;
        if(ad_mp4_find(f, md, mde, "hdlr", &hd, &hde) != 1 || hde - hd < 12 || !ad_read_at(f, hd, hb, 12) || memcmp(hb + 8, "soun", 4)) continue;
        unsigned long long ts = 0, dur = 0;
        if(ad_mp4_find(f, md, mde, "mdhd", &dh, &dhe) == 1) ad_mp4_ts(f, dh, dhe, &ts, &dur);
        if(ad_mp4_find(f, md, mde, "minf", &mi, &mie) != 1 || ad_mp4_find(f, mi, mie, "stbl", &sb, &sbe) != 1
           || ad_mp4_find(f, sb, sbe, "stsd", &sd, &sde) != 1 || sde - sd < 8) return;
        char et[5]; unsigned char e[28];
        if(ad_mp4_atom(f, sd + 8, sde, et, &ep, &ee) != 1 || ee - ep < 28 || !ad_read_at(f, ep, e, 28)) return;
        /* audio sample entry: 6 reserved, 2 data ref, 2 version, 2 revision, 4 vendor, 2 channels, 2 sample size,
         * 2 compression id, 2 packet size, 4 rate (16.16, so it can't say more than 65535 Hz) */
        int ver = e[8] << 8 | e[9];
        m->channels = e[16] << 8 | e[17];
        unsigned rate = (unsigned)e[24] << 8 | e[25];
        if(!memcmp(et, "alac", 4)){
            set_codec(m, "alac");
            m->bits = e[18] << 8 | e[19];
            off_t cp, ce;                                   /* the ALAC cookie: real depth, channels and rate */
            off_t cstart = ep + 28 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
            unsigned char c[28];
            if(cstart < ee && ad_mp4_find(f, cstart, ee, "alac", &cp, &ce) == 1 && ce - cp >= 28 && ad_read_at(f, cp, c, 28)){
                if(c[9]) m->bits = c[9];
                if(c[13]) m->channels = c[13];
                if(be32(c + 24)) rate = be32(c + 24);
            }
        } else if(!memcmp(et, "mp4a", 4)) set_codec(m, "aac");
        else if(!memcmp(et, "fLaC", 4)) set_codec(m, "flac");
        else if(!memcmp(et, "Opus", 4)) set_codec(m, "opus");
        else if(!memcmp(et, "ac-3", 4)) set_codec(m, "ac3");
        else if(!memcmp(et, "ec-3", 4)) set_codec(m, "eac3");
        m->sample_rate = (int)(rate ? rate : (ts < 1000000 ? ts : 0));
        return;
    }
}
static void fmt_probe(FILE *f, const char *fname, scan_meta *m){
    off_t fsz = ad_size(f);
    if(fsz <= 0) return;
    unsigned char b[96];
    if(has_ext(fname,".flac")){
        set_codec(m, "flac");
        off_t o = ad_skip_id3(f, fsz);
        if(ad_read_at(f, o, b, 42) && !memcmp(b, "fLaC", 4) && (b[4] & 0x7f) == 0){
            const unsigned char *s = b + 8;                  /* STREAMINFO */
            m->sample_rate = (s[10] << 12) | (s[11] << 4) | (s[12] >> 4);
            m->channels = ((s[12] >> 1) & 7) + 1;
            m->bits = (((s[12] & 1) << 4) | (s[13] >> 4)) + 1;
        }
    } else if(has_ext(fname,".mp3")){
        ad_mpa a;
        if(ad_mpa_sync(f, ad_skip_id3(f, fsz), fsz, &a) >= 0){
            m->sample_rate = a.sr; m->channels = a.mono ? 1 : 2;
            set_codec(m, a.layer == 3 ? "mp3" : a.layer == 2 ? "mp2" : "mp1");
        } else set_codec(m, "mp3");
    } else if(has_ext(fname,".m4a") || has_ext(fname,".m4b")) fmt_m4a(f, fsz, m);
    else if(has_ext(fname,".wav")){
        off_t pos = 12;
        if(!ad_read_at(f, 0, b, 12) || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return;
        for(int guard = 0; guard < 64 && pos + 8 <= fsz; guard++){
            if(!ad_read_at(f, pos, b, 8)) return;
            uint32_t len = le32(b + 4);
            if(!memcmp(b, "fmt ", 4)){
                if(len < 16 || !ad_read_at(f, pos + 8, b + 8, len >= 40 ? 40 : 16)) return;
                unsigned tag = b[8] | b[9] << 8;
                if(tag == 0xFFFE && len >= 40) tag = b[32] | b[33] << 8;   /* WAVE_FORMAT_EXTENSIBLE: its subformat */
                m->channels = b[10] | b[11] << 8;
                m->sample_rate = (int)le32(b + 12);
                m->bits = b[22] | b[23] << 8;
                set_codec(m, tag == 1 || tag == 3 ? "pcm" : "wav");
                return;
            }
            pos += 8 + (off_t)len + (len & 1);
        }
    } else if(has_ext(fname,".aif") || has_ext(fname,".aiff")){
        if(!ad_read_at(f, 0, b, 12) || memcmp(b, "FORM", 4) || (memcmp(b + 8, "AIFF", 4) && memcmp(b + 8, "AIFC", 4))) return;
        int aifc = !memcmp(b + 8, "AIFC", 4);
        off_t pos = 12;
        for(int guard = 0; guard < 64 && pos + 8 <= fsz; guard++){
            if(!ad_read_at(f, pos, b, 8)) return;
            uint32_t len = be32(b + 4);
            if(!memcmp(b, "COMM", 4)){
                if(len < 18 || !ad_read_at(f, pos + 8, b + 8, len >= 22 ? 22 : 18)) return;
                m->channels = b[8] << 8 | b[9];
                m->bits = b[14] << 8 | b[15];
                m->sample_rate = aiff_rate(b + 16);
                const unsigned char *ct = b + 26;            /* AIFF-C compression type */
                int pcm = !aifc || len < 22 || !memcmp(ct, "NONE", 4) || !memcmp(ct, "sowt", 4) || !memcmp(ct, "twos", 4)
                       || !memcmp(ct, "in24", 4) || !memcmp(ct, "in32", 4) || !memcmp(ct, "fl32", 4) || !memcmp(ct, "fl64", 4);
                set_codec(m, pcm ? "pcm" : "aifc");
                return;
            }
            pos += 8 + (off_t)len + (len & 1);
        }
    } else if(has_ext(fname,".dsf")){
        set_codec(m, "dsd");
        if(ad_read_at(f, 0, b, 28 + 40) && !memcmp(b, "DSD ", 4) && !memcmp(b + 28, "fmt ", 4)){
            m->channels = (int)le32(b + 28 + 24);
            m->sample_rate = (int)le32(b + 28 + 28);
            m->bits = 1;
        }
    } else if(has_ext(fname,".dff")){
        set_codec(m, "dsd");
        m->bits = 1;
        if(!ad_read_at(f, 0, b, 16) || memcmp(b, "FRM8", 4) || memcmp(b + 12, "DSD ", 4)) return;
        off_t pos = 16;
        for(int guard = 0; guard < 32 && pos + 12 <= fsz; guard++){   /* FVER, PROP (SND: FS, CHNL, CMPR), DSD ... */
            if(!ad_read_at(f, pos, b, 12)) return;
            uint64_t len = (uint64_t)be32(b + 4) << 32 | be32(b + 8);
            if(len > (uint64_t)(fsz - pos - 12)) return;
            if(!memcmp(b, "PROP", 4)){
                off_t q = pos + 16, qe = pos + 12 + (off_t)len;   /* after "SND " */
                for(int g2 = 0; g2 < 32 && q + 12 <= qe; g2++){
                    if(!ad_read_at(f, q, b, 16)) return;
                    uint64_t sl = (uint64_t)be32(b + 4) << 32 | be32(b + 8);
                    if(sl > (uint64_t)(qe - q - 12)) return;
                    if(!memcmp(b, "FS  ", 4) && sl >= 4) m->sample_rate = (int)be32(b + 12);
                    else if(!memcmp(b, "CHNL", 4) && sl >= 2) m->channels = b[12] << 8 | b[13];
                    else if(!memcmp(b, "CMPR", 4) && sl >= 4 && !memcmp(b + 12, "DST ", 4)) snprintf(m->codec, sizeof m->codec, "dst");
                    q += 12 + (off_t)sl + (off_t)(sl & 1);
                }
                return;
            }
            pos += 12 + (off_t)len + (off_t)(len & 1);
        }
    } else if(has_ext(fname,".ogg") || has_ext(fname,".opus") || has_ext(fname,".oga")){
        if(!ad_read_at(f, 0, b, 27) || memcmp(b, "OggS", 4)) return;
        int ns = b[26];
        if(!ad_read_at(f, 27 + ns, b, 16)) return;
        if(!memcmp(b, "OpusHead", 8)){ set_codec(m, "opus"); m->channels = b[9]; m->sample_rate = 48000; }   /* Opus always decodes at 48 kHz */
        else if(b[0] == 1 && !memcmp(b + 1, "vorbis", 6)){ set_codec(m, "vorbis"); m->channels = b[11]; m->sample_rate = (int)le32(b + 12); }
        else if(!memcmp(b, "\x7f" "FLAC", 5)) set_codec(m, "flac");
    } else if(has_ext(fname,".ape")){
        set_codec(m, "ape");
        off_t o = ad_skip_id3(f, fsz);
        if(!ad_read_at(f, o, b, 76) || memcmp(b, "MAC ", 4)) return;
        unsigned ver = b[4] | b[5] << 8;
        if(ver >= 3980){                                     /* descriptor, then the header */
            uint32_t dbytes = le32(b + 8);
            unsigned char hd[24];
            if(dbytes < 8 || dbytes > 1024 || !ad_read_at(f, o + dbytes, hd, 24)) return;
            m->bits = hd[16] | hd[17] << 8; m->channels = hd[18] | hd[19] << 8; m->sample_rate = (int)le32(hd + 20);
        } else {
            unsigned flags = b[8] | b[9] << 8;
            m->channels = b[10] | b[11] << 8; m->sample_rate = (int)le32(b + 12);
            m->bits = (flags & 1) ? 8 : (flags & 8) ? 24 : 16;
        }
    } else if(has_ext(fname,".aac")){
        set_codec(m, "aac");
        static const int SR[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
        if(ad_read_at(f, ad_skip_id3(f, fsz), b, 4) && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0){
            int si = (b[2] >> 2) & 0xF;
            if(si < 13) m->sample_rate = SR[si];
            m->channels = ((b[2] & 1) << 2) | (b[3] >> 6);
        }
    } else if(has_ext(fname,".dts")) set_codec(m, "dts");
    else if(has_ext(fname,".wma")) set_codec(m, "wma");      /* wma_read read the format already */
    if(m->channels < 0 || m->channels > 64) m->channels = 0;
    if(m->sample_rate < 0 || m->sample_rate > 50000000) m->sample_rate = 0;
    if(m->bits < 0 || m->bits > 64) m->bits = 0;
}

/* Read tags + fill a filename fallback into *m. Return code drives how the caller writes the row:
 *   1  = tags read (or a no-tag container like WAV) -> safe to INSERT a new row OR UPDATE in place.
 *   0  = a tag-bearing file that could NOT be OPENED (transient I/O) -> preserve existing tags and
 *        do NOT insert a new one (a file we can't open at all is skipped).
 *  -1  = OPENED but its tags could not be LOADED (a mid-read I/O error, or oversized/hostile atoms):
 *        the title is the filename fallback. The caller must NOT overwrite an existing
 *        row's tags (never clobber good tags), but may INSERT a fallback row when the path is new so the file
 *        is still playable. */
static int tags_with_provenance(const char *path, const char *fname, scan_meta *m, long long *dur_ms){
    memset(m, 0, sizeof *m); *dur_ms=0;
    int rerr=0;
    FILE *f=fopen(path,"rb");
    if(!f && (has_ext(fname,".flac") || has_ext(fname,".mp3") || has_ext(fname,".m4a") || has_ext(fname,".m4b")
              || has_ext(fname,".ogg") || has_ext(fname,".opus") || has_ext(fname,".oga") || has_ext(fname,".ape") || has_ext(fname,".aif") || has_ext(fname,".aiff")
              || has_ext(fname,".wma") || has_ext(fname,".dsf") || has_ext(fname,".aac")))
        return 0;   /* unopened: preserve the entire existing row (incl. length), no new row */
    if(f){
        if(has_ext(fname,".flac")) flac_read(f,m,&rerr);
        else if(has_ext(fname,".mp3")){ id3v2_read(f,m,&rerr); id3v1_read(f,m,&rerr); }
        else if(has_ext(fname,".m4a") || has_ext(fname,".m4b")) mp4_read(f,m,&rerr);
        else if(has_ext(fname,".ogg") || has_ext(fname,".opus") || has_ext(fname,".oga")) ogg_read(f,m,&rerr);
        else if(has_ext(fname,".ape")){ ape_read(f,m,&rerr);
                                        if(!m->title[0] && fseek(f,0,SEEK_SET)==0) id3v1_read(f,m,&rerr); }
        else if(has_ext(fname,".aif") || has_ext(fname,".aiff")) aiff_read(f,m,&rerr);
        else if(has_ext(fname,".dsf")) dsf_read(f,m,&rerr);
        else if(has_ext(fname,".wma")) wma_read(f,m,&rerr);
        else if(has_ext(fname,".aac")) id3v2_read(f,m,&rerr);  /* ADTS may lead with ID3v2 */
        /* .wav/.dff/.dts (and any other accepted container) -> filename fallback below; no tag probing,
         * so a WAV whose last 128 bytes happen to start with "TAG" isn't misread as ID3v1. */
        if(ferror(f)) rerr=1;   /* ANY stream IO error during tag reads (headers/seeks/body) -> a read failure,
                                 * not "no tags" (feof/clean EOF does NOT set this, so a short valid file still
                                 * falls back to filename). Catches the header/seek paths rerr doesn't. */
        if(rerr==AD_CANCELLED || scan_cancelled(NULL)){
            fclose(f); return AD_CANCELLED;
        }
        *dur_ms = audio_duration_ms_cancel(f, fname, scan_cancelled, NULL);   /* AFTER the ferror check: its clearerr can't hide a tag read error;
                                                  * negative = read/resource/work failure (store unknown for SONG) */
        if(*dur_ms==AD_CANCELLED || scan_cancelled(NULL)){ fclose(f); return AD_CANCELLED; }
        fmt_probe(f, fname, m);                  /* best effort: a failure leaves the format unknown */
        fclose(f);
    }
    str_trim(m->album_artist);   /* whitespace-only album-artist -> empty, so it never overwrites a good value */
    if(m->title[0]) m->title_tag=1;
    if(!m->title[0]){                            /* filename (minus extension) */
        snprintf(m->title,TAGLEN,"%s",fname);
        char *dot=strrchr(m->title,'.'); if(dot) *dot=0;
        str_trim(m->title);
    }
    /* Opus ReplayGain (RFC 7845): R128 gains are Q7.8 dB against -23 LUFS; ReplayGain's reference is -18 LUFS */
    if(!(m->rg_flags & LDB_RG_TG) && (m->r128_flags & LDB_RG_TG)){ m->rg_tg = m->r128_tg / 256.0 + 5.0; m->rg_flags |= LDB_RG_TG; }
    if(!(m->rg_flags & LDB_RG_AG) && (m->r128_flags & LDB_RG_AG)){ m->rg_ag = m->r128_ag / 256.0 + 5.0; m->rg_flags |= LDB_RG_AG; }
    return rerr ? -1 : 1;   /* -1: fallback-only (preserve existing tags, insert-if-new); 1: normal upsert */
}
/* SONG's columns: the first value of each, with the fallbacks the stock UI shows */
static const char *song_artist(const scan_meta *m){ return m->n_artists ? m->artists[0] : "Unknown artist"; }
static const char *song_album(const scan_meta *m){ return m->album[0] ? m->album : "Unknown album"; }
static const char *song_genre(const scan_meta *m){ return m->n_genres ? m->genres[0] : "Unknown genre"; }


/* ---- SQLite: schema + insert ---- */
static const char *SCHEMA_SONG =
    "CREATE TABLE IF NOT EXISTS SONG (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, NAME TEXT,"
    "TITLE TEXT, ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT,"
    "IS_DSD INT, OFFSET BIGINT, DURATION BIGINT, NAME_CODE INT, TITLE_CODE INT, ALBUM_CODE INT,"
    "ARTIST_CODE INT, GENRE_CODE INT, ADD_TIME INT8, SAMPLE_RATE INT, BIT_PER_SAMPLE INT, CHANNELS INT,"
    "BIT_RATE INT, SONG_MIMETYPE TEXT, SONG_PRODUCTION_YEAR TEXT, IS_SELECT INT, ALBUM_ARTIST TEXT,"
    /* IS_M3U/M3U_PATH match the V2.28 stock SONG superset (its playback SQL SELECTs them); ACCENT is
     * our private per-song art-accent cache. TEXT for ALBUM_ARTIST matches stock (was wrongly INT). */
    "ALBUM_ARTIST_CODE INT, IS_M3U INT DEFAULT 0, M3U_PATH TEXT DEFAULT '', ACCENT INTEGER DEFAULT 0, TITLE_TAG INTEGER);";
static const char *INS_SONG =
    "INSERT INTO SONG (PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
    "DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,"
    "BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,"
    /* Explicit ?N params so the existing bind_tags indices are unchanged: 1=PATH, 2..6=tags, 7..11=codes,
     * 12=ADD_TIME; 13=DISC, 14=TRACK, 15=ALBUM_ARTIST (NULL when absent), 16=ALBUM_ARTIST_CODE. DISC/TRACK/
     * ALBUM_ARTIST were hardcoded 0/0/NULL before (the 2026-09-05 audit flagged the missing metadata).
     * IS_M3U/M3U_PATH/ACCENT are omitted so they take their schema DEFAULTs. 17=DURATION (ms, 0 = unknown). */
    "ALBUM_ARTIST_CODE,TITLE_TAG) VALUES (?1,?2,?3,?4,?5,?6,?13,?14,0,0,0,0,?17,?7,?8,?9,?10,?11,?12,0,0,0,0,'','',0,?15,?16,?18);";
/* MERGE (not delete+reinsert): UPDATE an existing PATH's metadata in place so its ID and ACCENT are
 * preserved - MEMORY_PLAY.MUSIC_ID (resume) and MY_LOVE.ID (favourites) are ID-keyed, so a delete+
 * reinsert with fresh autoincrement IDs used to break resume + favourites and wipe the art-accent cache
 * on every rescan. ADD_TIME is intentionally left untouched here (keep the original add time). */
static const char *UPD_SONG =
    /* Explicit ?N: 1..5=tags, 6..10=codes, 11=PATH (WHERE); 12=DISC, 13=TRACK, 14=ALBUM_ARTIST,
     * 15=ALBUM_ARTIST_CODE. Now re-populates disc/track/album-artist on every rescan too (was omitted,
     * so existing rows never gained the metadata). */
    "UPDATE SONG SET NAME=?1,TITLE=?2,ALBUM=?3,ARTIST=?4,GENRE=?5,"
    "NAME_CODE=?6,TITLE_CODE=?7,ALBUM_CODE=?8,ARTIST_CODE=?9,GENRE_CODE=?10,"
    /* Only FILL IN disc/track/album-artist we actually extracted; keep any existing (e.g. stock-written)
     * value when our read yields nothing, so a rescan never clobbers richer metadata with 0/NULL. */
    "DISC=CASE WHEN ?12>0 THEN ?12 ELSE DISC END,"
    "TRACK=CASE WHEN ?13>0 THEN ?13 ELSE TRACK END,"
    "ALBUM_ARTIST=CASE WHEN ?14 IS NOT NULL THEN ?14 ELSE ALBUM_ARTIST END,"
    "ALBUM_ARTIST_CODE=CASE WHEN ?14 IS NOT NULL THEN ?15 ELSE ALBUM_ARTIST_CODE END,"
    /* Store this opened file's length, including 0 on inspection failure; cancellation rolls back. */
    "DURATION=?16,TITLE_TAG=?17 "
    "WHERE PATH=?11;";
/* per-connection temp table of PATHs seen this scan; drives the post-walk delete of vanished songs. */
static const char *SEEN_DDL = "CREATE TEMP TABLE IF NOT EXISTS seen(PATH TEXT PRIMARY KEY);"
    "CREATE TEMP TABLE IF NOT EXISTS unreadable(PATH TEXT PRIMARY KEY); DELETE FROM unreadable;";   /* listed but unstat-able audio: a CUE it backs cannot be rewritten, so its rows must not be retired */
static const char *SEEN_INS = "INSERT OR IGNORE INTO seen(PATH) VALUES(?);";
/* Books whose tags were SUCCESSFULLY read this scan (tags_with_provenance rc==1). The BOOKS relocate refreshes an
 * existing book's metadata ONLY for these paths, so a later good read fixes stale/fallback tags while a read
 * FAILURE (filename fallback) never clobbers previously-good metadata. */
static const char *BOOKOK_DDL = "CREATE TEMP TABLE IF NOT EXISTS book_ok(PATH TEXT PRIMARY KEY);";
static const char *BOOKOK_INS = "INSERT OR IGNORE INTO book_ok(PATH) VALUES(?);";
/* Books whose LENGTH was read without an I/O error this scan (0 = read fine, length unknown). Only these may replace a
 * book's stored length; a read failure keeps the old one. */
static const char *DUROK_DDL = "CREATE TEMP TABLE IF NOT EXISTS book_dur_ok(PATH TEXT PRIMARY KEY);";
static const char *DUROK_INS = "INSERT OR IGNORE INTO book_dur_ok(PATH) VALUES(?);";

static sqlite3 *g_db;
static sqlite3_stmt *g_ins, *g_upd, *g_seen, *g_cuechk, *g_bookok, *g_durok;
/* L39: a CUE/ISO backing file (e.g. one .flac holding many virtual tracks) has DB rows that share its
 * PATH but carry IS_CUE=1/IS_ISO=1 + per-track TITLE/TRACK/OFFSET. A rescan must NOT overwrite those
 * (UPDATE ... WHERE PATH=? would hit them) nor add a duplicate plain row - so we skip such a path. */
static const char *CUE_CHECK =
    "SELECT 1 FROM SONG WHERE PATH=?1 AND (COALESCE(IS_CUE,0)=1 OR COALESCE(IS_ISO,0)=1) LIMIT 1;";

/* True iff SONG has column `col`. Used to PROVE the V2.28-required columns really exist after the
 * migration ALTERs before we rebuild the library - a silently-failed ALTER (SQLITE_BUSY/FULL/IOERR)
 * must not produce a library the stock V2.28 player can't query. PRAGMA table_info is universally
 * supported (no dependency on the pragma-function feature). */
static int song_has_col(sqlite3 *db, const char *col){
    sqlite3_stmt *st = NULL; int found = 0;
    if(sqlite3_prepare_v2(db, "PRAGMA table_info(SONG);", -1, &st, NULL) == SQLITE_OK){
        while(sqlite3_step(st) == SQLITE_ROW){
            const unsigned char *n = sqlite3_column_text(st, 1);   /* col 1 = name */
            if(n && strcmp((const char*)n, col) == 0){ found = 1; break; }
        }
        sqlite3_finalize(st);
    }
    return found;
}

/* bind the 5 text tags + their 5 codes (fname/title/album/artist/genre) to a prepared stmt starting at
 * parameter index `p0` (used for both the UPDATE's SET list and the INSERT's value list). */
static void bind_tags(sqlite3_stmt *s, int p0, const char *fname, const char *title,
                      const char *album, const char *artist, const char *genre){
    sqlite3_bind_text(s, p0+0, fname, -1, SQLITE_TRANSIENT);   /* NAME = filename */
    sqlite3_bind_text(s, p0+1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+2, album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+3, artist,-1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+4, genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, p0+5, code4(fname));
    sqlite3_bind_int (s, p0+6, code4(title));
    sqlite3_bind_int (s, p0+7, code4(album));
    sqlite3_bind_int (s, p0+8, code4(artist));
    sqlite3_bind_int (s, p0+9, code4(genre));
}
/* bind DISC, TRACK, ALBUM_ARTIST, ALBUM_ARTIST_CODE at params d0..d0+3. Album-artist absent -> NULL. */
static void bind_meta(sqlite3_stmt *s, int d0, int disc, int track, const char *aa){
    sqlite3_bind_int(s, d0+0, disc);
    sqlite3_bind_int(s, d0+1, track);
    if(aa && aa[0]){ sqlite3_bind_text(s, d0+2, aa, -1, SQLITE_TRANSIENT); sqlite3_bind_int(s, d0+3, code4(aa)); }
    else          { sqlite3_bind_null(s, d0+2);                            sqlite3_bind_int(s, d0+3, 0); }
}
/* Record a path in `seen` WITHOUT touching its row - used to PRESERVE an existing row for a file we
 * couldn't index this pass (ENOENT / unreadable), so the vanished-row prune doesn't delete it. If we
 * can't even record it, block the prune (conservative: never delete a file we failed to preserve). */
static void mark_seen(const char *path){
    if(!g_seen){ g_prune_blocked = 1; return; }
    sqlite3_reset(g_seen); sqlite3_clear_bindings(g_seen);
    sqlite3_bind_text(g_seen, 1, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_seen) != SQLITE_DONE) g_prune_blocked = 1;
}
/* ---- library.db (librarydb.c) and the scan report ---- */
static ldb_writer *g_ldb;               /* this scan's library.db writer; NULL when library.db isn't written */
static char g_ldb_why[200];             /* why not, for the scan report */
typedef struct { char arts[MAXV][LDB_NAME], sorts[MAXV][LDB_NAME], genres[MAXV][LDB_NAME]; } ldb_lists;
static ldb_lists *g_lists;              /* scratch for one song's split names (heap, like g_meta) */
static long long g_song_size0;          /* song.db's size when the scan began */
static int g_song_done;                 /* SONG committed: its journal is gone */
/* /usr/data that library.db.new must leave free right now: song.db's journal still to come (it can grow to song.db's
 * size, "-journal" or "-wal") and 16 MB for song.db itself to grow (about 30,000 new songs) */
#ifndef SONG_GROWTH_ROOM
#define SONG_GROWTH_ROOM (16LL << 20)
#endif
static long long ldb_reserve(void){
    long long r = SONG_GROWTH_ROOM;
    if(!g_song_done){
        struct stat js;
        long long j = 0;
        if(stat(DB_PATH "-journal", &js) == 0) j += (long long)js.st_size;
        if(stat(DB_PATH "-wal", &js) == 0) j += (long long)js.st_size;
        if(g_song_size0 > j) r += g_song_size0 - j;
    }
    return r;
}
_Static_assert(TAGLEN == LDB_NAME, "library.db names are the scanner's tag values");

/* The scan report: short, capped lists, written to the SD card once the walk has read the whole card. */
enum { REP_ENC, REP_SPLIT, REP_NOMEDIA, REP_KEPT, REP_HIDDEN, REP_N };
#define REP_MAX 500                     /* lines per list; the rest are only counted */
typedef struct { char *buf; size_t len, cap; int n, more; } rep_list;
static rep_list g_rep[REP_N];
static const char *rel_path(const char *p){
    size_t n = strlen(SCAN_ROOT);
    return !strncmp(p, SCAN_ROOT, n) && p[n] == '/' ? p + n + 1 : p;
}
static void rep_add(int which, const char *fmt, ...){
    rep_list *l = &g_rep[which];
    if(l->n >= REP_MAX){ l->more++; return; }
    char line[1700];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if(n < 0) return;
    if(n >= (int)sizeof line){ u8_fix_tail(line); n = (int)strlen(line); }   /* cut: never half a character */
    if(l->len + (size_t)n + 2 > l->cap){
        size_t nc = l->cap ? l->cap * 2 : 8192;
        while(nc < l->len + (size_t)n + 2) nc *= 2;
        void *p = realloc(l->buf, nc);
        if(!p){ l->more++; return; }
        l->buf = p; l->cap = nc;
    }
    memcpy(l->buf + l->len, line, (size_t)n); l->len += (size_t)n;
    l->buf[l->len++] = '\n'; l->buf[l->len] = 0;
    l->n++;
}
static char g_split_last[2 * TAGLEN];   /* the last credit listed as split: an album's songs repeat it, list it once */
static void rep_reset(void){
    for(int i = 0; i < REP_N; i++){ free(g_rep[i].buf); memset(&g_rep[i], 0, sizeof g_rep[i]); }
    g_split_last[0] = 0;
}
/* a file's text that isn't Unicode, or is broken */
static void rep_meta(const char *path, const scan_meta *m){
    if(!m->n_enc) return;
    char b[1600]; int n = snprintf(b, sizeof b, "  %s", rel_path(path));
    for(int i = 0; i < m->n_enc && n > 0 && n < (int)sizeof b; i++)
        n += snprintf(b + n, sizeof b - (size_t)n, "\n      %s: \"%s\" (%s)", m->enc[i].tag, m->enc[i].value,
                      m->enc[i].kind == ENC_LATIN1 ? "Latin-1, not Unicode" : "broken text");
    u8_fix_tail(b);
    rep_add(REP_ENC, "%s", b);
}
/* Songs this scan can't read keep their rows from the previous library.db (librarydb.c, ldb_keep). */
static void ldb_keep_note(const char *path, int kind, const char *shown, const char *why){
    if(!g_ldb || ldb_failed(g_ldb) || ldb_in_nomedia(g_ldb, path)) return;
    ldb_keep(g_ldb, path, kind);
    rep_add(REP_KEPT, "  %s (%s)", rel_path(shown ? shown : path), why);
}
/* The song's artists for library.db: Picard's ARTISTS when it lists several (ARTIST is then a display credit; a single
 * ARTISTS value may be Picard's ID3v2.3 "A/B" join, so it doesn't win), else every ARTIST value, split on ';' and
 * "feat.". Their sort names are used when they line up one to one. Returns the count. */
static int meta_artists(const scan_meta *m, const char *path, ldb_lists *L, int *has_sorts){
    const char *v[MAXV] = { 0 };
    int pic = m->n_artists_pic > 1;
    const char (*src)[TAGLEN] = pic ? m->artists_pic : m->artists;
    int nv = pic ? m->n_artists_pic : m->n_artists;
    for(int i = 0; i < nv; i++) v[i] = src[i];
    int n = ldb_split_names(v, nv, 1, L->arts, MAXV);
    if(path && n > nv){                                   /* one value became several names: the report lists it */
        char was[2 * TAGLEN] = "", got[2 * TAGLEN] = "";
        for(int i = 0; i < nv; i++){ size_t l = strlen(was); snprintf(was + l, sizeof was - l, "%s\"%s\"", i ? ", " : "", v[i]); }
        for(int i = 0; i < n; i++){ size_t l = strlen(got); snprintf(got + l, sizeof got - l, "%s%s", i ? " | " : "", L->arts[i]); }
        u8_fix_tail(was); u8_fix_tail(got);
        if(strcmp(g_split_last, was)){ snprintf(g_split_last, sizeof g_split_last, "%s", was); rep_add(REP_SPLIT, "  %s -> %s   (%s)", was, got, rel_path(path)); }
    }
    *has_sorts = 0;
    if(m->n_artist_sorts && n){
        const char *sv[MAXV] = { 0 };
        for(int i = 0; i < m->n_artist_sorts; i++) sv[i] = m->artist_sorts[i];
        *has_sorts = ldb_split_names(sv, m->n_artist_sorts, 1, L->sorts, MAXV) == n;
    }
    return n;
}
/* ALBUMARTIST; with none, "Various Artists" for a compilation, else the song's first artist ("" = unknown) */
static const char *meta_album_artist(const scan_meta *m, const ldb_lists *L, int na){
    if(m->album_artist[0]) return m->album_artist;
    if(m->compilation) return "Various Artists";
    return na ? L->arts[0] : "";
}
/* Everything of a library.db song that comes from the file's own tags; the caller sets path, slice and source. */
static void ldb_fill(ldb_track *t, const scan_meta *m, const ldb_lists *L, int na, int has_sorts, int ng){
    memset(t, 0, sizeof *t);
    t->album_title = m->album[0] ? m->album : NULL;
    t->album_artist = meta_album_artist(m, L, na);
    t->title = m->title; t->title_sort = m->title_sort;
    t->artists = (const char (*)[LDB_NAME])L->arts; t->n_artists = na;
    t->artist_sorts = has_sorts ? (const char (*)[LDB_NAME])L->sorts : NULL;
    t->genres = (const char (*)[LDB_NAME])L->genres; t->n_genres = ng;
    t->disc = m->disc; t->track = m->track; t->year = m->year_date ? m->year_date : m->year_year;
    t->bpm = m->bpm; t->initial_key = m->initial_key;
    t->rg_flags = m->rg_flags; t->rg_track_gain = m->rg_tg; t->rg_track_peak = m->rg_tp;
    t->rg_album_gain = m->rg_ag; t->rg_album_peak = m->rg_ap;
    t->sample_rate = m->sample_rate; t->bits = m->bits; t->channels = m->channels; t->codec = m->codec;
    t->has_lyrics = m->has_lyrics;
    t->track_total = m->track_total; t->disc_total = m->disc_total; t->compilation = m->compilation;
    t->release_type = m->release_type; t->album_artist_sort = m->album_artist_sort; t->album_sort = m->album_sort;
    t->end_ms = -1;
}
static int meta_genres(const scan_meta *m, ldb_lists *L){
    const char *v[MAXV] = { 0 };
    for(int i = 0; i < m->n_genres; i++) v[i] = m->genres[i];
    return ldb_split_names(v, m->n_genres, 0, L->genres, MAXV);
}
/* One plain file into library.db. tst < 0 (tags unreadable): its row from the previous scan wins when there is one. */
static void ldb_file(const char *path, const char *fname, const struct stat *st, const scan_meta *m, long long dur, int tst){
    if(!g_ldb || ldb_failed(g_ldb) || has_ext(fname, ".m4b") || ldb_in_nomedia(g_ldb, path)) return;   /* books aren't music */
    rep_meta(path, m);
    if(tst < 0) ldb_keep_note(path, LDB_KEEP_PATH, NULL, "tags unreadable");
    int hs, na = meta_artists(m, path, g_lists, &hs), ng = meta_genres(m, g_lists);
    ldb_track t;
    ldb_fill(&t, m, g_lists, na, hs, ng);
    t.path = path; t.source = LDB_SRC_FILE;
    t.duration_ms = dur > 0 ? dur : 0;
    t.file_mtime = (long long)st->st_mtime; t.file_size = (long long)st->st_size;
    ldb_add(g_ldb, &t);
}
/* Merge one file into SONG: record it as seen, then UPDATE the existing PATH in place (preserving ID +
 * ACCENT), or INSERT a new row if the PATH is new. Any DB error sets g_scan_err (blocks the commit). to_ldb: also
 * write it to library.db (the walk; not the CUE clean-up, which re-adds rows library.db already has). */
static void upsert_song(const char *path, const char *fname, const struct stat *fst, int to_ldb){
    if(g_scan_err) return;   /* an earlier write failed -> don't add writes to a possibly rolled-back txn */
    scan_meta *m = g_meta;
    long long dur_ms=0;
    int tst = tags_with_provenance(path, fname, m, &dur_ms);
    if(tst==AD_CANCELLED || atomic_load(&g_abort)){ g_scan_err=1; return; }
    if(tst == 0){
        mark_seen(path);   /* couldn't open this pass -> preserve the entire existing row, no insert */
        g_skipped++;
        if(to_ldb) ldb_keep_note(path, LDB_KEEP_PATH, NULL, "couldn't open");
        return;
    }
    if(to_ldb) ldb_file(path, fname, fst, m, dur_ms, tst);
    /* record as seen (drives the post-walk delete of paths that vanished from the SD) */
    sqlite3_reset(g_seen); sqlite3_clear_bindings(g_seen);
    sqlite3_bind_text(g_seen, 1, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_seen)!=SQLITE_DONE){ g_scan_err=1; return; }
    /* Track books we read REAL tags for this scan (rc==1), so the post-walk relocate can refresh their
     * BOOKS metadata without a fallback read ever clobbering previously-good tags. */
    if(tst == 1 && has_ext(fname, ".m4b") && g_bookok){
        sqlite3_reset(g_bookok); sqlite3_clear_bindings(g_bookok);
        sqlite3_bind_text(g_bookok, 1, path, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(g_bookok)!=SQLITE_DONE){ g_scan_err=1; return; }
    }
    if(dur_ms >= 0 && has_ext(fname, ".m4b") && g_durok){
        sqlite3_reset(g_durok); sqlite3_clear_bindings(g_durok);
        sqlite3_bind_text(g_durok, 1, path, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(g_durok)!=SQLITE_DONE){ g_scan_err=1; return; }
    }
    /* An opened SONG file replaces its previous length even on inspection failure.
     * Keep the separate BOOKS read-error policy above; cancellation has already returned. */
    if(dur_ms < 0) dur_ms = 0;
    /* L39: if this path already backs stock CUE/ISO virtual tracks, leave them entirely alone (marked
     * seen above so they survive the prune) - do NOT overwrite their per-track metadata via UPDATE, and
     * do NOT insert a duplicate plain row. */
    sqlite3_reset(g_cuechk); sqlite3_clear_bindings(g_cuechk);
    if(sqlite3_bind_text(g_cuechk, 1, path, -1, SQLITE_TRANSIENT)!=SQLITE_OK){ g_scan_err=1; return; }
    int cchk = sqlite3_step(g_cuechk);
    if(cchk==SQLITE_ROW){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); return; }  /* CUE/ISO backing -> preserve */
    if(cchk!=SQLITE_DONE){ g_scan_err=1; return; }   /* query error -> fail closed, never risk the UPDATE on unknown state */
    if(tst < 0){
        /* File opened but tags could not be loaded (oversized/hostile atoms, mid-read IO).
         * The filename fallback is filled:
         * NEVER overwrite an existing row's good tags. Insert a filename-fallback row only when the path
         * is new, so a new file (e.g. an m4a with an oversized embedded cover) is still indexed + playable. */
        sqlite3_stmt *chk = NULL; int exists = 0;
        if(sqlite3_prepare_v2(g_db, "SELECT 1 FROM SONG WHERE PATH=?1 LIMIT 1;", -1, &chk, NULL) != SQLITE_OK){ g_scan_err=1; return; }
        if(sqlite3_bind_text(chk, 1, path, -1, SQLITE_TRANSIENT) != SQLITE_OK){ sqlite3_finalize(chk); g_scan_err=1; return; }
        int erc = sqlite3_step(chk);
        sqlite3_finalize(chk);
        if(erc == SQLITE_ROW) exists = 1;
        else if(erc != SQLITE_DONE){ g_scan_err=1; return; }   /* query error -> fail closed; never risk a duplicate INSERT on unknown state */
        if(exists){
            /* Preserve its tags while replacing the length with this scan's value (0 on failure). */
            sqlite3_stmt *du = NULL;
            if(sqlite3_prepare_v2(g_db, "UPDATE SONG SET DURATION=?2 WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 "
                                        "AND COALESCE(IS_ISO,0)=0;", -1, &du, NULL) != SQLITE_OK){ g_scan_err=1; return; }
            sqlite3_bind_text(du, 1, path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(du, 2, (sqlite3_int64)dur_ms);
            int urc = sqlite3_step(du);
            sqlite3_finalize(du);
            if(urc != SQLITE_DONE){ g_scan_err=1; return; }
            pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);
            return;   /* preserve the existing tags */
        }
        /* new path -> fall through to the shared INSERT below with the filename fallback */
    } else {
        /* tst == 1: UPDATE in place (keeps ID/ACCENT/ADD_TIME). params 1..10 = tags, 11 = PATH (WHERE). */
        sqlite3_reset(g_upd); sqlite3_clear_bindings(g_upd);
        bind_tags(g_upd, 1, fname, m->title, song_album(m), song_artist(m), song_genre(m));
        sqlite3_bind_text(g_upd, 11, path, -1, SQLITE_TRANSIENT);
        bind_meta(g_upd, 12, m->disc, m->track, m->album_artist);
        sqlite3_bind_int64(g_upd, 16, (sqlite3_int64)dur_ms);
        sqlite3_bind_int(g_upd,17,m->title_tag);
        if(sqlite3_step(g_upd)!=SQLITE_DONE){ g_scan_err=1; return; }
        if(sqlite3_changes(g_db) > 0){                     /* existing PATH updated in place */
            pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    /* new PATH -> INSERT (new autoincrement ID, ACCENT = schema default 0). params: 1=PATH, 2..11 tags, 12=ADD_TIME */
    sqlite3_reset(g_ins); sqlite3_clear_bindings(g_ins);
    sqlite3_bind_text(g_ins, 1, path, -1, SQLITE_TRANSIENT);
    bind_tags(g_ins, 2, fname, m->title, song_album(m), song_artist(m), song_genre(m));
    sqlite3_bind_int64(g_ins, 12, (sqlite3_int64)time(NULL));   /* ADD_TIME = now (new rows only; UPDATE keeps the original) */
    bind_meta(g_ins, 13, m->disc, m->track, m->album_artist);
    sqlite3_bind_int64(g_ins, 17, (sqlite3_int64)dur_ms);   /* normalized above: failures are unknown */
    if(tst<0) sqlite3_bind_null(g_ins,18);
    else sqlite3_bind_int(g_ins,18,m->title_tag);
    if(sqlite3_step(g_ins)==SQLITE_DONE){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); }
    else g_scan_err=1;   /* insert failure (disk full, DB corruption) must block the commit */
}

/* ---- CUE sheets (L16) ----
 * Stock V2.57's scanner turns an EXTERNAL .cue sheet into one SONG row per track (add_cue_song, P257 0x443160;
 * internal RE notes) and drops the whole-file row of the audio it describes; the player then plays a row
 * from OFFSET for DURATION (both ms). diskOS writes the same rows: PATH = the audio file the sheet names, TRACK = the
 * sheet's track number, IS_CUE=1, DISC=0x7fffffff, ALBUM_ARTIST NULL; TITLE / PERFORMER / REM GENRE from the sheet,
 * else the audio file's own tags. Deliberate differences from stock: frames are 1/75 s (stock reads them as 10 ms),
 * NAME is the file name without its extension (stock cuts 4 characters), and sort codes use diskOS's code4 like
 * every other row. A sheet is skipped - the file keeps its ordinary row - when it names no usable file, has a track
 * without INDEX 01, offsets that don't increase or run past the file, or when the file's length is unknown (the last
 * track's length is file length - its offset). Embedded sheets are not read (stock keeps the ordinary row there too).
 * Sheets are processed after the walk, inside the scan transaction; rows keep their IDs across rescans (matched on
 * PATH + TRACK), and the file's ordinary row BECOMES the first track (same ID: resume and favourites keep pointing at
 * it). Files diskOS expanded are listed in DISKOS_CUE_FILES: when no sheet writes such a file any more (deleted, or now
 * unusable) its first track turns back into the ordinary row and the other tracks go. CUE rows stock wrote are left
 * alone, with one exception: when a sheet diskOS reads describes a file whose only rows are stock CUE rows (stock's
 * external-sheet shape), that sheet is the source of truth - its tracks update the stock rows in place (same IDs),
 * tracks it no longer lists go, and the file becomes diskOS's. A file with stock CUE rows BESIDE its ordinary row
 * (stock's embedded-sheet shape) is never touched by an external sheet. A sheet may only name a file the walk itself indexed (no "..", no absolute path; "." and empty parts
 * are dropped), so symlinks and paths outside the card are never reached. A sheet that cannot be READ (I/O error)
 * blocks this cleanup and the prune, like any unreadable entry. Sheets for .m4b audiobooks and sheets with non-AUDIO
 * tracks are ignored. Work is bounded: 256 KB per sheet, 32 MB of sheets per scan. */
#define CUE_MAX_BYTES  (256 * 1024)
#define CUE_MAX_TRACKS 99
#define CUE_MAX_FILES  64
#define CUE_MAX_SHEETS 4096
#define CUE_MAX_TOTAL  (32LL * 1024 * 1024)   /* sheet bytes read per scan; the rest wait for the next one */
static long long g_cue_bytes;
static sqlite3_stmt *g_cue_up, *g_cue_in, *g_cue_seen;   /* prepared once per scan (cue_finish) */
typedef struct { int num; long long ms; char title[TAGLEN], perf[TAGLEN]; int rg_flags; double rg_tg, rg_tp; } cue_track_t;
typedef struct { char name[MAXPATH]; int first, ntr; } cue_file_t;   /* its tracks: tr[first .. first+ntr) */
typedef struct { char title[TAGLEN], perf[TAGLEN], genre[TAGLEN], date[32]; int nfiles, ntr, bad, rg_flags; double rg_ag, rg_ap;
                 cue_file_t f[CUE_MAX_FILES]; cue_track_t tr[CUE_MAX_TRACKS]; } cue_sheet_t;   /* ~120 KB, one per scan */
static char (*g_cues)[MAXPATH];   /* .cue paths seen by the walk (processed after it) */
static int g_ncues, g_cues_cap;

/* library.db: a sheet not read this scan keeps the CUE songs it may own (those under its folder no other sheet wrote) */
static void cue_keep(const char *cue_path, const char *why){
    char dir[MAXPATH];
    snprintf(dir, sizeof dir, "%s", cue_path);
    char *sl = strrchr(dir, '/');
    if(!sl) return;
    sl[1] = 0;
    ldb_keep_note(dir, LDB_KEEP_CUE_TREE, cue_path, why);
}
static void cue_note(const char *path){
    if(g_ncues >= CUE_MAX_SHEETS){ g_prune_blocked = 1; cue_keep(path, "too many CUE sheets"); return; }   /* can't track it -> never delete rows it may own */
    if(g_ncues == g_cues_cap){
        int nc = g_cues_cap ? g_cues_cap * 2 : 64;
        if(nc > CUE_MAX_SHEETS) nc = CUE_MAX_SHEETS;
        void *p = realloc(g_cues, (size_t)nc * sizeof *g_cues);
        if(!p){ g_prune_blocked = 1; cue_keep(path, "out of memory"); return; }
        g_cues = p; g_cues_cap = nc;
    }
    snprintf(g_cues[g_ncues++], MAXPATH, "%s", path);
}
/* 1 if s[0..n) is well-formed UTF-8 (no overlongs/surrogates) */
static int utf8_ok(const unsigned char *s, size_t n){
    for(size_t i = 0; i < n; ){
        unsigned c = s[i];
        if(c < 0x80){ i++; continue; }
        int k = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
        if(k < 0 || i + (size_t)k >= n) return 0;          /* the continuation bytes must all be inside s */
        unsigned cp = c & (0x3F >> k);
        for(int j = 1; j <= k; j++){ if((s[i + j] & 0xC0) != 0x80) return 0; cp = (cp << 6) | (s[i + j] & 0x3F); }
        if((k == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
        i += (size_t)k + 1;
    }
    return 1;
}
/* a sheet value: "quoted" (to the closing quote) or the rest of the word run, trimmed; UTF-8 in */
static void cue_value(const char *p, char *out, int cap){
    while(*p == ' ' || *p == '\t') p++;
    int n = 0;
    if(*p == '"'){ p++; while(*p && *p != '"' && n < cap - 1) out[n++] = *p++; }
    else { while(*p && n < cap - 1) out[n++] = *p++; }
    out[n] = 0;
    u8_fix_tail(out);   /* a long name cut at cap: whole characters only (the sheet is UTF-8 by now) */
    str_trim(out);
}
/* "mm:ss:ff" (ff = 1/75 s frames) -> ms, -1 if malformed */
static long long cue_time(const char *p){
    while(*p == ' ' || *p == '\t') p++;
    long long v[3] = {0, 0, 0};
    for(int k = 0; k < 3; k++){
        int nd = 0;
        while(*p >= '0' && *p <= '9' && nd < 6){ v[k] = v[k] * 10 + (*p - '0'); p++; nd++; }
        if(nd == 0) return -1;
        if(k < 2){ if(*p != ':') return -1; p++; }
    }
    if(v[1] > 59 || v[2] > 74) return -1;
    return (v[0] * 60 + v[1]) * 1000 + v[2] * 1000 / 75;
}
static int kw(const char *p, const char *k){ size_t n = strlen(k); return !strncasecmp(p, k, n) && (p[n] == ' ' || p[n] == '\t'); }
/* "REM REPLAYGAIN_ALBUM_GAIN -7.03 dB" before the tracks, REPLAYGAIN_TRACK_* inside one (library.db) */
static void cue_rem_rg(const char *q, cue_track_t *t, cue_sheet_t *cs){
    static const struct { const char *k; int flag; } R[] = {
        { "REPLAYGAIN_ALBUM_GAIN", LDB_RG_AG }, { "REPLAYGAIN_ALBUM_PEAK", LDB_RG_AP },
        { "REPLAYGAIN_TRACK_GAIN", LDB_RG_TG }, { "REPLAYGAIN_TRACK_PEAK", LDB_RG_TP } };
    for(unsigned i = 0; i < sizeof R / sizeof R[0]; i++){
        if(!kw(q, R[i].k)) continue;
        int gain = R[i].flag & (LDB_RG_TG | LDB_RG_AG), track = R[i].flag & (LDB_RG_TG | LDB_RG_TP);
        char v[32]; double d;
        cue_value(q + strlen(R[i].k), v, sizeof v);
        if(!parse_real(v, gain ? -64 : 0, gain ? 64 : 16, &d)) return;
        if(track && t){ if(gain) t->rg_tg = d; else t->rg_tp = d; t->rg_flags |= R[i].flag; }
        else if(!track && !t){ if(gain) cs->rg_ag = d; else cs->rg_ap = d; cs->rg_flags |= R[i].flag; }
        return;
    }
}
/* parse UTF-8 sheet text into *cs (bounded); cs->bad = 1 on a structural error */
static void cue_parse(char *text, cue_sheet_t *cs){
    memset(cs, 0, sizeof *cs);
    cue_file_t *f = NULL; cue_track_t *t = NULL;
    for(char *line = text, *next; line && *line; line = next){
        next = strpbrk(line, "\r\n");
        if(next){ *next++ = 0; while(*next == '\r' || *next == '\n') next++; }
        char *p = line; while(*p == ' ' || *p == '\t') p++;
        if(kw(p, "FILE")){
            if(cs->nfiles >= CUE_MAX_FILES){ cs->bad = 1; return; }
            f = &cs->f[cs->nfiles++]; t = NULL; f->first = cs->ntr; f->ntr = 0;
            char v[MAXPATH]; cue_value(p + 4, v, sizeof v);
            if(strchr(p + 4, '"') == NULL){                /* unquoted: FILE name TYPE -> drop the type word */
                char *sp = strrchr(v, ' '); if(sp) *sp = 0;
            }
            snprintf(f->name, sizeof f->name, "%s", v);
        } else if(kw(p, "TRACK")){
            if(!f || cs->ntr >= CUE_MAX_TRACKS){ cs->bad = 1; return; }
            t = &cs->tr[cs->ntr++]; f->ntr++;
            char *q = p + 5; while(*q == ' ' || *q == '\t') q++;
            t->num = atoi(q); t->ms = -1;
            if(t->num < 1 || t->num > CUE_MAX_TRACKS){ cs->bad = 1; return; }
            while(*q >= '0' && *q <= '9') q++;
            while(*q == ' ' || *q == '\t') q++;
            if(strncasecmp(q, "AUDIO", 5) != 0){ cs->bad = 1; return; }   /* a data track (mixed-mode disc): not handled */
        } else if(kw(p, "INDEX")){
            char *q = p + 5; while(*q == ' ' || *q == '\t') q++;
            if(t && atoi(q) == 1){ while(*q >= '0' && *q <= '9') q++; t->ms = cue_time(q); }
        } else if(kw(p, "TITLE")){
            cue_value(p + 5, t ? t->title : cs->title, TAGLEN);
        } else if(kw(p, "PERFORMER")){
            cue_value(p + 9, t ? t->perf : cs->perf, TAGLEN);
        } else if(kw(p, "REM")){
            char *q = p + 3; while(*q == ' ' || *q == '\t') q++;
            if(!t && kw(q, "GENRE")) cue_value(q + 5, cs->genre, TAGLEN);
            else if(!t && kw(q, "DATE")) cue_value(q + 4, cs->date, sizeof cs->date);
            else cue_rem_rg(q, t, cs);
        }
    }
}
/* 1 if the walk indexed exactly this path this scan (in `seen`) */
static int cue_seen(const char *path){
    sqlite3_reset(g_cue_seen); sqlite3_clear_bindings(g_cue_seen);
    sqlite3_bind_text(g_cue_seen, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(g_cue_seen);
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) g_scan_err = 1;
    return rc == SQLITE_ROW;
}
/* the audio file a FILE line names, relative to the sheet's folder: no absolute path, no ".." part; "." and empty parts
 * are dropped so the path matches the walk's spelling, and the walk must have indexed it. When it isn't there, the same
 * name with another audio extension (a sheet written for a .wav that was converted to .flac). */
static int cue_backing_ex(const char *cue_path, const char *name, char *out, int cap, int (*seen)(const char *)){
    if(!name[0] || name[0] == '/' || name[0] == '\\') return 0;
    char dir[MAXPATH]; snprintf(dir, sizeof dir, "%s", cue_path);
    char *sl = strrchr(dir, '/'); if(!sl) return 0; *sl = 0;
    char rel[MAXPATH]; int rl = 0;
    for(const char *p = name; *p; ){
        const char *e = p; while(*e && *e != '/' && *e != '\\') e++;
        size_t n = (size_t)(e - p);
        if(n == 2 && p[0] == '.' && p[1] == '.') return 0;
        if(n && !(n == 1 && p[0] == '.')){
            if(rl + (int)n + 2 >= (int)sizeof rel) return 0;
            if(rl) rel[rl++] = '/';
            memcpy(rel + rl, p, n); rl += (int)n;
        }
        p = *e ? e + 1 : e;
    }
    rel[rl] = 0;
    if(!rl) return 0;
    if(snprintf(out, cap, "%s/%s", dir, rel) < cap && is_audio(out) && seen(out)) return 1;
    static const char *const EXT[] = { ".flac", ".wav", ".ape", ".m4a", ".mp3", ".wma", ".ogg", ".aiff", ".aif", ".dsf" };
    char *dot = strrchr(rel, '.'); char *rs = strrchr(rel, '/');
    if(!dot || (rs && dot < rs)) return 0;
    *dot = 0;
    for(unsigned i = 0; i < sizeof EXT / sizeof EXT[0]; i++)
        if(snprintf(out, cap, "%s/%s%s", dir, rel, EXT[i]) < cap && is_audio(out) && seen(out)) return 1;
    return 0;
}
static int cue_backing(const char *cue_path, const char *name, char *out, int cap){ return cue_backing_ex(cue_path, name, out, cap, cue_seen); }
static const char *CUE_UPD =
    "UPDATE SONG SET NAME=?3,TITLE=?4,ALBUM=?5,ARTIST=?6,GENRE=?7,OFFSET=?8,DURATION=?9,NAME_CODE=?10,TITLE_CODE=?11,"
    "ALBUM_CODE=?12,ARTIST_CODE=?13,GENRE_CODE=?14 WHERE PATH=?1 AND TRACK=?2 AND COALESCE(IS_CUE,0)=1;";
static const char *CUE_INS =
    "INSERT INTO SONG (PATH,TRACK,NAME,TITLE,ALBUM,ARTIST,GENRE,OFFSET,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,"
    "GENRE_CODE,ADD_TIME,DISC,IS_CUE,IS_ISO,IS_DSD,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,"
    "SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,"
    "?15,2147483647,1,0,0,0,0,0,0,NULL,NULL,0,NULL,2147483647);";
static int db_exec_path(const char *sql, const char *path, const char *extra){
    sqlite3_stmt *s = NULL;
    if(sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    if(extra) sqlite3_bind_text(s, 2, extra, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}
/* library.db: the sheet's tracks for one FILE replace the file's whole-file row. Album and album artist come from the
 * sheet (TITLE/PERFORMER before the tracks), else from the file's own tags, the same for every track of the sheet. */
static void cue_ldb(const cue_sheet_t *cs, const cue_file_t *f, const char *audio, const char *name, const scan_meta *m, long long dur){
    struct stat st;
    if(!g_ldb || ldb_failed(g_ldb) || ldb_in_nomedia(g_ldb, audio) || lstat(audio, &st) != 0) return;
    ldb_drop_path(g_ldb, audio);
    ldb_lists *L = g_lists;
    const cue_track_t *tr = cs->tr + f->first;
    int hs = 0, na = meta_artists(m, NULL, L, &hs);
    char aa[TAGLEN];
    snprintf(aa, sizeof aa, "%s", cs->perf[0] ? cs->perf : meta_album_artist(m, L, na));
    int ng;
    if(cs->genre[0]){ const char *g[1] = { cs->genre }; ng = ldb_split_names(g, 1, 0, L->genres, MAXV); }
    else ng = meta_genres(m, L);
    for(int i = 0; i < f->ntr && !ldb_failed(g_ldb); i++){
        const cue_track_t *t = &tr[i];
        hs = 0;
        if(t->perf[0] || cs->perf[0]){ const char *v[1] = { t->perf[0] ? t->perf : cs->perf }; na = ldb_split_names(v, 1, 1, L->arts, MAXV); }
        else na = meta_artists(m, NULL, L, &hs);
        ldb_track lt;
        ldb_fill(&lt, m, L, na, hs, ng);
        lt.path = audio; lt.source = LDB_SRC_CUE;
        lt.start_ms = t->ms;
        lt.end_ms = i + 1 < f->ntr ? tr[i + 1].ms : -1;          /* the last track runs to the end of the file */
        lt.duration_ms = (i + 1 < f->ntr ? tr[i + 1].ms : dur) - t->ms;
        lt.title = t->title[0] ? t->title : name; lt.title_sort = NULL;
        if(cs->title[0]){ lt.album_title = cs->title; lt.album_sort = NULL; }
        lt.album_artist = aa;
        if(cs->perf[0]) lt.album_artist_sort = NULL;
        lt.track = t->num; lt.track_total = cs->ntr;
        if(cs->date[0] && year4(cs->date)) lt.year = year4(cs->date);
        lt.has_lyrics = 0; lt.bpm = 0; lt.initial_key = NULL;   /* the file's lyrics and tempo are the whole file's */
        lt.rg_flags = (t->rg_flags & (LDB_RG_TG | LDB_RG_TP)) | (cs->rg_flags ? (cs->rg_flags & (LDB_RG_AG | LDB_RG_AP)) : (m->rg_flags & (LDB_RG_AG | LDB_RG_AP)));
        lt.rg_track_gain = t->rg_tg; lt.rg_track_peak = t->rg_tp;
        if(cs->rg_flags){ lt.rg_album_gain = cs->rg_ag; lt.rg_album_peak = cs->rg_ap; }
        lt.file_mtime = (long long)st.st_mtime; lt.file_size = (long long)st.st_size;
        ldb_add(g_ldb, &lt);
    }
}
/* write one FILE of a sheet as CUE rows; 1 done, 0 skipped (the file keeps its ordinary row), -1 DB error */
static int cue_write_file(const cue_sheet_t *cs, const cue_file_t *f, const char *audio){
    if(f->ntr < 1) return 0;
    const cue_track_t *tr = cs->tr + f->first;
    { sqlite3_stmt *q = NULL; int claimed = 0;           /* another sheet already described this file */
      if(sqlite3_prepare_v2(g_db, "SELECT 1 FROM cue_claimed WHERE PATH=?1;", -1, &q, NULL) != SQLITE_OK) return -1;
      sqlite3_bind_text(q, 1, audio, -1, SQLITE_TRANSIENT);
      int rc = sqlite3_step(q); sqlite3_finalize(q);
      if(rc == SQLITE_ROW) claimed = 1; else if(rc != SQLITE_DONE) return -1;
      if(claimed) return 0; }
    const char *base = strrchr(audio, '/'); base = base ? base + 1 : audio;
    scan_meta *m = g_meta; long long dur = 0;
    int tst = tags_with_provenance(audio, base, m, &dur);
    if(tst == 0 || dur <= 0) return 0;                  /* can't open it, or its length is unknown -> ordinary row */
    if(has_ext(audio, ".m4b")) return 0;                /* an audiobook: never a CUE row in the music table */
    { sqlite3_stmt *sh = NULL; int shape = -1;          /* stock embedded shape: CUE rows + ordinary row, not ours */
      if(sqlite3_prepare_v2(g_db, "SELECT EXISTS(SELECT 1 FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=1) AND "
            "EXISTS(SELECT 1 FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0) AND NOT "
            "EXISTS(SELECT 1 FROM DISKOS_CUE_FILES WHERE PATH=?1);", -1, &sh, NULL) != SQLITE_OK) return -1;
      sqlite3_bind_text(sh, 1, audio, -1, SQLITE_TRANSIENT);
      if(sqlite3_step(sh) == SQLITE_ROW) shape = sqlite3_column_int(sh, 0);
      sqlite3_finalize(sh);
      if(shape < 0) return -1;
      if(shape) return 0; }
    for(int i = 0; i < f->ntr; i++){                    /* offsets present, increasing, inside the file; numbers unique */
        if(tr[i].ms < 0 || tr[i].ms >= dur) return 0;
        if(i && (tr[i].ms <= tr[i - 1].ms || tr[i].num <= tr[i - 1].num)) return 0;
    }
    char name[MAXPATH]; snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.'); if(dot && dot != name) *dot = 0;
    sqlite3_stmt *up = g_cue_up, *in = g_cue_in;
    /* the ordinary row becomes the first track (keeps its ID) unless that track already exists */
    { sqlite3_stmt *cv = NULL;
      if(sqlite3_prepare_v2(g_db, "UPDATE SONG SET IS_CUE=1,IS_ISO=0,TRACK=?2,DISC=2147483647,ALBUM_ARTIST=NULL,"
            "ALBUM_ARTIST_CODE=2147483647 WHERE ID=(SELECT MIN(ID) FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 "
            "AND COALESCE(IS_ISO,0)=0) AND NOT EXISTS (SELECT 1 FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=1 "
            "AND TRACK=?2);", -1, &cv, NULL) != SQLITE_OK) return -1;
      sqlite3_bind_text(cv, 1, audio, -1, SQLITE_TRANSIENT); sqlite3_bind_int(cv, 2, tr[0].num);
      int crc = sqlite3_step(cv); sqlite3_finalize(cv);
      if(crc != SQLITE_DONE) return -1; }
    int rc = 1;
    char keep[CUE_MAX_TRACKS * 4 + 8] = ""; int kl = 0;
    for(int i = 0; i < f->ntr && rc == 1; i++){
        const cue_track_t *t = &tr[i];
        long long len = (i + 1 < f->ntr ? tr[i + 1].ms : dur) - t->ms;
        char tt[TAGLEN], ta[TAGLEN], tb[TAGLEN], tg[TAGLEN];
        snprintf(tt, sizeof tt, "%.*s", TAGLEN - 1, t->title[0] ? t->title : name);   /* tags are TAGLEN, like every row */
        snprintf(tb, sizeof tb, "%s", cs->title[0] ? cs->title : song_album(m));
        snprintf(ta, sizeof ta, "%s", t->perf[0] ? t->perf : cs->perf[0] ? cs->perf : song_artist(m));
        snprintf(tg, sizeof tg, "%s", cs->genre[0] ? cs->genre : song_genre(m));
        for(int pass = 0; pass < 2 && rc == 1; pass++){
            sqlite3_stmt *s = pass ? in : up;
            sqlite3_reset(s); sqlite3_clear_bindings(s);
            sqlite3_bind_text(s, 1, audio, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(s, 2, t->num);
            sqlite3_bind_text(s, 3, name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 4, tt, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, tb, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 6, ta, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 7, tg, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 8, (sqlite3_int64)t->ms);
            sqlite3_bind_int64(s, 9, (sqlite3_int64)len);
            sqlite3_bind_int(s, 10, code4(name)); sqlite3_bind_int(s, 11, code4(tt)); sqlite3_bind_int(s, 12, code4(tb));
            sqlite3_bind_int(s, 13, code4(ta)); sqlite3_bind_int(s, 14, code4(tg));
            if(pass) sqlite3_bind_int64(s, 15, (sqlite3_int64)time(NULL));
            if(sqlite3_step(s) != SQLITE_DONE){ rc = -1; break; }
            if(!pass && sqlite3_changes(g_db) > 0) break;   /* existing row updated in place (keeps its ID) */
        }
        kl += snprintf(keep + kl, sizeof keep - (size_t)kl, "%s%d", kl ? "," : "", t->num);
    }
    if(rc != 1) return rc;
    char sql[sizeof keep + 128];                        /* tracks the sheet no longer lists (keep = digits and commas) */
    snprintf(sql, sizeof sql, "DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=1 AND TRACK NOT IN (%s);", keep);
    if(db_exec_path(sql, audio, NULL) != 0) return -1;
    if(db_exec_path("DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0;", audio, NULL) != 0) return -1;
    if(db_exec_path("INSERT OR IGNORE INTO cue_claimed(PATH) VALUES(?1);", audio, NULL) != 0) return -1;
    if(db_exec_path("INSERT OR IGNORE INTO DISKOS_CUE_FILES(PATH) VALUES(?1);", audio, NULL) != 0) return -1;
    cue_ldb(cs, f, audio, name, m, dur);
    return 1;
}
enum { CR_OK, CR_ENOENT, CR_OPEN_ERR, CR_IOERR, CR_SKIP, CR_OOM };
/* Read and parse one sheet (<= CUE_MAX_BYTES, UTF-8 or Latin-1, BOM dropped) into *cs. *nread = bytes read (budget).
 * *latin1 (may be NULL) = the sheet wasn't UTF-8, so it was read as Latin-1 (a sheet declares no encoding). */
static int cue_read_sheet(const char *cue_path, cue_sheet_t *cs, size_t *nread, int *latin1){
    *nread = 0;
    if(latin1) *latin1 = 0;
    FILE *fp = fopen(cue_path, "rb");
    if(!fp) return errno == ENOENT ? CR_ENOENT : CR_OPEN_ERR;
    char *raw = malloc(CUE_MAX_BYTES + 1);
    if(!raw){ fclose(fp); return CR_OOM; }
    size_t n = fread(raw, 1, CUE_MAX_BYTES + 1, fp);
    int err = ferror(fp); fclose(fp);
    *nread = n;
    if(err){ free(raw); return CR_IOERR; }
    if(n > CUE_MAX_BYTES){ free(raw); return CR_SKIP; }         /* not a sheet we read */
    size_t off = (n >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) ? 3 : 0;
    char *text;
    if(memchr(raw + off, 0, n - off)){ free(raw); return CR_SKIP; }   /* binary / UTF-16: not handled */
    if(utf8_ok((unsigned char *)raw + off, n - off)){ raw[n] = 0; text = raw + off; }
    else {                                              /* not UTF-8: Latin-1, no code page guessing */
        text = malloc((n - off) * 2 + 1);               /* a Latin-1 byte becomes at most 2 UTF-8 bytes */
        if(!text){ free(raw); return CR_OOM; }
        raw[n] = 0;
        int e = latin1_copy((unsigned char *)raw + off, n - off, text, (int)((n - off) * 2 + 1));
        if(latin1) *latin1 = e == ENC_LATIN1;
        free(raw); raw = text;
    }
    cue_parse(text, cs);
    free(raw);
    return CR_OK;
}
/* Browse Files: the audio files a sheet names, resolved exactly like the scan does (same bounds, same path
 * normalisation and traversal guard); `seen(path)` says whether a candidate file is in the library. Writes up to `max`
 * distinct MAXPATH-sized paths to `out`; returns the count (0 = unreadable, not a usable sheet, or nothing resolved). */
int scan_cue_files(const char *cue_path, char (*out)[MAXPATH], int max, int (*seen)(const char *)){
    if(!cue_path || !out || max <= 0 || !seen) return 0;
    cue_sheet_t *cs = malloc(sizeof *cs);
    if(!cs) return 0;
    size_t n; int n_out = 0;
    if(cue_read_sheet(cue_path, cs, &n, NULL) == CR_OK && !cs->bad){
        for(int i = 0; i < cs->nfiles && n_out < max; i++){
            char audio[MAXPATH]; int dup = 0;
            if(!cue_backing_ex(cue_path, cs->f[i].name, audio, sizeof audio, seen)) continue;
            for(int j = 0; j < n_out; j++) if(!strcmp(out[j], audio)) dup = 1;
            if(!dup) snprintf(out[n_out++], MAXPATH, "%s", audio);
        }
    }
    free(cs);
    return n_out;
}
static void cue_process(const char *cue_path, cue_sheet_t *cs){
    if(g_cue_bytes >= CUE_MAX_TOTAL){ g_prune_blocked = 1; cue_keep(cue_path, "CUE sheets over this scan's budget"); return; }   /* over budget: its rows wait, untouched */
    size_t n; int lat; int rc = cue_read_sheet(cue_path, cs, &n, &lat);
    g_cue_bytes += (long long)n;
    if(rc == CR_ENOENT || rc == CR_SKIP) return;
    if(rc == CR_OPEN_ERR || rc == CR_IOERR){ g_prune_blocked = 1; cue_keep(cue_path, "CUE sheet unreadable"); return; }   /* unreadable sheet: its rows must not be dropped */
    if(rc == CR_OOM){ g_scan_err = 1; return; }
    if(lat && g_ldb && !ldb_in_nomedia(g_ldb, cue_path)) rep_add(REP_ENC, "  %s\n      the CUE sheet (Latin-1, not Unicode)", rel_path(cue_path));
    if(cs->bad) return;
    for(int i = 0; i < cs->nfiles && !g_scan_err; i++){
        char audio[MAXPATH];
        if(!cue_backing(cue_path, cs->f[i].name, audio, sizeof audio)) continue;
        if(cue_write_file(cs, &cs->f[i], audio) < 0) g_scan_err = 1;
    }
}
/* after the walk: write every sheet's rows, then retire rows of sheets that are gone */
static void cue_finish(void){
    if(sqlite3_exec(g_db, "CREATE TEMP TABLE IF NOT EXISTS cue_claimed(PATH TEXT PRIMARY KEY); DELETE FROM cue_claimed;"
                          "CREATE TABLE IF NOT EXISTS DISKOS_CUE_FILES(PATH TEXT PRIMARY KEY);", 0,0,0) != SQLITE_OK){
        g_scan_err = 1; return;
    }
    g_cue_bytes = 0; g_cue_up = g_cue_in = g_cue_seen = NULL;
    cue_sheet_t *cs = malloc(sizeof *cs);
    if(!cs || sqlite3_prepare_v2(g_db, CUE_UPD, -1, &g_cue_up, NULL) != SQLITE_OK
           || sqlite3_prepare_v2(g_db, CUE_INS, -1, &g_cue_in, NULL) != SQLITE_OK
           || sqlite3_prepare_v2(g_db, "SELECT 1 FROM seen WHERE PATH=?1;", -1, &g_cue_seen, NULL) != SQLITE_OK) g_scan_err = 1;
    for(int i = 0; i < g_ncues && !g_scan_err && !atomic_load(&g_abort); i++) cue_process(g_cues[i], cs);
    free(cs);
    sqlite3_finalize(g_cue_up); sqlite3_finalize(g_cue_in); sqlite3_finalize(g_cue_seen);
    g_cue_up = g_cue_in = g_cue_seen = NULL;
    if(g_scan_err || g_prune_blocked) return;           /* a sheet may be hidden in a skipped folder: keep all rows */
    /* files diskOS expanded that no sheet wrote this scan (deleted or now unusable): the first track turns back into the
     * ordinary row (same ID) and the other tracks go (only DISKOS_CUE_FILES - rows stock wrote are never removed here).
     * At most CUE_MAX_SHEETS per scan; the rest follow on the next one. */
    sqlite3_stmt *q = NULL;
    if(sqlite3_prepare_v2(g_db, "SELECT PATH FROM DISKOS_CUE_FILES WHERE PATH LIKE '" SCAN_ROOT "/%' "
                                "AND PATH NOT IN (SELECT PATH FROM cue_claimed) AND PATH NOT IN (SELECT PATH FROM unreadable) LIMIT 4096;", -1, &q, NULL) != SQLITE_OK){
        g_scan_err = 1; return;
    }
    char (*gone)[MAXPATH] = NULL; int ng = 0, cap = 0, rc;
    while((rc = sqlite3_step(q)) == SQLITE_ROW){
        const unsigned char *p = sqlite3_column_text(q, 0);
        if(!p) continue;
        if(ng == cap){ int nc = cap ? cap * 2 : 16; void *m = realloc(gone, (size_t)nc * sizeof *gone); if(!m){ rc = SQLITE_NOMEM; break; } gone = m; cap = nc; }
        snprintf(gone[ng++], MAXPATH, "%s", (const char *)p);
    }
    sqlite3_finalize(q);
    if(rc != SQLITE_DONE){ free(gone); g_scan_err = 1; return; }
    for(int i = 0; i < ng && !g_scan_err; i++){
        if(atomic_load(&g_abort)){ g_scan_err = 1; break; }   /* cancel: stop per item */
        if(db_exec_path("UPDATE SONG SET IS_CUE=0,TRACK=0,DISC=0,OFFSET=0,ALBUM_ARTIST_CODE=0 WHERE ID=(SELECT MIN(ID) FROM SONG WHERE PATH=?1 "
                        "AND COALESCE(IS_CUE,0)=1) AND NOT EXISTS (SELECT 1 FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=0 "
                        "AND COALESCE(IS_ISO,0)=0);", gone[i], NULL) != 0
           || db_exec_path("DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_CUE,0)=1;", gone[i], NULL) != 0
           || db_exec_path("DELETE FROM DISKOS_CUE_FILES WHERE PATH=?1;", gone[i], NULL) != 0){ g_scan_err = 1; break; }
        struct stat st;
        const char *b = strrchr(gone[i], '/');
        if(lstat(gone[i], &st) == 0 && S_ISREG(st.st_mode) && b && is_audio(b + 1)) upsert_song(gone[i], b + 1, &st, 0);
    }
    free(gone);
}

/* ---- SACD disc images (L17) ----
 * Stock V2.57's scanner turns a SACD .iso into one SONG row per track of its first readable area (add_iso_song, P257
 * 0x443894; parser 0x621140..; internal RE notes), and the player re-opens the image and plays a row by its
 * TRACK (OFFSET is 0; the image itself never gets an ordinary row). diskOS writes the same rows from the same
 * structures: 2048-byte sectors; Master TOC "SACDMTOC" at LSN 510 (version <= 1.20; area 1 start/size at +64/+84,
 * area 2 at +72/+86); an area's "TWOCHTOC"/"MULCHTOC" TOC (track count at +69, text locale charset at +90), then its
 * "SACDTTxt" (per-track TITLE item 1 / PERFORMER item 2), "SACD_IGL" (2 sectors), "SACD_ACC" (32), "SACDTRL1" and
 * "SACDTRL2" (per-track duration min/sec/frame at +8+4*255+4*i). An image whose track list (SACDTRL1) points past its end
 * (cut short) gets no rows - stock doesn't check, but those tracks could not play. Rows: PATH = the image, NAME = its file name without
 * the extension, TRACK 1..n, IS_ISO=1, OFFSET 0, DISC 0x7fffffff, ALBUM_ARTIST NULL. Differences from stock, on
 * purpose: DURATION counts frames as 1/75 s (stock 10 ms), ALBUM/ARTIST fall back to the disc's own album title and
 * artist (Master Text +16/+18; stock always writes "unknown album"), and text in the disc's charset (Shift-JIS,
 * EUC-KR, GB2312, Big5) is converted to UTF-8 with iconv. A missing TITLE is "Track N". The rows come from the
 * STEREO area when the disc has one (the player plays that area; stock's scanner takes the first readable area, which
 * on a multichannel-first disc lists tracks that don't match playback), else the first readable area. An image that
 * reads fine but is no longer a usable SACD image loses its rows. Rows keep their IDs across
 * rescans (PATH + TRACK); tracks the image no longer has go; an unreadable image keeps its rows (the walk marked it
 * seen) and a deleted one is pruned with the other formats. */
#include <iconv.h>
#define ISO_MAX 1024
#define ISO_SECT 2048
static char (*g_isos)[MAXPATH];
static int g_nisos, g_isos_cap;
static void iso_note(const char *path){
    if(g_nisos >= ISO_MAX){ g_prune_blocked = 1; ldb_keep_note(path, LDB_KEEP_PATH, NULL, "too many disc images"); return; }
    if(g_nisos == g_isos_cap){
        int nc = g_isos_cap ? g_isos_cap * 2 : 16; if(nc > ISO_MAX) nc = ISO_MAX;
        void *p = realloc(g_isos, (size_t)nc * sizeof *g_isos);
        if(!p){ g_prune_blocked = 1; ldb_keep_note(path, LDB_KEEP_PATH, NULL, "out of memory"); return; }
        g_isos = p; g_isos_cap = nc;
    }
    snprintf(g_isos[g_nisos++], MAXPATH, "%s", path);
}
/* one sector: 1 read, 0 not in the file (structure problem), -1 I/O error (musl's fseeko does not set the stream error
 * flag on a failed seek, so it is reported here, not left to ferror) */
static int g_iso_ioerr;
static int iso_read(FILE *f, off_t fsz, long lsn, unsigned char *buf){
    off_t at = (off_t)lsn * ISO_SECT;
    if(lsn < 0 || at + ISO_SECT > fsz) return 0;
    if(fseeko(f, at, SEEK_SET) != 0 || fread(buf, 1, ISO_SECT, f) != ISO_SECT){ g_iso_ioerr = 1; return -1; }
    return 1;
}
static unsigned be16u(const unsigned char *p){ return (unsigned)p[0] << 8 | p[1]; }
static unsigned long be32u(const unsigned char *p){ return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]; }
/* disc text (NUL-terminated within the sector, from byte `off`) -> UTF-8 in out, by the locale's charset byte */
static void iso_text(const unsigned char *sec, unsigned off, int charset, char *out, int cap){
    out[0] = 0;
    if(off < 8 || off >= ISO_SECT) return;
    size_t n = 0; while(off + n < ISO_SECT && sec[off + n]) n++;
    if(!n) return;
    static const char *const CS[8] = { NULL, NULL, NULL, "SHIFT_JIS", "EUC-KR", "GB2312", "BIG5", NULL };
    const char *cs = CS[charset & 7];
    if(cs){
        iconv_t cd = iconv_open("UTF-8", cs);
        if(cd != (iconv_t)-1){
            char in[ISO_SECT]; memcpy(in, sec + off, n);
            char *ip = in, *op = out; size_t il = n, ol = (size_t)cap - 1;
            size_t r = iconv(cd, &ip, &il, &op, &ol);
            int full = r == (size_t)-1 && errno == E2BIG;      /* out is full: what fits is whole characters */
            iconv_close(cd);
            if(r != (size_t)-1 || full){ *op = 0; str_trim(out); return; }
        }
    }
    if(utf8_ok(sec + off, n)){ snprintf(out, cap, "%.*s", (int)n, (const char *)sec + off); u8_fix_tail(out); str_trim(out); return; }
    char *o = out, *end = out + cap - 1;                /* ISO 646 / 8859-1 (1, 2, 7) or undecodable: Latin-1 */
    for(size_t i = 0; i < n; i++) put_u8(&o, end, sec[off + i]);
    *o = 0; str_trim(out);
}
typedef struct { int n, channels; long long dur[255], start[255]; char title[255][TAGLEN], perf[255][TAGLEN]; char album[TAGLEN], artist[TAGLEN]; } iso_disc_t;
/* 1 = parsed into *d, 0 = not a SACD image / unsupported, -1 = I/O error */
static int iso_parse(const char *path, iso_disc_t *d){
    memset(d, 0, sizeof *d);
    FILE *f = fopen(path, "rb");
    if(!f) return errno == ENOENT ? 0 : -1;
    off_t fsz = file_size(f);
    unsigned char m[ISO_SECT], t[ISO_SECT], s[ISO_SECT];
    int ok = 0;
    g_iso_ioerr = 0;
    if(fsz < 0){ fclose(f); return -1; }
    if(iso_read(f, fsz, 510, m) != 1 || memcmp(m, "SACDMTOC", 8) || m[8] > 1 || m[9] > 20) goto out;   /* stock's limits */
    { unsigned char mt[ISO_SECT];                       /* the disc's own album title / artist (Master Text) */
      if(iso_read(f, fsz, 511, mt) == 1 && !memcmp(mt, "SACDText", 8)){
          iso_text(mt, be16u(mt + 16), m[138], d->album, TAGLEN);
          iso_text(mt, be16u(mt + 18), m[138], d->artist, TAGLEN);
      } }
    /* The player PLAYS the stereo area (it stores that area's index), stock's scanner lists the first area it can read.
     * Prefer the stereo area so the rows are the tracks that actually play; else the first readable one. */
    for(int pass = 0; pass < 2 && !ok && !g_iso_ioerr; pass++)           /* pass 0: stereo areas, pass 1: the others */
    for(int area = 0; area < 2 && !ok && !g_iso_ioerr; area++){
        long start = (long)be32u(m + (area ? 72 : 64));
        if(!start || iso_read(f, fsz, start, t) != 1) continue;
        if((memcmp(t, "TWOCHTOC", 8) && memcmp(t, "MULCHTOC", 8)) || t[8] > 1 || t[9] > 20) continue;
        int stereo = t[32] == 2 && t[33] == 0;                            /* channel count 2, speaker config 0 */
        if(stereo != (pass == 0)) continue;
        unsigned size = be16u(t + 10);                                    /* the area's own sector count (stock) */
        int n = t[69], charset = t[90];
        if(n < 1 || size < 1 || size > 4096) continue;
        memset(d->title, 0, sizeof d->title); memset(d->perf, 0, sizeof d->perf); memset(d->dur, 0, sizeof d->dur);
        memset(d->start, 0, sizeof d->start);
        int have_trl2 = 0, have_trl1 = 0, got_text = 0;
        for(long lsn = start + 1; lsn < start + (long)size; lsn++){   /* the area's other sectors, in order */
            if(atomic_load(&g_abort)){ g_scan_err = 1; break; }   /* user cancel inside a long image read */
            if(iso_read(f, fsz, lsn, s) != 1) break;
            if(!memcmp(s, "SACDTTxt", 8)){
                if(!got_text){
                    for(int i = 0; i < n; i++){
                        unsigned off = be16u(s + 8 + 2 * i);
                        if(off < 8 || off + 4 >= ISO_SECT) continue;
                        int items = s[off]; unsigned p = off + 4;
                        for(int k = 0; k < items && p + 2 < ISO_SECT; k++){
                            int type = s[p]; p += 2;
                            char txt[TAGLEN]; iso_text(s, p, charset, txt, sizeof txt);
                            if(type == 1) snprintf(d->title[i], TAGLEN, "%s", txt);
                            else if(type == 2) snprintf(d->perf[i], TAGLEN, "%s", txt);
                            while(p < ISO_SECT && s[p]) p++;                 /* past the text */
                            while(p < ISO_SECT && !s[p]) p++;                /* and its padding */
                        }
                    }
                    got_text = 1;
                }
            } else if(!memcmp(s, "SACD_IGL", 8)) lsn += 1;
            else if(!memcmp(s, "SACD_ACC", 8)) lsn += 31;
            else if(!memcmp(s, "SACDTRL1", 8)){                         /* every track's audio must be in the file */
                unsigned long long total = (unsigned long long)(fsz / ISO_SECT);
                for(int i = 0; i < n; i++){
                    unsigned long long st0 = be32u(s + 8 + 4 * i), ln = be32u(s + 8 + 4 * 255 + 4 * i);
                    if(!ln || st0 < (unsigned long long)start || st0 > total || ln > total - st0){ have_trl1 = -1; break; }
                }
                if(have_trl1 == 0) have_trl1 = 1;
            }
            else if(!memcmp(s, "SACDTRL2", 8)){
                for(int i = 0; i < n; i++){
                    const unsigned char *r = s + 8 + 4 * 255 + 4 * i, *a = s + 8 + 4 * i;   /* length; start time */
                    if(r[1] > 59 || r[2] > 74){ have_trl2 = 0; break; }
                    d->dur[i] = ((long long)r[0] * 60 + r[1]) * 1000 + r[2] * 1000 / 75;
                    d->start[i] = a[1] > 59 || a[2] > 74 ? -1 : ((long long)a[0] * 60 + a[1]) * 1000 + a[2] * 1000 / 75;
                    have_trl2 = 1;
                }
            } else break;                                                     /* unknown sector: the area ends */
        }
        if(!have_trl2 || have_trl1 != 1) continue;     /* no track list, or audio cut off: not a playable image */
        d->n = n; d->channels = t[32]; ok = 1;
    }
out:
    { int err = ferror(f) || g_iso_ioerr; fclose(f); if(err) return -1; }   /* any I/O error: never "not an image" */
    return ok;
}
/* library.db: the image's tracks, each a slice of the image starting at its TRL2 start time (when those times go up
 * track by track; else the lengths added up). Album and artist as the SONG rows; the album artist is the disc's artist,
 * else the first track's performer. */
static void iso_ldb(const char *path, const char *name, const iso_disc_t *d){
    struct stat st;
    if(!g_ldb || ldb_failed(g_ldb) || ldb_in_nomedia(g_ldb, path) || lstat(path, &st) != 0) return;
    ldb_lists *L = g_lists;
    int rising = d->n > 0 && d->start[0] >= 0;
    for(int i = 1; i < d->n && rising; i++) rising = d->start[i] > d->start[i - 1];
    const char *aa = d->artist[0] ? d->artist : d->perf[0];
    long long at = 0;
    for(int i = 0; i < d->n && !ldb_failed(g_ldb); i++){
        char tt[TAGLEN];
        if(d->title[i][0]) snprintf(tt, sizeof tt, "%s", d->title[i]); else snprintf(tt, sizeof tt, "Track %d", i + 1);
        const char *v[1] = { d->perf[i][0] ? d->perf[i] : d->artist };
        int na = v[0][0] ? ldb_split_names(v, 1, 1, L->arts, MAXV) : 0;
        ldb_track t;
        memset(&t, 0, sizeof t);
        t.path = path; t.source = LDB_SRC_ISO;
        t.start_ms = rising ? d->start[i] : at;
        t.end_ms = t.start_ms + d->dur[i];
        at += d->dur[i];
        t.album_title = d->album[0] ? d->album : name;
        t.album_artist = aa;
        t.title = tt;
        t.artists = (const char (*)[LDB_NAME])L->arts; t.n_artists = na;
        t.track = i + 1; t.track_total = d->n;
        t.duration_ms = d->dur[i];
        t.sample_rate = 2822400; t.bits = 1; t.channels = d->channels; t.codec = "dsd";
        t.file_mtime = (long long)st.st_mtime; t.file_size = (long long)st.st_size;
        ldb_add(g_ldb, &t);
    }
}
static void iso_write(const char *path, const iso_disc_t *d){
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char name[MAXPATH]; snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.'); if(dot && dot != name) *dot = 0;
    iso_ldb(path, name, d);
    static const char *UPD =
        "UPDATE SONG SET NAME=?3,TITLE=?4,ALBUM=?5,ARTIST=?6,GENRE=?7,DURATION=?8,NAME_CODE=?9,TITLE_CODE=?10,ALBUM_CODE=?11,"
        "ARTIST_CODE=?12,GENRE_CODE=?13 WHERE PATH=?1 AND TRACK=?2 AND COALESCE(IS_ISO,0)=1;";
    static const char *INS =
        "INSERT INTO SONG (PATH,TRACK,NAME,TITLE,ALBUM,ARTIST,GENRE,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,"
        "GENRE_CODE,ADD_TIME,DISC,IS_CUE,IS_ISO,IS_DSD,OFFSET,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,"
        "SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,"
        "2147483647,0,1,0,0,0,0,0,0,NULL,NULL,0,NULL,2147483647);";
    sqlite3_stmt *up = NULL, *in = NULL;
    if(sqlite3_prepare_v2(g_db, UPD, -1, &up, NULL) != SQLITE_OK || sqlite3_prepare_v2(g_db, INS, -1, &in, NULL) != SQLITE_OK){
        sqlite3_finalize(up); g_scan_err = 1; return;
    }
    for(int i = 0; i < d->n && !g_scan_err; i++){
        char tt[TAGLEN], ta[TAGLEN];
        if(d->title[i][0]) snprintf(tt, sizeof tt, "%s", d->title[i]); else snprintf(tt, sizeof tt, "Track %d", i + 1);
        snprintf(ta, sizeof ta, "%s", d->perf[i][0] ? d->perf[i] : d->artist[0] ? d->artist : "Unknown artist");
        const char *al = d->album[0] ? d->album : "Unknown album", *ge = "Unknown genre";
        for(int pass = 0; pass < 2; pass++){
            sqlite3_stmt *s = pass ? in : up;
            sqlite3_reset(s); sqlite3_clear_bindings(s);
            sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT); sqlite3_bind_int(s, 2, i + 1);
            sqlite3_bind_text(s, 3, name, -1, SQLITE_TRANSIENT); sqlite3_bind_text(s, 4, tt, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, al, -1, SQLITE_TRANSIENT);   sqlite3_bind_text(s, 6, ta, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 7, ge, -1, SQLITE_TRANSIENT);   sqlite3_bind_int64(s, 8, (sqlite3_int64)d->dur[i]);
            sqlite3_bind_int(s, 9, code4(name)); sqlite3_bind_int(s, 10, code4(tt)); sqlite3_bind_int(s, 11, code4(al));
            sqlite3_bind_int(s, 12, code4(ta));  sqlite3_bind_int(s, 13, code4(ge));
            if(pass) sqlite3_bind_int64(s, 14, (sqlite3_int64)time(NULL));
            if(sqlite3_step(s) != SQLITE_DONE){ g_scan_err = 1; break; }
            if(!pass && sqlite3_changes(g_db) > 0) break;      /* updated in place: same ID */
        }
        if(!g_scan_err){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); }   /* a card of only images still commits */
    }
    sqlite3_finalize(up); sqlite3_finalize(in);
    if(g_scan_err) return;
    sqlite3_stmt *del = NULL;                               /* tracks the image no longer has */
    if(sqlite3_prepare_v2(g_db, "DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_ISO,0)=1 AND TRACK>?2;", -1, &del, NULL) != SQLITE_OK){ g_scan_err = 1; return; }
    sqlite3_bind_text(del, 1, path, -1, SQLITE_TRANSIENT); sqlite3_bind_int(del, 2, d->n);
    if(sqlite3_step(del) != SQLITE_DONE) g_scan_err = 1;
    sqlite3_finalize(del);
}
static void iso_finish(void){
    iso_disc_t *d = malloc(sizeof *d);
    if(!d){ if(g_nisos) g_scan_err = 1; return; }
    for(int i = 0; i < g_nisos && !g_scan_err && !atomic_load(&g_abort); i++){
        int r = iso_parse(g_isos[i], d);
        if(r > 0) iso_write(g_isos[i], d);
        else if(r < 0){ g_prune_blocked = 1; ldb_keep_note(g_isos[i], LDB_KEEP_PATH, NULL, "disc image unreadable"); }   /* an image we couldn't READ: keep its rows, prune nothing */
        else {                                              /* read fine but no longer a usable SACD image: its rows go */
            pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);   /* a card read in full: the removal commits */
            sqlite3_stmt *del = NULL;
            if(sqlite3_prepare_v2(g_db, "DELETE FROM SONG WHERE PATH=?1 AND COALESCE(IS_ISO,0)=1;", -1, &del, NULL) != SQLITE_OK){ g_scan_err = 1; break; }
            sqlite3_bind_text(del, 1, g_isos[i], -1, SQLITE_TRANSIENT);
            if(sqlite3_step(del) != SQLITE_DONE) g_scan_err = 1;
            sqlite3_finalize(del);
        }
    }
    free(d);
}

/* recursive walk; inserts every audio file found under dir. lstat (not stat) so symlinks
 * are never followed, + a depth cap, so a hostile/looping tree can't run away. */
static void walk(const char *dir, int depth){
    if(depth > 24){ g_scan_err=1; return; }   /* absurdly deep -> treat as incomplete, don't commit */
    DIR *d=opendir(dir);
    if(!d){ g_scan_err=1; return; }           /* the SD is exFAT/vfat (no perms): a failed opendir means
                                               * I/O error or the card was pulled -> incomplete scan */
    struct dirent *e;
    char path[MAXPATH];
    if(g_ldb && !ldb_in_nomedia(g_ldb, dir)){   /* a .nomedia file keeps this folder out of library.db (SONG still has it) */
        struct stat ns;
        int w = snprintf(path, sizeof path, "%s/.nomedia", dir);
        if(w > 0 && w < (int)sizeof path && lstat(path, &ns) == 0){
            ldb_nomedia(g_ldb, dir);
            rep_add(REP_NOMEDIA, "  %s/", rel_path(dir));
        }
    }
    for(errno=0; (e=readdir(d)); errno=0){    /* errno reset before each readdir so we can detect a read error */
        if(g_scan_err) break;                 /* an earlier write failed (may have auto-rolled-back the txn): stop issuing writes */
        if(atomic_load(&g_abort)){ g_scan_err = 1; break; }  /* SD access revoked mid-walk: stop reading the card at once
                                                * (g_scan_err also blocks the commit, keeping the library) */
        if(e->d_name[0]=='.'){
            if(e->d_name[1]==0 || (e->d_name[1]=='.'&&e->d_name[2]==0)) continue;   /* . / .. */
            /* Hidden files and folders are never scanned, and they don't block the vanished-row prune (yopo,
             * 2026-10-18): song.db only serves the stock player until diskOS plays by itself, so a song indexed inside
             * a hidden folder long ago may simply drop out of it. The report lists the hidden folders it skipped. */
            int wl = snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
            struct stat ds;
            if(wl > 0 && wl < (int)sizeof path && lstat(path,&ds) == 0 && S_ISDIR(ds.st_mode))
                rep_add(REP_HIDDEN, "  %s/", rel_path(path));
            continue;
        }
        int w = snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
        if(w<0 || w>=(int)sizeof path){ g_skipped++; g_prune_blocked=1; continue; }   /* overlong path unknowable -> can't preserve its row -> block the prune */
        struct stat st;
        /* Failing the whole scan on ANY per-file lstat error was the killer bug: every real exFAT SD
         * has entries that readdir lists but lstat can't resolve (ENOENT) - orphaned entries and, on
         * this device, real files whose special-character names don't round-trip through the mount's
         * encoding. Those always set g_scan_err=1, so the commit gate rolled back a fully-successful
         * index -> empty library (why the DB had to be hand-built). Skip ONLY the benign ENOENT case
         * (the file can't be opened to index anyway); any OTHER errno (EIO/EACCES = the card going bad)
         * stays fatal so we never commit a silently-incomplete library over a good one. */
        if(lstat(path,&st)!=0){
            if(errno == ENOENT){
                /* library.db: what this entry may hold keeps its songs from the previous scan */
                if(is_audio(e->d_name) || has_ext(e->d_name, ".iso")) ldb_keep_note(path, LDB_KEEP_PATH, NULL, "name unreadable");
                else if(has_ext(e->d_name, ".cue")) cue_keep(path, "name unreadable");
                if(e->d_type == DT_DIR || e->d_type == DT_UNKNOWN){
                    char tree[MAXPATH];
                    int tl = snprintf(tree, sizeof tree, "%s/", path);
                    if(tl > 0 && tl < (int)sizeof tree) ldb_keep_note(tree, LDB_KEEP_TREE, NULL, "folder unreadable");
                }
                if(is_audio(e->d_name)){
                    mark_seen(path);   /* preserve the entire existing row (special-char names) */
                    if(db_exec_path("INSERT OR IGNORE INTO unreadable(PATH) VALUES(?1);", path, NULL) != 0) g_prune_blocked = 1;
                }
                else if(e->d_type == DT_REG && !has_ext(e->d_name, ".iso") && !has_ext(e->d_name, ".cue")){
                    /* readdir says plain file (a special-char .m3u/.jpg): it owns no rows and hides no subtree,
                     * so it must not block the prune - one such file used to stop vanished songs and images
                     * from ever leaving the library */
                } else g_prune_blocked = 1;                /* could be an unresolvable DIRECTORY whose songs we never
                                                            * walked -> block the prune so its subtree rows aren't deleted */
                g_skipped++; continue;                     /* unresolvable entry -> skip, best-effort */
            }
            g_scan_err = 1; continue;                        /* real I/O/access error -> don't commit a partial index */
        }
        if(S_ISDIR(st.st_mode)) walk(path, depth+1);
        else if(S_ISREG(st.st_mode) && is_audio(e->d_name)) upsert_song(path, e->d_name, &st, 1);
        else if(S_ISREG(st.st_mode) && has_ext(e->d_name, ".cue")) cue_note(path);   /* written after the walk (cue_finish) */
        else if(S_ISREG(st.st_mode) && has_ext(e->d_name, ".iso")){ mark_seen(path); iso_note(path); }   /* SACD image (iso_finish) */
        else if(S_ISREG(st.st_mode) && is_unsupported_audio(e->d_name)){  /* AAC/M4A/OGG/... not indexed yet */
            pthread_mutex_lock(&g_mu); g_unsupported++; pthread_mutex_unlock(&g_mu);
        }
    }
    if(errno) g_scan_err=1;                   /* readdir error -> this directory listing was incomplete */
    closedir(d);
}

/* True only when SCAN_ROOT is a REAL, readable mountpoint (its st_dev differs from its
 * parent's). A missing/late SD, or a bare placeholder /tmp/sdcard dir, returns 0 - so we
 * never wipe the live library rebuilding from an unmounted card. */
static int scan_root_ready(void){
    struct stat sroot, sparent;
    if(stat(SCAN_ROOT, &sroot)!=0 || !S_ISDIR(sroot.st_mode)) return 0;
    char parent[MAXPATH];
    if(snprintf(parent,sizeof parent,"%s/..",SCAN_ROOT) >= (int)sizeof parent) return 0;
    if(stat(parent, &sparent)!=0) return 0;
    if(sroot.st_dev == sparent.st_dev) return 0;   /* not a separate mount -> SD not mounted */
    DIR *d=opendir(SCAN_ROOT); if(!d) return 0; closedir(d);
    return 1;
}

/* The worker's one decision point: commit (want=1) or fail. Taken under g_mu together with cancel acceptance, so a
 * cancel is either accepted before it (=> cancelled, no commit) or refused after it (the outcome stays committed or
 * failed). *go = 1 only if the worker may COMMIT. */
static void scan_decide(int want, int *go){
    pthread_mutex_lock(&g_mu);
    *go = 0;
    if(!g_cancelled){
        g_commit_decided = 1;
        *go = want && !atomic_load(&g_abort);
    }
    pthread_mutex_unlock(&g_mu);
#ifdef SCAN_HOOK_AFTER_DECISION
    SCAN_HOOK_AFTER_DECISION();   /* host tests only: pause the worker right after the decision */
#endif
}
static int scan_busy(void *p, int n){
    (void)p;
    if(atomic_load(&g_abort) || n >= 800) return 0;   /* give up: cancelled, or ~8 s of waiting */
    usleep(10000);
    return 1;
}
/* The scan report, SCAN_ROOT/diskos-scan-report.txt: what library.db got and what the owner may want to fix on a
 * computer (text that isn't Unicode, artists split in two, folders left out, files this scan couldn't read). Written
 * only after a scan that read the whole card, through a temporary file so a half-written report never replaces one. */
#define REPORT_PATH SCAN_ROOT "/diskos-scan-report.txt"
#define REPORT_TMP  SCAN_ROOT "/.diskos-scan-report.tmp"
static void report_list(FILE *f, int which, const char *title){
    const rep_list *l = &g_rep[which];
    fprintf(f, "\n== %s ==\n", title);
    if(!l->n && !l->more){ fputs("  none\n", f); return; }
    if(l->buf) fputs(l->buf, f);
    if(l->more) fprintf(f, "  ... and %d more\n", l->more);
}
static void report_write(long secs, int published){
    ldb_stats st;
    ldb_get_stats(g_ldb, &st);
    const char *why = !g_ldb ? g_ldb_why : ldb_error(g_ldb);
    if(published)
        fprintf(stderr, "scanner: library.db: %lld songs, %lld albums, %lld artists, %lld carried over, %lld KB\n",
                st.tracks, st.albums, st.artists, st.carried, st.bytes / 1024);
    else fprintf(stderr, "scanner: library.db not written: %s\n", why);
    FILE *f = fopen(REPORT_TMP, "w");
    if(!f){ fprintf(stderr, "scanner: scan report: %s\n", strerror(errno)); return; }
    char when[32] = "";
    time_t now = time(NULL); struct tm tm;
    if(localtime_r(&now, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    fprintf(f, "diskOS scan report\n\nScanned %s, took %ld s.\n", when, secs);
    if(published){
        fprintf(f, "library.db: %lld songs, %lld albums, %lld artists, %lld KB.\n", st.tracks, st.albums, st.artists, st.bytes / 1024);
        if(st.carried) fprintf(f, "Songs kept from the previous scan (not readable this time): %lld.\n", st.carried);
        if(st.carry_failed) fputs("The previous library.db couldn't be read, so nothing was kept from it.\n", f);
    } else fprintf(f, "library.db was not written this scan (%s). The previous one is still used.\n", why[0] ? why : "unknown error");
    struct statvfs vs;
    if(statvfs(LIBRARYDB_DIR, &vs) == 0)
        fprintf(f, "Free space on /usr/data: %.1f MB.\n", (double)vs.f_bavail * (double)vs.f_frsize / (1024.0 * 1024.0));
    report_list(f, REP_ENC, "Text that isn't Unicode (fix these tags on a computer)");
    report_list(f, REP_SPLIT, "Artists split into several names (on ';' or \"feat.\")");
    report_list(f, REP_NOMEDIA, "Folders left out of the library (.nomedia)");
    report_list(f, REP_KEPT, "Couldn't read this scan (their songs from the previous scan stay)");
    report_list(f, REP_HIDDEN, "Hidden folders not scanned (their songs are not in the library)");
    int bad = fflush(f) != 0 || ferror(f);
    if(!bad) fsync(fileno(f));
    if(fclose(f) != 0) bad = 1;
    if(bad || rename(REPORT_TMP, REPORT_PATH) != 0){
        fprintf(stderr, "scanner: scan report not written: %s\n", strerror(errno));
        unlink(REPORT_TMP);
    }
}
static void *scan_thread(void *arg){
    (void)arg;
    int ok=0, found=0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    /* Guard: if the SD isn't actually mounted at SCAN_ROOT, do NOT open a transaction or
     * DELETE anything - keep the existing library intact. This is the #1 data-loss guard:
     * an absent/late mount must never commit an empty SONG table. */
    if(!scan_root_ready()){
        pthread_mutex_lock(&g_mu);
        g_no_sd=1; g_total=0; g_outcome=OUT_NO_SD;      /* 0 => library kept; g_no_sd lets the UI say "insert SD" */
        g_active=0; g_finished_seq++;
        pthread_mutex_unlock(&g_mu);
        sd_io_end();
        return NULL;
    }
    /* one song's tags and split names: too big for this thread's stack */
    g_meta = malloc(sizeof *g_meta); g_meta_save = malloc(sizeof *g_meta_save); g_lists = malloc(sizeof *g_lists);
    if(!g_meta || !g_meta_save || !g_lists) g_scan_err = 1;
    rep_reset();
    if(sqlite3_open_v2(DB_PATH,&g_db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK){
        sqlite3_busy_handler(g_db, scan_busy, NULL);   /* up to 8 s, but gives up at once when a cancel/abort lands */
        /* Everything that can change song.db (schema migration, index, scratch tables) runs INSIDE the scan
         * transaction, so a cancelled or failed scan rolls back to a byte-identical file. */
        int in_txn = (sqlite3_exec(g_db,"BEGIN IMMEDIATE;",0,0,0)==SQLITE_OK);
        if(!in_txn) g_scan_err = 1;
        if(in_txn){
        sqlite3_exec(g_db, SCHEMA_SONG, 0,0,0);
        /* Bring an EXISTING (older diskOS or stock) SONG table up to the V2.28 superset BEFORE the
         * scan transaction, so the stock player's playback SQL (which SELECTs IS_M3U/M3U_PATH) always
         * finds the columns instead of depending on the player winning a startup migration race.
         * ALTERs are idempotent here - a duplicate column just errors harmlessly. */
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN IS_M3U INT DEFAULT 0;", 0,0,0);
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN M3U_PATH TEXT DEFAULT '';", 0,0,0);
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN ACCENT INTEGER DEFAULT 0;", 0,0,0);
        /* NULL = legacy/unknown; 0 = fallback; 1 = TITLE read before filename fallback. */
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN TITLE_TAG INTEGER;", 0,0,0);
        /* Non-unique index on SONG(PATH): every scanned file looks its path up (UPDATE ... WHERE PATH=?, the
         * cue check, the insert-if-new check), which without it is a full-table scan per file - quadratic, and
         * all inside the scan's write transaction. Non-unique on purpose: stock rows may repeat a PATH (cue/iso
         * tracks). Checked (sampled, not exhaustive) not to change the order of the stock player's queries, on the
         * firmware's own SQLite over a real library plus duplicate cue/iso paths (tools/path_index_order_check.py:
         * V2.40 + V2.57, 3,493 queries, 0 differences); the player's joins on SONG.PATH use it instead of building
         * a temporary index each time. Best effort: without it a scan is only slower. */
        sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS diskos_song_path ON SONG(PATH);", 0,0,0);
        /* PROVE the required columns exist (a duplicate-column ALTER error is fine, but a real
         * failure - BUSY/FULL/IOERR/corruption - is not). If any is missing, flag a scan error so
         * the commit gate below rolls back and KEEPS the existing library rather than rebuilding one
         * the stock V2.28 player can't query (its playback SQL SELECTs IS_M3U/M3U_PATH). */
        if(!song_has_col(g_db,"IS_M3U") || !song_has_col(g_db,"M3U_PATH") || !song_has_col(g_db,"ACCENT") || !song_has_col(g_db,"TITLE_TAG")){
            g_scan_err = 1;
            fprintf(stderr, "scanner: SONG schema migration incomplete -> keeping existing library\n");
        }
        }   /* in_txn */
        int decided = 0;
        g_ins=NULL; g_upd=NULL; g_seen=NULL; g_cuechk=NULL; g_bookok=NULL; g_durok=NULL;
        /* a required column still missing: roll back and finish now, no walk (retried on the next scan) */
        if(in_txn && !g_scan_err && sqlite3_prepare_v2(g_db, INS_SONG, -1, &g_ins,  NULL)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, UPD_SONG, -1, &g_upd,  NULL)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, CUE_CHECK, -1, &g_cuechk, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, SEEN_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, SEEN_INS, -1, &g_seen, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, BOOKOK_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, BOOKOK_INS, -1, &g_bookok, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, DUROK_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, DUROK_INS, -1, &g_durok, NULL)==SQLITE_OK){
            /* ONE atomic transaction: the whole MERGE (UPDATE existing / INSERT new / DELETE vanished)
             * commits together, or ROLLBACK on any failure - a failed/partial scan can never corrupt or
             * wipe the library. MERGE (not delete+reinsert) so existing rows KEEP their ID + ACCENT,
             * preserving resume (MEMORY_PLAY.MUSIC_ID) + favourites (MY_LOVE.ID) + art-accent across a
             * rescan. Only mp3/flac/wav rows are ever removed; m4a/ape/dsf/... are never touched. */
            {
                g_skipped=0; g_prune_blocked=0;
                /* Audiobooks live in their OWN table, never SONG (the player queues all of SONG for
                 * music, so a book in SONG would leak into Play-All/shuffle). */
                sqlite3_exec(g_db,"CREATE TABLE IF NOT EXISTS BOOKS (ID INTEGER PRIMARY KEY autoincrement,"
                    "PATH TEXT UNIQUE, NAME TEXT, TITLE TEXT, ARTIST TEXT, ALBUM TEXT, GENRE TEXT,"
                    "DURATION BIGINT, ADD_TIME INT8);",0,0,0);
                sqlite3_exec(g_db,"DELETE FROM seen;",0,0,0);   /* start from an empty seen-set */
                sqlite3_exec(g_db,"DELETE FROM book_ok;",0,0,0);   /* and an empty real-tags-book set */
                g_ncues = 0; g_nisos = 0;
                {   /* library.db is rebuilt beside song.db, always leaving song.db the room it needs (ldb_reserve) */
                    struct stat ds;
                    g_song_size0 = stat(DB_PATH, &ds) == 0 ? (long long)ds.st_size : 0;
                    g_song_done = 0;
                    g_ldb_why[0] = 0;
                    g_ldb = ldb_begin(ldb_reserve, g_ldb_why, (int)sizeof g_ldb_why);
                }
                walk(SCAN_ROOT, 0);
                if(!g_scan_err) cue_finish();   /* CUE sheets found by the walk -> per-track rows */
                if(!g_scan_err) iso_finish();   /* SACD images -> per-track rows */
                pthread_mutex_lock(&g_mu); found = g_done; pthread_mutex_unlock(&g_mu);
                /* observability: one concise line per scan */
                fprintf(stderr,"scanner: merged %d songs, skipped %d unreadable file(s), scan_err=%d\n",
                        found, g_skipped, g_scan_err);
                /* Commit only if the walk found audio AND the SD is STILL mounted afterwards (guards a
                 * card pulled / gone-I/O mid-scan; zero-found rolls back too). Then delete scanned-format
                 * rows whose PATH vanished from the SD - inside the txn, so it rolls back on any failure. */
                if(found > 0 && !g_scan_err && !atomic_load(&g_abort) && scan_root_ready()){
                    /* Prune vanished songs - but ONLY when pruning wasn't blocked (!g_prune_blocked: no
                     * overlong/unknown-type skip, no mark_seen failure) and scoped to THIS mount (PATH
                     * under SCAN_ROOT). ENOENT/unreadable audio files ARE preserved in `seen`, so they
                     * don't block; only unknowable skips do. Rows from another mount/source that this
                     * scanner never inspected must not be touched. If blocked, we still commit the merge
                     * (UPDATEs/INSERTs) but do NOT delete. */
                    static const char *DEL_ABSENT =
                        "DELETE FROM SONG WHERE (lower(PATH) LIKE '%.mp3' OR lower(PATH) LIKE '%.flac'"
                        " OR lower(PATH) LIKE '%.wav' OR lower(PATH) LIKE '%.m4a' OR lower(PATH) LIKE '%.m4b'"
                        " OR lower(PATH) LIKE '%.aac' OR lower(PATH) LIKE '%.ogg' OR lower(PATH) LIKE '%.ape'"
                        " OR lower(PATH) LIKE '%.aif' OR lower(PATH) LIKE '%.aiff' OR lower(PATH) LIKE '%.wma'"
                        " OR lower(PATH) LIKE '%.dsf' OR lower(PATH) LIKE '%.dff' OR lower(PATH) LIKE '%.dts' OR lower(PATH) LIKE '%.iso'"
                        " OR lower(PATH) LIKE '%.opus' OR lower(PATH) LIKE '%.oga')"
                        " AND PATH LIKE '" SCAN_ROOT "/%' "
                        "AND PATH NOT IN (SELECT PATH FROM seen);";
                    int del_ok = (g_prune_blocked) ? 1
                                                   : (sqlite3_exec(g_db,DEL_ABSENT,0,0,0)==SQLITE_OK);
                    /* Relocate the .m4b rows the walk indexed into BOOKS, then remove them from SONG, all
                     * inside this txn so the player never sees a book in the music queue. Only SEEN (present)
                     * books move (never lose a book under a prune-blocked skipped dir); vanished books are
                     * pruned from BOOKS only when pruning is unblocked. */
                    if(del_ok && DISKOS_AUDIOBOOKS){
                        int r = sqlite3_exec(g_db,
                            /* Insert new books; for an EXISTING book, refresh NAME/TITLE/ARTIST/ALBUM/GENRE
                             * ONLY when this scan actually read its tags (PATH in book_ok) - so a good re-read
                             * fixes stale/fallback metadata, while a read FAILURE (filename fallback) never
                             * clobbers previously-good tags. ADD_TIME is left out of the SET (kept on conflict).
                             * DURATION is set by the separate UPDATE below from this scan's reading whenever the
                             * length was READ (book_dur_ok), even if unknown (0) and even if the tags failed; a
                             * read error keeps the old length. CUE/ISO virtual rows of an .m4b are never relocated
                             * or deleted (the player times those by OFFSET+DURATION). */
                            "INSERT INTO BOOKS(PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME) "
                            "SELECT PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME FROM SONG "
                            "WHERE lower(PATH) LIKE '%.m4b' AND PATH IN (SELECT PATH FROM seen) "
                            "AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0 "
                            "ON CONFLICT(PATH) DO UPDATE SET "
                            "NAME=excluded.NAME,TITLE=excluded.TITLE,ARTIST=excluded.ARTIST,"
                            "ALBUM=excluded.ALBUM,GENRE=excluded.GENRE "
                            "WHERE BOOKS.PATH IN (SELECT PATH FROM book_ok);",0,0,0)==SQLITE_OK
                          /* the LENGTH, independently of the tags: every book whose length was READ this scan */
                          && sqlite3_exec(g_db,
                            "UPDATE BOOKS SET DURATION=(SELECT MAX(0,IFNULL(S.DURATION,0)) FROM SONG S WHERE S.PATH=BOOKS.PATH "
                            "AND COALESCE(S.IS_CUE,0)=0 AND COALESCE(S.IS_ISO,0)=0 LIMIT 1) "
                            "WHERE PATH IN (SELECT PATH FROM book_dur_ok) AND EXISTS (SELECT 1 FROM SONG S WHERE "
                            "S.PATH=BOOKS.PATH AND COALESCE(S.IS_CUE,0)=0 AND COALESCE(S.IS_ISO,0)=0);",0,0,0)==SQLITE_OK
                          && sqlite3_exec(g_db,
                            "DELETE FROM SONG WHERE lower(PATH) LIKE '%.m4b' AND PATH IN (SELECT PATH FROM seen) "
                            "AND COALESCE(IS_CUE,0)=0 AND COALESCE(IS_ISO,0)=0;",0,0,0)==SQLITE_OK;
                        if(r && !g_prune_blocked)
                            r = sqlite3_exec(g_db,
                                "DELETE FROM BOOKS WHERE PATH LIKE '" SCAN_ROOT "/%' AND PATH NOT IN (SELECT PATH FROM seen);",0,0,0)==SQLITE_OK;
                        if(!r) del_ok = 0;   /* relocate failed -> roll back the whole scan */
                    }
                    /* A cancellation that lands AFTER the walk (or during the pruning SQL) must still keep the
                     * library as it was: check again immediately before COMMIT, and roll back instead. */
                    /* The commit decision and a cancel's acceptance are ONE transition under g_mu: once the worker
                     * decides to commit, scanner_cancel() is refused, so a "Cancelling" toast is never followed by a
                     * committed scan. */
                    /* library.db never stands in SONG's way: a failed library.db.new, or one /usr/data can't spare the
                     * room for, is removed before the commit (the previous library.db stays) */
                    if(g_ldb && (ldb_failed(g_ldb) || !ldb_room_ok(g_ldb))){
                        snprintf(g_ldb_why, sizeof g_ldb_why, "%s", ldb_failed(g_ldb) ? ldb_error(g_ldb) : "not enough free space on /usr/data");
                        ldb_end(g_ldb); g_ldb = NULL;
                    }
                    int go = 0;
                    scan_decide(del_ok, &go); decided = 1;
                    if(go) ok = (sqlite3_exec(g_db,"COMMIT;",0,0,0)==SQLITE_OK);
                    else ok = 0;
                    if(ok){
                        /* song.db is safe now; library.db is finished after it (carry-over, albums, sort keys, indexes),
                         * stopped only by SD access being revoked (a cancel is refused once the commit is decided) */
                        g_song_done = 1;
                        int pub = g_ldb && ldb_finish(g_ldb, scan_cancelled, NULL) == 0 && ldb_publish(g_ldb) == 0;
                        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
                        report_write((long)(t1.tv_sec - t0.tv_sec), pub);
                    }
                }
            }
        }
        if(!decided){ int go_; scan_decide(0, &go_); }   /* a failure before the commit gate is decided too */
        if(in_txn && !ok) sqlite3_exec(g_db,"ROLLBACK;",0,0,0);   /* also covers a failed prepare / walk error */
        sqlite3_finalize(g_ins);  g_ins=NULL;
        sqlite3_finalize(g_upd);  g_upd=NULL;
        sqlite3_finalize(g_cuechk); g_cuechk=NULL;
        sqlite3_finalize(g_seen); g_seen=NULL;
        sqlite3_finalize(g_bookok); g_bookok=NULL;
        sqlite3_finalize(g_durok); g_durok=NULL;
    }
    if(ok) ipc_note_library_commit();
    sqlite3_close(g_db); g_db=NULL;   /* close even on a failed open: sqlite3_open_v2 may still return a handle */
    ldb_end(g_ldb); g_ldb = NULL;     /* an unpublished library.db.new is removed */
    free(g_meta); free(g_meta_save); free(g_lists); g_meta = g_meta_save = NULL; g_lists = NULL;
    rep_reset();
    pthread_mutex_lock(&g_mu);
    g_skipped_result = g_skipped;
    g_outcome = ok ? OUT_COMMITTED : (g_cancelled ? OUT_CANCELLED : OUT_FAILED);
    if(ok) g_committed_pending = 1;   /* held until scanner_take_committed(): a later start must not hide it */
    g_total = ok ? g_done : 0;    /* committed count; 0 signals a failed rebuild (library kept) */
    g_active=0; g_finished_seq++;
    pthread_mutex_unlock(&g_mu);
    sd_io_end();
        return NULL;
}

/* Stop a running walk. The scan holds a single lease for its whole run, so closing admission alone
 * cannot stop it - without this, revoking SD access left the scanner traversing the card. */
void scanner_abort(void){ atomic_store(&g_abort, 1); }
/* User cancel: same stop as scanner_abort (the walk sets g_scan_err, so nothing commits and the txn rolls
 * back), but remembered so the UI reports "Scan cancelled" rather than a failure. */
int scanner_cancel(void){
    pthread_mutex_lock(&g_mu);
    int accepted = g_active && !g_commit_decided;   /* refused once the worker has decided to commit */
    if(accepted) g_cancelled = 1;
    pthread_mutex_unlock(&g_mu);
    if(accepted) atomic_store(&g_abort, 1);
    return accepted;
}
int scanner_start(void){
    if(!sd_io_begin()) return -1;
    pthread_mutex_lock(&g_mu);
    /* Refuse an overlapping start BEFORE touching the cancellation flag: clearing it first erased a
     * cancellation aimed at the scan that is already running. */
    if(g_active){ pthread_mutex_unlock(&g_mu); sd_io_end(); return -1; }   /* already scanning */
    atomic_store(&g_abort, 0);
    g_active=1; g_done=0; g_total=0; g_no_sd=0; g_cancelled=0; g_commit_decided=0; g_outcome=OUT_NONE; g_skipped=0; g_skipped_result=0; g_scan_err=0; g_unsupported=0;
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    if(pthread_create(&th,NULL,scan_thread,NULL)!=0){
        pthread_mutex_lock(&g_mu); g_active=0; pthread_mutex_unlock(&g_mu);
        sd_io_end(); return -1;
    }
    pthread_detach(th);
    return 0;
}

/* ===================== [isolated test harness] =========================== */
#ifdef SCANNER_TEST
#include <unistd.h>
int main(void){
    fprintf(stderr,"scan test -> %s (root %s)\n", DB_PATH, SCAN_ROOT);
    if(scanner_start()!=0){ fprintf(stderr,"start failed\n"); return 1; }
    int done,total;
    while(scanner_active()){ scanner_progress(&done,NULL); fprintf(stderr,"  %d...\r",done); usleep(200000); }
    scanner_progress(&done,&total);
    fprintf(stderr,"\nDONE: %d songs\n", total);
    return 0;
}
#endif

int scan_read_chapters(const char *path, chapter_t *out, int max){
    if(!sd_io_begin()) return 0;
    int result = scan_read_chapters_leased(path, out, max);
    sd_io_end();
    return result;
}

int scan_read_narrator(const char *path, char *out, int cap){
    if(!sd_io_begin()) return 0;
    int result = scan_read_narrator_leased(path, out, cap);
    sd_io_end();
    return result;
}

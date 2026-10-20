/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* m3u.c - playlists as .m3u8 files on the card, and songs whose file is gone (see playlists.h). */
#include "playlists.h"
#include "shelvesdb.h"
#include "sdio.h"
#include "artcache.h"   /* sd_write_begin / sd_write_end */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CARD
#define CARD "/tmp/sdcard"
#endif
#define EXPORT_DIR CARD "/Playlists"

void playlists_mark_missing(ldb_list *l){
    if(!l || l->n <= 0 || !sd_io_begin()) return;
    for(int i = 0; i < l->n; i++){
        struct stat st;
        if(l->v[i].count == 1 && l->v[i].path && stat(l->v[i].path, &st) != 0) l->v[i].count = 2;
    }
    sd_io_end();
}

/* ---- export ---- */
/* A file name for a playlist: its name without the characters a card or a computer refuses. */
static void file_name(const char *name, char *out, int cap){
    int j = 0;
    const unsigned char *s = (const unsigned char *)name;
    for(; *s && j < cap - 1; s++) out[j++] = (*s < 32 || strchr("/\\:*?\"<>|", *s)) ? '_' : (char)*s;
    if(*s && j > 0 && ((unsigned char)out[j-1] & 0x80)){   /* cut short: never end inside a character */
        int k = j - 1;
        while(k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80) k--;
        unsigned char lead = (unsigned char)out[k];
        int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if(j - k < need) j = k;
    }
    while(j > 0 && (out[j-1] == ' ' || out[j-1] == '.')) j--;
    out[j] = 0;
    if(!out[0]) snprintf(out, (size_t)cap, "Playlist");
}
/* 1 when path is our export of the playlist called name (its #PLAYLIST line), so writing over it loses nothing */
static int our_export(const char *path, const char *name){
    FILE *f = fopen(path, "r");
    if(!f) return 0;
    char line[LDB_NAME + 16];
    int ours = 0;
    for(int i = 0; i < 4 && fgets(line, sizeof line, f); i++){
        if(strncmp(line, "#PLAYLIST:", 10)) continue;
        line[strcspn(line, "\r\n")] = 0;
        ours = !strcmp(line + 10, name);
        break;
    }
    fclose(f);
    return ours;
}
static void one_line(char *s){ for(; *s; s++) if(*s == '\r' || *s == '\n') *s = ' '; }

static int export_locked(const char *name, const ldb_list *l, char *file, int cap){
    char base[200], path[300];
    file_name(name, base, 160);
    snprintf(path, sizeof path, EXPORT_DIR "/%s.m3u8", base);
    for(int k = 2; access(path, F_OK) == 0 && !our_export(path, name); k++){   /* never over someone else's file */
        if(k > 99) return 0;
        snprintf(path, sizeof path, EXPORT_DIR "/%s (%d).m3u8", base, k);
    }
    mkdir(EXPORT_DIR, 0755);
    char tmp[300];
    snprintf(tmp, sizeof tmp, EXPORT_DIR "/.diskos-export-XXXXXX");
    int fd = mkstemp(tmp);
    if(fd < 0) return 0;
    FILE *f = fdopen(fd, "w");
    if(!f){ close(fd); unlink(tmp); return 0; }
    char nm[LDB_NAME];
    snprintf(nm, sizeof nm, "%s", name);
    one_line(nm);
    int ok = fprintf(f, "#EXTM3U\n#PLAYLIST:%s\n", nm) > 0;
    for(int i = 0; ok && i < l->n; i++){
        const ldb_item *s = &l->v[i];
        char info[2 * LDB_NAME + 8];
        if(s->artist && s->artist[0]) snprintf(info, sizeof info, "%s - %s", s->artist, s->title);
        else snprintf(info, sizeof info, "%s", s->title);
        one_line(info);
        ok = fprintf(f, "#EXTINF:%d,%s\n", s->duration_ms > 0 ? (s->duration_ms + 500) / 1000 : -1, info) > 0;
        /* a CUE/ISO song is a part of its file: other players see the file, ours the part */
        if(ok && s->start_ms > 0) ok = fprintf(f, "#DISKOS-START:%lld\n", s->start_ms) > 0;
        /* a song on the card by its place from this file, so a computer finds it too */
        const char *p = s->path;
        if(ok) ok = !strncmp(p, CARD "/", sizeof CARD) ? fprintf(f, "../%s\n", p + sizeof CARD) > 0 : fprintf(f, "%s\n", p) > 0;
    }
    if(fflush(f) != 0) ok = 0;
    if(ok && fsync(fileno(f)) != 0 && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) ok = 0;
    if(fclose(f) != 0) ok = 0;
    if(ok && rename(tmp, path) != 0) ok = 0;
    if(!ok){ unlink(tmp); return 0; }
    int dfd = open(EXPORT_DIR, O_RDONLY | O_DIRECTORY);
    if(dfd >= 0){ fsync(dfd); close(dfd); }
    const char *slash = strrchr(path, '/');
    snprintf(file, (size_t)cap, "%s", slash + 1);
    return 1;
}
int playlists_export(long long id, const char *name, char *file, int cap){
    ldb_list l;
    if(!name || !name[0] || !shdb_ready() || ldb_read_playlist(id, &l) < 0) return 0;
    int ok = 0;
    if(sd_io_begin()){
        if(sd_write_begin()){ ok = export_locked(name, &l, file, cap); sd_write_end(); }
        sd_io_end();
    }
    ldb_list_free(&l);
    return ok;
}

/* Import. A line naming a song: a path from the playlist file's folder, from the card's root, or a whole path. */
#define M3U_MAX_BYTES (4L * 1024 * 1024)
#define WALK_DEPTH   8
#define WALK_DIRS    4096
#define WALK_FILES   512
#define WALK_ENTRIES 65536

static int utf8_ok(const char *s){
    const unsigned char *p = (const unsigned char *)s;
    while(*p){
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if(n < 0) return 0;
        p++;
        while(n--){ if((*p & 0xC0) != 0x80) return 0; p++; }
    }
    return 1;
}
/* A line that isn't UTF-8 is taken as Latin-1 (an old .m3u from a computer). */
static void to_utf8(const char *in, char *out, int cap){
    if(utf8_ok(in)){ snprintf(out, (size_t)cap, "%s", in); return; }
    int j = 0;
    for(const unsigned char *p = (const unsigned char *)in; *p && j < cap - 2; p++){
        if(*p < 0x80) out[j++] = (char)*p;
        else { out[j++] = (char)(0xC0 | (*p >> 6)); out[j++] = (char)(0x80 | (*p & 0x3F)); }
    }
    out[j] = 0;
}
/* "/a/b/../c/./d" -> "/a/c/d", in place */
static void normalize(char *p){
    char *out = p, *in = p;
    while(*in){
        if(in[0] == '/' && in[1] == '.' && (in[2] == '/' || !in[2])){ in += 2; continue; }
        if(in[0] == '/' && in[1] == '.' && in[2] == '.' && (in[3] == '/' || !in[3])){
            while(out > p && *--out != '/'){}
            in += 3;
            continue;
        }
        if(in[0] == '/' && in[1] == '/'){ in++; continue; }
        *out++ = *in++;
    }
    *out = 0;
    if(!p[0]){ p[0] = '/'; p[1] = 0; }
}
static int is_file(const char *p){ struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
/* a+sep+b when it names a file on the card: 1, its whole path in out */
static int try_path(const char *a, const char *sep, const char *b, char *out, int cap){
    char buf[1024];
    if(snprintf(buf, sizeof buf, "%s%s%s", a, sep, b) >= (int)sizeof buf) return 0;
    normalize(buf);
    if(strncmp(buf, CARD "/", sizeof CARD) || !is_file(buf) || (int)strlen(buf) >= cap) return 0;
    strcpy(out, buf);
    return 1;
}
/* The song a line names: a whole Disc path, a path from the card's root ("/Music/x.flac", "E:/Music/x.flac" from a
 * computer), or a path from the playlist file's folder. Only songs on the card. 1 = found. */
static int resolve(const char *dir, const char *line, char *out, int cap){
    if(line[0] && line[1] == ':' && line[2] == '/') line += 2;
    if(line[0] == '/') return try_path("", "", line, out, cap) || try_path(CARD, "", line, out, cap);
    return try_path(dir, "/", line, out, cap);
}
typedef struct {
    shdb_song *v; int n, cap;
    char **strs; int ns, scap;   /* the strings v points at */
} songs_t;
static const char *keep_str(songs_t *s, const char *t){
    if(!t || !t[0]) return NULL;
    if(s->ns == s->scap){
        int nc = s->scap ? s->scap * 2 : 64;
        char **p = realloc(s->strs, (size_t)nc * sizeof *p);
        if(!p) return NULL;
        s->strs = p; s->scap = nc;
    }
    char *d = strdup(t);
    if(d) s->strs[s->ns++] = d;
    return d;
}
static int songs_add(songs_t *s, const char *path, long long start_ms, const char *title){
    if(s->n == s->cap){
        int nc = s->cap ? s->cap * 2 : 64;
        shdb_song *p = realloc(s->v, (size_t)nc * sizeof *p);
        if(!p) return 0;
        s->v = p; s->cap = nc;
    }
    shdb_song *x = &s->v[s->n];
    memset(x, 0, sizeof *x);
    x->path = keep_str(s, path);
    x->start_ms = start_ms;
    x->title = keep_str(s, title);
    if(!x->path) return 0;
    s->n++;
    return 1;
}
static void songs_free(songs_t *s){
    for(int i = 0; i < s->ns; i++) free(s->strs[i]);
    free(s->strs); free(s->v);
    memset(s, 0, sizeof *s);
}
static void trim(char *s){
    char *b = s;
    while(*b == ' ' || *b == '\t') b++;
    if(b != s) memmove(s, b, strlen(b) + 1);
    size_t n = strlen(s);
    while(n && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
}
/* 1 = a playlist made, 0 = left out (its name exists, or no song of it was found) */
static int import_file(const char *file){
    FILE *f = fopen(file, "r");
    if(!f) return 0;
    struct stat st;
    if(fstat(fileno(f), &st) != 0 || st.st_size > M3U_MAX_BYTES){ fclose(f); return 0; }
    char dir[600], name[LDB_NAME] = "", raw[1024], line[1024], title[LDB_NAME] = "";
    snprintf(dir, sizeof dir, "%s", file);
    char *sl = strrchr(dir, '/');
    if(sl) *sl = 0;
    songs_t s;
    memset(&s, 0, sizeof s);
    long long start = 0;
    int start_known = 0, cue_track = 0, ok = 1;
    while(ok && fgets(raw, sizeof raw, f)){
        if(!strchr(raw, '\n') && !feof(f)){   /* longer than any path: skip it whole */
            int c; while((c = fgetc(f)) != '\n' && c != EOF){}
            continue;
        }
        raw[strcspn(raw, "\r\n")] = 0;
        char *p = raw;
        if((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
        to_utf8(p, line, sizeof line);
        trim(line);
        if(!line[0]) continue;
        if(!strncmp(line, "#PLAYLIST:", 10)){
            if(!name[0]){ snprintf(name, sizeof name, "%s", line + 10); trim(name); }
            continue;
        }
        if(!strncmp(line, "#EXTINF:", 8)){
            const char *c = strchr(line, ',');
            snprintf(title, sizeof title, "%s", c ? c + 1 : "");
            continue;
        }
        if(!strncmp(line, "#DISKOS-START:", 14)){ start = strtoll(line + 14, NULL, 10); start_known = start >= 0; continue; }
        if(!strncmp(line, "#DISKOS-TRACK:", 14)){ cue_track = atoi(line + 14); continue; }   /* older exports */
        if(line[0] == '#') continue;
        for(char *q = line; *q; q++) if(*q == '\\') *q = '/';
        char path[1024];
        if(resolve(dir, line, path, sizeof path)){
            long long at = start_known ? start : 0;
            if(!start_known && cue_track > 0 && !ldb_track_start_at(path, cue_track, &at)) at = -1;   /* that track isn't known */
            if(at >= 0) ok = songs_add(&s, path, at, title);
        }
        start = 0; start_known = 0; cue_track = 0; title[0] = 0;
    }
    if(ferror(f)) ok = 0;
    fclose(f);
    if(!name[0]){   /* no #PLAYLIST line: the file's name */
        const char *b = strrchr(file, '/');
        to_utf8(b ? b + 1 : file, name, sizeof name);
        char *dot = strrchr(name, '.');
        if(dot) *dot = 0;
        trim(name);
    }
    int made = 0;
    if(ok && s.n > 0 && name[0] && shdb_pl_find(name) == 0){
        long long id = shdb_pl_create(name, 0);
        if(id > 0){
            if(shdb_pl_add(id, s.v, s.n) == s.n) made = 1;
            else shdb_pl_delete(id);
        }
    }
    songs_free(&s);
    return made;
}
/* budget: [0] folders, [1] files, [2] directory entries still allowed */
static void import_walk(const char *dir, int depth, int *budget, int *made, int *skipped){
    if(budget[0]-- <= 0) return;
    DIR *d = opendir(dir);
    if(!d) return;
    struct dirent *e;
    while(budget[2] > 0 && (e = readdir(d))){
        budget[2]--;
        const char *n = e->d_name;
        if(n[0] == '.' || !strcasecmp(n, "System Volume Information")) continue;   /* hidden folders hold our own files */
        char path[600];
        if(snprintf(path, sizeof path, "%s/%s", dir, n) >= (int)sizeof path) continue;
        int isdir = e->d_type == DT_DIR, isreg = e->d_type == DT_REG;
        if(e->d_type == DT_UNKNOWN){ struct stat st; if(lstat(path, &st) == 0){ isdir = S_ISDIR(st.st_mode); isreg = S_ISREG(st.st_mode); } }
        size_t L = strlen(n);
        if(isreg && ((L > 4 && !strcasecmp(n + L - 4, ".m3u")) || (L > 5 && !strcasecmp(n + L - 5, ".m3u8")))){
            if(budget[1]-- <= 0) break;
            if(import_file(path)) (*made)++; else (*skipped)++;
        } else if(isdir && depth < WALK_DEPTH) import_walk(path, depth + 1, budget, made, skipped);
    }
    closedir(d);
}
int playlists_import_card(int *skipped){
    int made = 0, left = 0, budget[3] = { WALK_DIRS, WALK_FILES, WALK_ENTRIES };
    if(skipped) *skipped = 0;
    if(!shdb_ready() || !sd_io_begin()) return -1;
    import_walk(CARD, 0, budget, &made, &left);
    sd_io_end();
    if(skipped) *skipped = left;
    return made;
}

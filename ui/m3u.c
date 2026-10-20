/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* m3u.c - playlists as .m3u8 files on the card, and songs whose file is gone (see playlists.h). */
#include "playlists.h"
#include "shelvesdb.h"
#include "sdio.h"
#include "artcache.h"   /* sd_write_begin / sd_write_end */
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
long long playlists_import_file(const char *file, long long folder_id, char *name_out, int cap, int *found, int *missed){
    if(found) *found = 0;
    if(missed) *missed = 0;
    if(name_out && cap > 0) name_out[0] = 0;
    if(!file || !shdb_ready() || !sd_io_begin()) return -1;
    FILE *f = fopen(file, "r");
    if(!f){ sd_io_end(); return -1; }
    struct stat st;
    if(fstat(fileno(f), &st) != 0 || st.st_size > M3U_MAX_BYTES){ fclose(f); sd_io_end(); return -1; }
    char dir[600], name[LDB_NAME] = "", raw[1024], line[1024], title[LDB_NAME] = "";
    snprintf(dir, sizeof dir, "%s", file);
    char *sl = strrchr(dir, '/');
    if(sl) *sl = 0;
    songs_t s;
    memset(&s, 0, sizeof s);
    long long start = 0;
    int start_known = 0, cue_track = 0, ok = 1, lost = 0;
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
            else lost++;
        } else lost++;
        start = 0; start_known = 0; cue_track = 0; title[0] = 0;
    }
    if(ferror(f)) ok = 0;
    fclose(f);
    sd_io_end();
    if(!name[0]){   /* no #PLAYLIST line: the file's name */
        const char *b = strrchr(file, '/');
        to_utf8(b ? b + 1 : file, name, sizeof name);
        char *dot = strrchr(name, '.');
        if(dot) *dot = 0;
        trim(name);
    }
    long long id = ok ? 0 : -1;
    if(ok && s.n > 0 && name[0]){
        char nm[LDB_NAME];
        snprintf(nm, sizeof nm, "%s", name);
        id = shdb_pl_create(nm, folder_id);
        for(int k = 2; id == 0 && k < 100; k++){   /* the name is taken: "Name (2)", "Name (3)"... */
            snprintf(nm, sizeof nm, "%.*s (%d)", LDB_NAME - 8, name, k);
            id = shdb_pl_create(nm, folder_id);
        }
        if(id == 0) id = -1;
        if(id > 0 && shdb_pl_add(id, s.v, s.n) != s.n){ shdb_pl_delete(id); id = -1; }
        if(id > 0){
            if(name_out && cap > 0) snprintf(name_out, (size_t)cap, "%s", nm);
            if(found) *found = s.n;
            if(missed) *missed = lost;
        }
    }
    songs_free(&s);
    return id;
}

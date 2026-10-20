/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* theme_kits.c - each unique theme's component kit (theme_kit.h): its styler and the builders it replaces. */
#include "theme.h"
#include "theme_kit.h"
#include "screens.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "theme_fonts.h"

lv_obj_t *kit_edge_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb);   /* ui.c */
int ui_screen_backlight_on(void);
int screen_figure_number(int which);

void kit_default_empty_state(kit_empty_kind_t kind, lv_obj_t *message){
    if(strcmp(theme_kit()->id, "default") || !message) return;
    (void)kind;
    lv_obj_set_style_text_font(message, TF(UI_14), 0);
    lv_obj_set_style_text_color(message, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_y(message, 180 - (a.y1 + a.y2) / 2, 0);
}

void kit_default_np_cover_state(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(strcmp(theme_kit()->id, "default") || !cover || !note) return;
    /* The cover itself is the rounded placeholder and remains the art tap target. */
    lv_obj_set_style_bg_color(cover, TC(ART_PLACEHOLDER), 0);
    lv_obj_set_style_radius(cover, 16, 0);
    lv_label_set_text(note, loading ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, TF(UI_32), 0);   /* the built-in symbol font contains the music note */
    lv_obj_set_style_text_color(note, loading ? TC(TEXT_PRIMARY) : TC(ACCENT_PRIMARY), 0);
    if(show) lv_obj_center(note);
}

/* The capsule is a display-only perimeter: parent MAIN_END precedes every child.
 * Five native draw primitives, no extra object or rounded clipping layer.
 * Arc radius is the OUTER radius in LVGL; lengths use its stroke centreline. */
typedef struct {
    int progress;
    lv_color_t accent, color;
    int variant;
} island_progress_t;

static float island_luminance(lv_color_t c){
    float v[3] = {c.red / 255.f, c.green / 255.f, c.blue / 255.f};
    for(int i = 0; i < 3; i++) v[i] = v[i] <= .04045f ? v[i]/12.92f : powf((v[i]+.055f)/1.055f, 2.4f);
    return .2126f*v[0] + .7152f*v[1] + .0722f*v[2];
}
static lv_color_t island_color(lv_color_t accent){
    /* Default dark historically passes arbitrary album colours through. Fit this
     * new indicator to 3:1 on both sides of its border, retaining the hue. */
    float bg[3] = {island_luminance(TC(CANVAS)), island_luminance(TC(SURFACE)),
                   island_luminance(TC(SURFACE_PRESSED))};
    lv_color_t c = accent;
    for(int n = 0; n < 256; n++){
        float l = island_luminance(c);
        int pass = 1;
        for(int i = 0; i < 3; i++) if((fmaxf(l,bg[i])+.05f)/(fminf(l,bg[i])+.05f) < 3.f) pass = 0;
        if(pass) break;
        int r = c.red, g = c.green, b = c.blue;
        if(theme_variant()){ if(r) r--; if(g) g--; if(b) b--; }
        else { if(r < 255) r++; if(g < 255) g++; if(b < 255) b++; }
        c = theme_color_from_rgb((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b);
    }
    return c;
}

/* Visit only the requested interval, in clockwise order from the left end's
 * 127-degree point, matching Now Playing's rotation. Used for drawing AND damage. */
static void island_visit(lv_obj_t *pill, float from, float to, lv_layer_t *layer, lv_color_t color){
    if(to <= from) return;
    lv_area_t a; lv_obj_get_coords(pill, &a);
    int radius = lv_obj_get_height(pill)/2;
    int straight = lv_obj_get_width(pill) - 2*radius;
    if(radius < 3 || straight < 0) return;
    float unit = (radius - 1.5f) * (float)M_PI / 180.f;
    float lengths[5] = {143*unit, straight, 180*unit, straight, 37*unit};
    float total = 2*straight + 360*unit, cursor = 0;
    from *= total; to *= total;
    for(int i = 0; i < 5; i++){
        float lo = fmaxf(from-cursor, 0), hi = fminf(to-cursor, lengths[i]);
        cursor += lengths[i];
        if(hi <= lo) continue;
        lv_area_t damage;
        if(i == 1 || i == 3){
            int x = i == 1 ? a.x1+radius : a.x2+1-radius;
            int y = i == 1 ? a.y1+1 : a.y2-1;
            int sign = i == 1 ? 1 : -1;
            lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
            d.p1 = (lv_point_precise_t){x+sign*(int)lroundf(lo), y};
            d.p2 = (lv_point_precise_t){x+sign*(int)lroundf(hi), y};
            if(d.p1.x == d.p2.x) continue;
            d.width = 3; d.color = color; d.opa = LV_OPA_COVER;
            d.round_start = d.round_end = 1;
            damage = (lv_area_t){LV_MIN(d.p1.x,d.p2.x)-3, y-3, LV_MAX(d.p1.x,d.p2.x)+3, y+3};
            if(layer) lv_draw_line(layer, &d);
        } else {
            int start = i == 0 ? 127 : i == 2 ? 270 : 90;
            lv_draw_arc_dsc_t d; lv_draw_arc_dsc_init(&d);
            d.center = (lv_point_t){i == 2 ? a.x2+1-radius : a.x1+radius, a.y1+radius};
            d.radius = radius; d.width = 3; d.color = color;
            d.opa = LV_OPA_COVER; d.rounded = 1;
            d.start_angle = start + (int)lroundf(lo/unit);
            d.end_angle = start + (int)lroundf(hi/unit);
            if(d.start_angle == d.end_angle) continue;
            lv_draw_arc_get_area(d.center.x, d.center.y, d.radius,
                                 d.start_angle, d.end_angle, d.width, true, &damage);
            if(layer) lv_draw_arc(layer, &d);
        }
        if(!layer) lv_obj_invalidate_area(pill, &damage);
    }
}
static void island_event(lv_event_t *e){
    island_progress_t *s = lv_event_get_user_data(e);
    if(lv_event_get_code(e) == LV_EVENT_DELETE){ free(s); return; }
    if(lv_event_get_code(e) == LV_EVENT_DRAW_MAIN_END)
        island_visit(lv_event_get_target(e), 0, s->progress/1000.f, lv_event_get_layer(e), s->color);
}
void kit_default_home_progress(lv_obj_t *pill, int progress, int have_track, lv_color_t accent){
    if(strcmp(theme_kit()->id, "default") || !pill) return;
    island_progress_t *s = NULL;
    for(uint32_t i = 0; i < lv_obj_get_event_count(pill); i++){
        lv_event_dsc_t *d = lv_obj_get_event_dsc(pill, i);
        if(lv_event_dsc_get_cb(d) == island_event){ s = lv_event_dsc_get_user_data(d); break; }
    }
    if(!s){
        s = calloc(1, sizeof(*s));
        if(!s) return;
        s->variant = -1;
        lv_obj_add_event_cb(pill, island_event, LV_EVENT_ALL, s);
    }
    progress = have_track ? LV_CLAMP(0, progress, 1000) : 0;
    int recolor = s->variant != theme_variant() || !lv_color_eq(s->accent, accent);
    if(recolor){ s->accent = accent; s->color = island_color(accent); s->variant = theme_variant(); }
    int old = s->progress;
    s->progress = progress;
    if(recolor) island_visit(pill, 0, LV_MAX(old,progress)/1000.f, NULL, s->color);
    else if(old != progress) island_visit(pill, LV_MIN(old,progress)/1000.f, LV_MAX(old,progress)/1000.f, NULL, s->color);
}


/* big words and numbers set in Default's large faces (the volume readout, the album-wall letter): the theme's own
 * largest text face instead. Glyph-only labels (icons) keep the icon-bearing face. */
static int is_large_default_text(lv_obj_t *o){
    const char *t = lv_label_get_text(o);
    if(!t || !*t || ((unsigned char)t[0] == 0xEF && strlen(t) == 3)) return 0;
    const lv_font_t *f = lv_obj_get_style_text_font(o, 0);
    return f == theme_font(THEME_FONT_UI_28) || f == theme_font(THEME_FONT_UI_32) || f == theme_font(THEME_FONT_UI_36)
        || f == theme_font(THEME_FONT_UI_40);
}

/* a clock string ("11:33", "5:53 PM") as its time and its meridiem ("" in 24-hour mode) */
static void clock_parts(const char *t, char *hm, int hcap, char *mer, int mcap){
    const char *sp = t ? strchr(t, ' ') : NULL;
    int n = sp ? (int)(sp - t) : (t ? (int)strlen(t) : 0);
    if(n >= hcap) n = hcap - 1;
    memcpy(hm, t ? t : "", (size_t)n); hm[n] = 0;
    lv_snprintf(mer, mcap, "%s", sp ? sp + 1 : "");
}
static lv_obj_t *mer_label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c){
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    kit_keep(l);
    return l;
}
/* Every theme with the round seek ring puts Now Playing's play-order and heart (or moon) buttons exactly where the
 * default theme does (ui.c: TOP_MID +-118, y 126, glyph not re-centred) - owner's rule 2026-09-29; the times lifted
 * clear of the rim */
#define NP_SIDES_Y 126
static void np_sides_on_cover(const np_parts_t *p, int times_y){
    lv_obj_update_layout(p->root);
    lv_obj_t *k[3] = { p->mode, p->fav, p->sleep };
    for(int i = 0; i < 3; i++) lv_obj_set_y(k[i], NP_SIDES_Y);
    if(times_y > 0){ lv_obj_set_y(p->elapsed, times_y); lv_obj_set_y(p->sep, times_y); lv_obj_set_y(p->remain, times_y); }
}
/* Now Playing's title/artist/album rows moved as a block (y of each label's top): used where a theme's play key sits
 * higher than the default's, so the album line clears it (tests/host/np_overlap.py checks the ink clearance) */
static void np_text_rows(const np_parts_t *p, int title_y, int artist_y, int album_y){
    lv_obj_set_y(p->title, title_y);
    lv_obj_set_y(p->artist, artist_y);
    lv_obj_set_y(p->album, album_y);
}
/* the unique themes keep their own backgrounds: no blurred cover behind Home and Now Playing */
static void own_backgrounds(void){ home_set_backdrop_enabled(0); ui_np_backdrop_enabled(0); }

/* Home's Search: the drawn magnifier replaced by the icon glyph in one colour (for filled or small buttons) */
static void home_search_glyph(lv_obj_t *btn, lv_color_t c){
    for(uint32_t i = 0; i < lv_obj_get_child_count(btn); i++) lv_obj_add_flag(lv_obj_get_child(btn, (int32_t)i), LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *mg = lv_label_create(btn);
    lv_label_set_text(mg, "\xEF\x80\x82");                                   /* f002 search */
    lv_obj_set_style_text_font(mg, TF(ICON_20), 0);
    lv_obj_set_style_text_color(mg, c, 0);
    lv_obj_center(mg);
}
/* Home's Library pill resized by a theme: its icon and word laid out again inside the new width */
static void home_library_fit(lv_obj_t *btn){
    lv_obj_t *icon = NULL, *word = NULL;
    for(uint32_t i = 0; i < lv_obj_get_child_count(btn); i++){
        lv_obj_t *c = lv_obj_get_child(btn, (int32_t)i);
        if(!lv_obj_check_type(c, &lv_label_class)) continue;
        if(strlen(lv_label_get_text(c)) == 3) icon = c; else word = c;
    }
    if(icon) lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);
    if(word){ lv_obj_align(word, LV_ALIGN_LEFT_MID, 46, 0); }
}

/* ================================================================ hi-fi
 * A 1980s hi-fi deck: flat faceplate rows with hairlines, machined plates (6 px corners, a hairline edge) for buttons
 * and popups, spaced small-caps titles, an LCD clock whose unlit segments glow faintly behind the time. */
static void hifi_led_scroll_event(lv_event_t *e);
static void hifi_led_scroll_hooks(lv_obj_t *row, lv_obj_t *led, int on){
    for(lv_obj_t *p = row; p; p = lv_obj_get_parent(p)){
        if(!lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) continue;
        lv_obj_remove_event_cb_with_user_data(p, hifi_led_scroll_event, led);
        if(on) lv_obj_add_event_cb(p, hifi_led_scroll_event, LV_EVENT_SCROLL_BEGIN, led);
    }
}
static void hifi_led_event(lv_event_t *e){
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *led = lv_event_get_user_data(e);
    if(code == LV_EVENT_DELETE){
        hifi_led_scroll_hooks(lv_event_get_target(e), led, 0);
        return;
    }
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    hifi_led_scroll_hooks(lv_event_get_target(e), led, code == LV_EVENT_PRESSED);
    lv_obj_set_style_bg_color(led, code == LV_EVENT_PRESSED ? TC(ACCENT_PRIMARY) : TC(BORDER_STRONG), 0);
}
static void hifi_led_scroll_event(lv_event_t *e){
    lv_obj_t *led = lv_event_get_user_data(e);
    lv_obj_set_style_bg_color(led, TC(BORDER_STRONG), 0);
}
static void hifi_led_attach(lv_obj_t *led){
    lv_obj_t *row = lv_obj_get_parent(led);
    for(uint32_t i = 0; i < lv_obj_get_event_count(row); i++){
        lv_event_dsc_t *d = lv_obj_get_event_dsc(row, i);
        if(lv_event_dsc_get_cb(d) == hifi_led_event && lv_event_dsc_get_user_data(d) == led) return;
    }
    lv_obj_add_event_cb(row, hifi_led_event, LV_EVENT_ALL, led);
}
static void hifi_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_TITLE:
        lv_obj_set_style_text_letter_space(o, 2, 0);
        lv_obj_set_style_text_color(o, TC(TEXT_SECONDARY), 0);
        break;
    case KIT_ROW:
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(o, 6, 0);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, TC(BORDER), 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE_RAISED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        break;
    case KIT_ROW_TEXT:
        lv_obj_set_style_text_letter_space(o, 1, 0);
        break;
    case KIT_ROW_CHEVRON:                             /* the drill-down mark becomes an unlit LED */
        lv_label_set_text(o, "");
        lv_obj_set_size(o, 6, 6);
        lv_obj_set_style_bg_color(o, TC(BORDER_STRONG), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(o, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
        hifi_led_attach(o);
        break;
    case KIT_BUTTON: case KIT_CARD: case KIT_POPUP:
        lv_obj_set_style_radius(o, r == KIT_POPUP ? 8 : 6, 0);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        if(r == KIT_BUTTON){ lv_obj_t *l = lv_obj_get_child(o, 0); if(l) lv_obj_set_style_text_letter_space(l, 1, 0); }
        break;
    case KIT_TOGGLE:
        lv_obj_set_style_radius(o, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(o, 4, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 3, LV_PART_KNOB);
        break;
    case KIT_SLIDER:
        lv_obj_set_style_radius(o, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(o, 2, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 3, LV_PART_KNOB);
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        lv_obj_set_style_radius(o, 6, 0);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 4, LV_PART_ITEMS);
        lv_obj_set_style_border_width(o, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(o, TC(BORDER_STRONG), LV_PART_ITEMS);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    default: break;
    }
}
static void plate(lv_obj_t *o, int radius){
    kit_keep(o);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, TC(SURFACE_RAISED), LV_STATE_PRESSED);
}
static lv_obj_t *g_hifi_clock, *g_hifi_mer, *g_hifi_ghost;
static void hifi_clock(const char *t){
    if(!g_hifi_clock) return;
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    lv_label_set_text(g_hifi_clock, hm);
    lv_label_set_text(g_hifi_mer, mer);
    /* 12-hour: the digits and an AM/PM column beside them are centred as ONE group, so "PM" never sits on the last
     * digit (it was pinned to the well's corner, over the 4th digit's lower segments); 24-hour: the digits alone */
    lv_obj_t *well = lv_obj_get_parent(g_hifi_ghost);
    lv_obj_update_layout(g_hifi_ghost);
    int32_t gw = lv_obj_get_width(g_hifi_ghost), mw = 0;
    if(mer[0]){ lv_point_t sz; lv_text_get_size(&sz, "PM", lv_obj_get_style_text_font(g_hifi_mer, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE); mw = sz.x + 4; }
    /* the LCD well grows by the AM/PM column in 12-hour mode (the 88:88 digits alone nearly fill 232 px); still well
     * inside the round screen at this height */
    lv_obj_set_width(well, mer[0] ? gw + mw + 24 : 232);
    lv_obj_update_layout(well);
    int32_t well_w = lv_obj_get_content_width(well);
    lv_obj_align(g_hifi_ghost, LV_ALIGN_LEFT_MID, (well_w - gw - mw) / 2, 0);
    lv_obj_update_layout(g_hifi_clock);
    lv_obj_align_to(g_hifi_clock, g_hifi_ghost, LV_ALIGN_RIGHT_MID, 0, 0);   /* right-aligned: "6:02" lights the last digits */
    if(mer[0]) lv_obj_align_to(g_hifi_mer, g_hifi_ghost, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);   /* beside the minutes, near their foot */
}
static void hifi_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_set_pos(p->status, 0, 38);
    lv_obj_set_style_text_color(p->status, TC(TEXT_MUTED), 0);
    /* the LCD: a dark well; the unlit segments ("88:88") sit behind the time in the ghost colour */
    lv_obj_t *well = lv_obj_create(p->root);
    lv_obj_remove_style_all(well);
    lv_obj_clear_flag(well, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(well, 232, 92);
    lv_obj_align(well, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_radius(well, 10, 0);
    lv_obj_set_style_bg_color(well, TC(SURFACE), 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
    lv_obj_move_to_index(well, lv_obj_get_index(p->clock));
    lv_obj_t *ghost = lv_label_create(well);
    lv_label_set_text(ghost, "88:88");
    lv_obj_set_style_text_font(ghost, TF(CLOCK), 0);
    lv_obj_set_style_text_color(ghost, TC(DECOR_1), 0);
    lv_obj_center(ghost);
    lv_obj_set_parent(p->clock, well);
    lv_obj_set_width(p->clock, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(p->clock, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_text_align(p->clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(p->clock, ghost, LV_ALIGN_RIGHT_MID, 0, 0);   /* digits line up with their unlit segments */
    g_hifi_clock = p->clock; g_hifi_ghost = ghost;
    g_hifi_mer = mer_label(well, TF(UI_12), TC(ACCENT_PRIMARY));   /* 12-hour: AM / PM under the minutes */
    hifi_clock(lv_label_get_text(p->clock));   /* lays out the digits + AM/PM group */
    lv_obj_set_pos(p->date, 0, 162);
    lv_obj_set_style_text_letter_space(p->date, 4, 0);
    lv_obj_set_style_text_color(p->date, TC(TEXT_SECONDARY), 0);
    /* Library + Search side by side on plates */
    /* Library + Search share the now-playing plate's edges (60..300) */
    lv_obj_set_pos(p->library, 60, 220); lv_obj_set_size(p->library, 184, 40);
    plate(p->library, 6);
    home_library_fit(p->library);
    lv_obj_set_pos(p->search, 252, 220); lv_obj_set_size(p->search, 48, 40);
    plate(p->search, 6);
    /* now playing: a plate with an amber indicator where the art would be */
    lv_obj_set_pos(p->np, 60, 268); lv_obj_set_size(p->np, 240, 46);
    plate(p->np, 8);
    lv_obj_set_style_bg_opa(p->np, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(p->np, LV_OPA_COVER, 0);
    lv_obj_set_pos(p->np_thumb, 10, 15); lv_obj_set_size(p->np_thumb, 16, 16);
    home_set_art_enabled(0);                          /* an indicator, not art */
    lv_obj_add_flag(p->np_glyph, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(p->np_title, 36, 7);  lv_obj_set_width(p->np_title, 150);
    lv_obj_set_pos(p->np_artist, 36, 27); lv_obj_set_width(p->np_artist, 150);
    lv_obj_set_pos(p->pp, 190, 1); lv_obj_set_size(p->pp, 44, 44);
    lv_obj_set_style_text_color(p->pp_label, TC(ACCENT_PRIMARY), 0);
}
static void hifi_np(const np_parts_t *p){
    own_backgrounds();
    /* The real ring remains the seek geometry; its fill is drawn by the bezel ticks. */
    lv_obj_set_style_arc_width(p->ring, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->ring, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->ring, TC(BORDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(p->ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(p->ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(p->ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(p->ring, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(p->cover, 6, 0);
    lv_obj_set_style_border_width(p->cover, 1, 0);
    lv_obj_set_style_border_color(p->cover, TC(BORDER_STRONG), 0);
    lv_obj_set_style_border_opa(p->cover, LV_OPA_COVER, 0);
    lv_obj_t *t[2] = { p->elapsed, p->remain };
    for(int i = 0; i < 2; i++){
        lv_obj_set_style_text_font(t[i], &diskos_hifi_seg_20, 0);
        lv_obj_set_style_text_color(t[i], TC(ACCENT_PRIMARY), 0);
        lv_obj_align(t[i], LV_ALIGN_TOP_MID, i ? 61 : -61, 310);   /* up and in: clear of the rim and of the plate */
    }
    lv_obj_add_flag(p->sep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(p->pp, 50, 50);
    lv_obj_set_style_radius(p->pp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p->pp, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(p->pp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->pp, 1, 0);
    lv_obj_set_style_border_color(p->pp, TC(BORDER_STRONG), 0);
    lv_obj_set_style_text_align(p->pp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p->pp, 9, 0);
    lv_obj_align(p->pp, LV_ALIGN_TOP_MID, 0, 262);
    lv_obj_align(p->prev, LV_ALIGN_TOP_MID, -66, 272);
    lv_obj_align(p->next, LV_ALIGN_TOP_MID, 66, 272);
    lv_obj_set_ext_click_area(p->pp, 4);                     /* the plate is the target; no reach into prev/next */
    np_text_rows(p, 193, 221, 240);                          /* the album line clears the plate at 262 */
    np_sides_on_cover(p, 0);
}
static void hifi_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    (void)toggle;
    /* a square key on the faceplate; a lit key glows amber through its legend */
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_bg_color(c, TC(SURFACE_RAISED), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, on ? TC(ACCENT_PRIMARY) : TC(BORDER_STRONG), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    if(g) lv_obj_set_style_text_color(g, on ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY), 0);
    if(cap){ lv_obj_set_style_text_color(cap, on ? TC(ACCENT_PRIMARY) : TC(TEXT_MUTED), 0);
             lv_obj_set_style_text_letter_space(cap, 1, 0); }
}
static void hifi_empty(kit_empty_kind_t kind, lv_obj_t *message){
    (void)kind;
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(USER_16), 0);
    lv_obj_set_style_text_color(message, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_y(message, 180 - (a.y1 + a.y2) / 2, 0);
}
static void hifi_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    lv_obj_set_style_bg_color(cover, TC(SURFACE), 0);
    lv_obj_set_style_radius(cover, 2, 0);
    if(!show) return;
    lv_label_set_text(note, loading ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, TF(UI_32), 0);   /* LVGL's symbol fallback has the music note */
    lv_obj_set_style_text_color(note, TC(ACCENT_PRIMARY), 0);
    lv_obj_center(note);
}
const theme_kit_t theme_kit_hifi = { .id = "hi-fi", .header = kit_edge_header, .style = hifi_style, .home = hifi_home,
                                     .home_clock = hifi_clock,
                                     .nowplaying = hifi_np, .qs_tile = hifi_qs_tile,
                                     .empty_state = hifi_empty, .np_cover_state = hifi_cover };

/* ================================================================ terminal
 * A command line: one monospace face, square corners, ~/path titles, rows that highlight with a cursor bar, bracketed
 * keys, [x] checkboxes in Quick Settings, and a Now Playing that is a readout with a straight seek bar (no ring). */
/* a plain surface fill (not an accent, brand or status colour): themes may redraw it their own way */
static int is_neutral_fill(lv_obj_t *o){
    lv_color_t c = lv_obj_get_style_bg_color(o, 0);
    static const theme_color_role_t N[] = { THEME_CLR_SURFACE, THEME_CLR_SURFACE_RAISED, THEME_CLR_SURFACE_STRONG,
        THEME_CLR_CONTROL_OFF, THEME_CLR_KEY_SURFACE, THEME_CLR_SURFACE_SELECTED, THEME_CLR_LIST_PRESSED, THEME_CLR_CANVAS };
    for(unsigned i = 0; i < sizeof N / sizeof N[0]; i++) if(lv_color_eq(c, theme_color(N[i]))) return 1;
    return 0;
}
static void term_square(lv_obj_t *o){ lv_obj_set_style_radius(o, 0, 0); }
static const char term_cursor_tag;
static lv_timer_t *term_cursor_timer;
static lv_obj_t *term_cursor_active;
static lv_obj_t *term_cursor_for(lv_obj_t *root){
    if(!root) return NULL;
    for(uint32_t i = 0; i < lv_obj_get_child_count(root); i++){
        lv_obj_t *c = lv_obj_get_child(root, (int32_t)i);
        if(lv_obj_get_user_data(c) == (void *)&term_cursor_tag) return c;
    }
    return NULL;
}
static void term_cursor_place(lv_obj_t *title){
    lv_obj_t *cur = term_cursor_for(lv_obj_get_parent(title));
    if(!cur) return;
    const lv_font_t *f = lv_obj_get_style_text_font(title, 0);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(title), f, lv_obj_get_style_text_letter_space(title, 0),
                     0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int room = lv_obj_get_width(title);
    if(sz.x > room) sz.x = room;
    int x = lv_obj_get_x(title) + (room + sz.x) / 2 + 3;
    if(x > 318) x = 318;
    lv_obj_set_pos(cur, x, lv_obj_get_y(title) + (lv_obj_get_height(title) - 16) / 2);
}
static void term_cursor_resize(lv_event_t *e){ term_cursor_place(lv_event_get_target(e)); }
static void term_cursor_tick(lv_timer_t *timer){
    (void)timer;
    lv_obj_t *root = screen_get_root(screen_current());
    if(!ui_screen_backlight_on() || !term_cursor_active || !root ||
       lv_obj_get_parent(term_cursor_active) != root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)){
        lv_timer_pause(term_cursor_timer); term_cursor_active = NULL; return;
    }
    if(lv_obj_has_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN);
}
void kit_cursor_sync(int backlight_on){
    if(strcmp(theme_kit()->id, "terminal")) return;
    lv_obj_t *cur = backlight_on ? term_cursor_for(screen_get_root(screen_current())) : NULL;
    if(!cur){ if(term_cursor_timer) lv_timer_pause(term_cursor_timer); term_cursor_active = NULL; return; }
    if(cur == term_cursor_active && term_cursor_timer && !lv_timer_get_paused(term_cursor_timer)) return;
    term_cursor_active = cur;
    lv_obj_remove_flag(cur, LV_OBJ_FLAG_HIDDEN); /* every entry/wake starts ON */
    if(!term_cursor_timer) term_cursor_timer = lv_timer_create(term_cursor_tick, 550, NULL);
    lv_timer_reset(term_cursor_timer);
    lv_timer_resume(term_cursor_timer);
}
#ifdef DISKOS_HOST
int kit_cursor_host_paused(void){ return !term_cursor_timer || lv_timer_get_paused(term_cursor_timer); }
int kit_cursor_host_on(void){ return term_cursor_active && !lv_obj_has_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN); }
void kit_cursor_host_phase(int on){
    if(!term_cursor_active) return;
    if(on) lv_obj_remove_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(term_cursor_active, LV_OBJ_FLAG_HIDDEN);
    if(term_cursor_timer) lv_timer_reset(term_cursor_timer);
}
#endif
static void term_title_cursor(lv_obj_t *title){
    lv_obj_t *root = lv_obj_get_parent(title);
    if(!root || term_cursor_for(root)) return;
    lv_obj_t *cur = lv_obj_create(root);
    lv_obj_remove_style_all(cur); kit_keep(cur);
    lv_obj_set_user_data(cur, (void *)&term_cursor_tag);
    lv_obj_clear_flag(cur, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cur, 8, 16);
    lv_obj_set_style_bg_color(cur, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(title, term_cursor_resize, LV_EVENT_SIZE_CHANGED, NULL);
    term_cursor_place(title);
}
static void term_row_value(lv_obj_t *row, lv_obj_t *title, lv_obj_t *sub, lv_obj_t *value, kit_value_kind_t kind){
    (void)row;
    if(kind != KIT_VALUE_DURATION) return;
    lv_obj_set_pos(value, 198, 17); lv_obj_set_size(value, 58, 18);
    lv_obj_set_style_text_font(value, TF(UI_14), 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(value, TC(TEXT_SECONDARY), 0);
    lv_obj_set_width(title, 178); /* reserve a fixed column before long text is dotted */
    if(sub) lv_obj_set_width(sub, 178);
}
/* a switch drawn as "[x]" / "[ ]" at draw time, so every state change shows (the user's and the code's) */
static void term_switch_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.font = TF(UI_16);
    d.color = lv_obj_has_state(o, LV_STATE_CHECKED) ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY);
    d.text = lv_obj_has_state(o, LV_STATE_CHECKED) ? "[x]" : "[ ]";
    d.align = LV_TEXT_ALIGN_RIGHT;
    lv_area_t a; lv_obj_get_coords(o, &a);
    int lh = lv_font_get_line_height(d.font);
    a.y1 = (a.y1 + a.y2) / 2 - lh / 2; a.y2 = a.y1 + lh;
    lv_draw_label(layer, &d, &a);
}
static void term_style(lv_obj_t *o, kit_role_t r){
    if(lv_color_eq(lv_obj_get_style_text_color(o, 0), TC(STATUS_INFO)))
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
    switch(r){
    case KIT_TITLE:
        term_title_cursor(o);
        break;
    case KIT_ROW:
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_LEFT, LV_STATE_PRESSED);   /* the cursor bar */
        lv_obj_set_style_border_width(o, 4, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
        break;
    case KIT_ROW_CHEVRON:
        lv_label_set_text(o, "/");
        lv_obj_set_style_text_font(o, TF(UI_16), 0);
        lv_obj_set_style_text_color(o, TC(TEXT_MUTED), 0);
        break;
    case KIT_BUTTON: case KIT_ICON_BUTTON:
        term_square(o);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);      /* a key is its bracket box, not a fill */
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(TEXT_SECONDARY), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        lv_obj_set_style_bg_color(o, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        break;
    case KIT_CARD:                                       /* list cards are flat lines of text, like the rows */
        term_square(o);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(o, 0, 0);
        }
        break;
    case KIT_POPUP:                                      /* a popup is a framed window */
        term_square(o);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){
            lv_obj_set_style_bg_color(o, TC(CANVAS), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);   /* a see-through window let the screen's text through a toast */
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_TOGGLE:
        for(int p = 0; p < 3; p++){
            lv_part_t part = p == 0 ? LV_PART_MAIN : p == 1 ? LV_PART_INDICATOR : LV_PART_KNOB;
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, part);
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, part | LV_STATE_CHECKED);
            lv_obj_set_style_border_width(o, 0, part);
            lv_obj_set_style_shadow_width(o, 0, part);
        }
        lv_obj_add_event_cb(o, term_switch_draw, LV_EVENT_DRAW_MAIN_END, NULL);
        break;
    case KIT_SLIDER: case KIT_BAR:
        for(int p = 0; p < 3; p++){
            lv_part_t part = p == 0 ? LV_PART_MAIN : p == 1 ? LV_PART_INDICATOR : LV_PART_KNOB;
            lv_obj_set_style_radius(o, 0, part);
        }
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        term_square(o);
        lv_obj_set_style_radius(o, 0, LV_PART_SELECTED);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 0, LV_PART_ITEMS);
        lv_obj_set_style_border_width(o, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(o, TC(BORDER), LV_PART_ITEMS);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_ITEMS);
        break;
    case KIT_TEXT: case KIT_ROW_SUB:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        if(!strcmp(lv_label_get_text(o), LV_SYMBOL_OK)){                 /* a tick is a star, as a shell marks it */
            lv_label_set_text(o, "*");
            lv_obj_set_style_text_font(o, TF(UI_20), 0);
        }
        break;
    default: break;
    }
}
/* the title as a working directory ("~/library"), Back as a "<" key on the left edge */
static lv_obj_t *term_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb){
    lv_obj_t *t = kit_edge_header(root, title, back_cb);
    lv_obj_set_style_text_color(t, TC(TEXT_SECONDARY), 0);
    return t;
}
static void term_title(lv_obj_t *label, const char *text){   /* "~/library" (already lower case) */
    char path[160]; int n = lv_snprintf(path, sizeof path, "~/%s", text);
    for(int i = 2; i < n && i < (int)sizeof path; i++) if(path[i] == ' ') path[i] = '_';
    lv_label_set_text(label, path);
    term_cursor_place(label);
}
static lv_obj_t *term_text(lv_obj_t *parent, const char *txt, lv_color_t c, int x, int y){
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, TF(UI_14), 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}
static void term_key(lv_obj_t *o){            /* a bracketed key: square box, 1 px border */
    kit_keep(o);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, TC(TEXT_SECONDARY), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
}
static lv_obj_t *g_term_clock, *g_term_cur, *g_term_mer;
static void term_clock(const char *t){
    if(!g_term_clock) return;
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    lv_label_set_text(g_term_clock, hm);
    char low[8]; lv_label_set_text(g_term_mer, theme_lower(low, sizeof low, mer));
    lv_point_t tw; lv_text_get_size(&tw, hm, TF(CLOCK), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_align(g_term_cur, LV_ALIGN_TOP_MID, tw.x / 2 + 14, 84);             /* the cursor right after the time */
    lv_obj_align_to(g_term_mer, g_term_cur, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);
}
static void term_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_t *banner = term_text(p->root, "diskOS 1.2 | tty0", TC(TEXT_MUTED), 0, 30);
    lv_obj_set_width(banner, 360); lv_obj_set_style_text_align(banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(p->status, 0, 50);
    lv_obj_set_style_text_color(p->status, TC(TEXT_MUTED), 0);
    lv_obj_set_pos(p->clock, 0, 70);
    lv_obj_t *cur = lv_obj_create(p->root);                  /* the block cursor after the time */
    lv_obj_remove_style_all(cur);
    lv_obj_clear_flag(cur, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cur, 12, 52);
    g_term_clock = p->clock; g_term_cur = cur;
    g_term_mer = mer_label(p->root, TF(UI_16), TC(TEXT_SECONDARY));
    term_clock(lv_label_get_text(p->clock));
    lv_obj_set_style_bg_color(cur, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
    term_text(p->root, "$ date", TC(TEXT_MUTED), 62, 148);
    lv_obj_set_style_text_align(p->date, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(p->date, 62, 164); lv_obj_set_width(p->date, 240);
    lv_obj_set_style_text_color(p->date, TC(TEXT_PRIMARY), 0);
    term_text(p->root, "$ now", TC(TEXT_MUTED), 62, 208);
    /* $ now: the now-playing line, with its play key. Everything hangs from x 62 and ends at 298 (mirrored) */
    lv_obj_set_pos(p->np, 54, 224); lv_obj_set_size(p->np, 252, 44);
    kit_keep(p->np);
    lv_obj_set_style_radius(p->np, 0, 0);
    lv_obj_set_style_bg_opa(p->np, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p->np, 0, 0);
    lv_obj_set_style_bg_color(p->np, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(p->np, LV_OPA_COVER, LV_STATE_PRESSED);
    home_set_art_enabled(0);
    lv_obj_set_pos(p->np_thumb, 8, 5); lv_obj_set_size(p->np_thumb, 18, 20);   /* holds the 19 px '>' line */
    lv_obj_set_style_radius(p->np_thumb, 0, 0);
    lv_obj_set_style_bg_opa(p->np_thumb, LV_OPA_TRANSP, 0);
    lv_label_set_text(p->np_glyph, ">");
    lv_obj_set_style_text_font(p->np_glyph, theme_font_size() == 2 ? TF(UI_14) : TF(UI_16), 0);
    lv_obj_set_style_text_color(p->np_glyph, TC(ACCENT_PRIMARY), 0);
    lv_obj_center(p->np_glyph);
    lv_obj_set_pos(p->np_title, 30, 4);  lv_obj_set_width(p->np_title, 164);
    lv_obj_set_pos(p->np_artist, 30, 24); lv_obj_set_width(p->np_artist, 164);
    lv_obj_set_pos(p->pp, 200, 4); lv_obj_set_size(p->pp, 44, 38);
    term_key(p->pp);
    /* [ library ] [ find ] */
    lv_obj_set_pos(p->library, 62, 276); lv_obj_set_size(p->library, 136, 38);
    term_key(p->library);
    lv_obj_t *ll = NULL; for(uint32_t i = 0; i < lv_obj_get_child_count(p->library); i++){
        lv_obj_t *c = lv_obj_get_child(p->library, (int32_t)i);
        if(lv_obj_check_type(c, &lv_label_class)){ if(!ll && strlen(lv_label_get_text(c)) == 3) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN); else ll = c; } }
    if(ll){ lv_label_set_text(ll, "library"); lv_obj_center(ll); }
    lv_obj_set_pos(p->search, 208, 276); lv_obj_set_size(p->search, 90, 38);
    term_key(p->search);
    for(uint32_t i = 0; i < lv_obj_get_child_count(p->search); i++) lv_obj_add_flag(lv_obj_get_child(p->search, (int32_t)i), LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *fl = lv_label_create(p->search);
    lv_label_set_text(fl, "find");
    lv_obj_set_style_text_font(fl, TF(UI_16), 0);
    lv_obj_set_style_text_color(fl, TC(TEXT_PRIMARY), 0);
    lv_obj_center(fl);
    lv_label_set_text(p->hint, ">");
    lv_obj_set_style_text_font(p->hint, TF(UI_14), 0);
}
static lv_obj_t *g_term_state;   /* the "> playing" readout line: follows the real play state */
static void term_np_state(int playing, int have_track){
    if(!g_term_state) return;
    const char *t = !have_track ? "> no track" : playing ? "> playing" : "> paused";
    if(strcmp(lv_label_get_text(g_term_state), t)) lv_label_set_text(g_term_state, t);
}
static void term_np(const np_parts_t *p){
    own_backgrounds();
    /* a readout: no ring, no art; the seek is a straight bar */
    lv_obj_add_flag(p->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    g_term_state = term_text(p->root, "> playing", TC(TEXT_MUTED), 60, 52);   /* set from the play state (np_state) */
    static const char *const K[3] = { "title", "artist", "album" };
    lv_obj_t *v[3] = { p->title, p->artist, p->album };
    for(int i = 0; i < 3; i++){
        term_text(p->root, K[i], TC(TEXT_MUTED), 60, 80 + i * 24);
        lv_obj_set_style_text_align(v[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(v[i], 176);
        /* each value sits on its key's baseline (the faces differ in height) */
        int kh = lv_font_get_line_height(TF(UI_14)), vh = lv_font_get_line_height(i ? TF(USER_14) : TF(USER_16));
        lv_obj_align(v[i], LV_ALIGN_TOP_LEFT, 124, 80 + i * 24 + kh - vh);
        lv_obj_set_style_text_font(v[i], i ? TF(USER_14) : TF(USER_16), 0);
        lv_obj_set_style_text_color(v[i], i ? TC(TEXT_SECONDARY) : TC(TEXT_PRIMARY), 0);
    }
    lv_obj_t *bar = lv_bar_create(p->root);
    lv_obj_set_size(bar, 240, 14);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 164);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, TC(SURFACE_SELECTED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, TC(TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);             /* seek is driven by the NP touch recognizer */
    ui_np_seek_line(bar);
    lv_obj_align(p->elapsed, LV_ALIGN_TOP_LEFT, 60, 184);
    lv_obj_set_style_text_align(p->elapsed, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(p->remain, LV_ALIGN_TOP_RIGHT, -60, 184);
    lv_obj_set_style_text_align(p->remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(p->sep, LV_OBJ_FLAG_HIDDEN);
    /* [<<] [||] [>>] */
    lv_obj_t *tr[3] = { p->prev, p->pp, p->next };
    for(int i = 0; i < 3; i++){
        lv_obj_set_style_text_font(tr[i], TF(UI_20), 0);
        lv_obj_set_style_text_color(tr[i], TC(TEXT_PRIMARY), 0);
        lv_obj_set_style_bg_opa(tr[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(tr[i], 0, 0);
        lv_obj_set_style_border_width(tr[i], 1, 0);
        lv_obj_set_style_border_color(tr[i], i == 1 ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0);
        lv_obj_set_style_border_opa(tr[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(tr[i], 14, 0);
        lv_obj_set_style_pad_ver(tr[i], 8, 0);
        lv_obj_set_size(tr[i], 56, LV_SIZE_CONTENT);          /* equal keys: equal gaps, a symmetric row */
        lv_obj_set_style_text_align(tr[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_hor(tr[i], 0, 0);
        lv_obj_align(tr[i], LV_ALIGN_TOP_MID, (i - 1) * 70, 216);
        lv_obj_set_ext_click_area(tr[i], 4);                 /* the key box is the target; keys never share a touch */
    }
    lv_obj_set_style_text_color(p->pp, TC(ACCENT_PRIMARY), 0);
    /* mode / fav / sleep as a row of keys */
    lv_obj_t *k[3] = { p->mode, p->fav, p->sleep };
    for(int i = 0; i < 3; i++){ lv_obj_set_size(k[i], 50, 40); term_key(k[i]); lv_obj_set_ext_click_area(k[i], 4); }
    /* the moon replaces the heart for audiobooks (ui.c): they share the second slot */
    lv_obj_align(p->mode, LV_ALIGN_TOP_MID, -32, 272);
    lv_obj_align(p->fav, LV_ALIGN_TOP_MID, 32, 272);
    lv_obj_align(p->sleep, LV_ALIGN_TOP_MID, 32, 272);
}
static void term_qs(const qs_parts_t *p){
    lv_obj_t *prompt = term_text(p->root, "$ quick", TC(TEXT_SECONDARY), 0, 26);
    lv_obj_set_width(prompt, 360); lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, 0);
    if(p->grab) lv_obj_add_flag(p->grab, LV_OBJ_FLAG_HIDDEN);       /* the prompt is the handle */
    /* the tiles become a checklist: "[x] wi-fi", one per line */
    lv_obj_update_layout(p->root);
    int y = p->bright ? lv_obj_get_y(p->bright) + 44 : 90;
    if(p->pp) y = lv_obj_get_y(p->pp) + 60;
    for(int i = 0; i < p->n; i++){
        lv_obj_set_size(p->tile[i], 210, 30);
        lv_obj_align(p->tile[i], LV_ALIGN_TOP_MID, 0, y + i * 32);
        lv_obj_t *g = lv_obj_get_child(p->tile[i], 0);
        if(g) lv_obj_align(g, LV_ALIGN_LEFT_MID, 8, 0);
        if(p->cap[i]){
            lv_obj_set_parent(p->cap[i], p->tile[i]);
            lv_obj_align(p->cap[i], LV_ALIGN_LEFT_MID, 56, 0);
            lv_obj_set_style_text_font(p->cap[i], TF(UI_16), 0);
        }
    }
    if(p->bright){ lv_obj_set_style_radius(p->bright, 0, 0); lv_obj_set_style_radius(p->bright, 0, LV_PART_INDICATOR); }
}
static void term_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_bg_color(c, TC(SURFACE_SELECTED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_STATE_PRESSED);
    if(g){
        lv_label_set_text(g, toggle ? (on ? "[x]" : "[ ]") : " > ");
        lv_obj_set_style_text_font(g, TF(UI_16), 0);
        lv_obj_set_style_text_color(g, on ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0);
    }
    if(cap){
        char low[32]; lv_label_set_text(cap, theme_lower(low, sizeof low, lv_label_get_text(cap)));
        lv_obj_set_style_text_color(cap, on ? TC(ACCENT_PRIMARY) : TC(TEXT_PRIMARY), 0);
    }
}
#define TERM_PROMPT LV_OBJ_FLAG_USER_1
static void term_prompt_deleted(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void term_prompt_draw(lv_event_t *e){
    lv_obj_t *message = lv_event_get_target(e);
    const char *saved = lv_obj_get_user_data(message);
    if(!saved || strcmp(saved, lv_label_get_text(message))) return;
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_area_t mark = { a.x1 + 2, a.y1, a.x1 + 30, a.y2 };
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.text = "$ ";
    d.font = TF(UI_16);
    d.color = TC(ACCENT_PRIMARY);
    lv_draw_label(lv_event_get_layer(e), &d, &mark);
}
static void term_empty(kit_empty_kind_t kind, lv_obj_t *message){
    (void)kind;
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(UI_16), 0);
    lv_obj_set_style_text_color(message, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(message, 272);
    lv_obj_set_style_pad_left(message, 24, 0);
    lv_obj_set_style_translate_x(message, 0, 0);
    lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_x(message, 38 - a.x1, 0);
    lv_obj_set_style_translate_y(message, 174 - a.y1, 0);
    /* Draw only for this empty message; a reused Lyrics label sheds the mark when its text changes. */
    if(lv_obj_has_flag(message, TERM_PROMPT)) free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(lv_label_get_text(message)));
    if(!lv_obj_has_flag(message, TERM_PROMPT)){
        lv_obj_add_flag(message, TERM_PROMPT);
        lv_obj_add_event_cb(message, term_prompt_draw, LV_EVENT_DRAW_MAIN_END, NULL);
        lv_obj_add_event_cb(message, term_prompt_deleted, LV_EVENT_DELETE, NULL);
    }
}
const theme_kit_t theme_kit_term = { .id = "terminal", .header = term_header, .style = term_style, .home = term_home,
                                     .home_clock = term_clock,
                                     .nowplaying = term_np, .quicksettings = term_qs, .qs_tile = term_qs_tile,
                                     .empty_state = term_empty, .row_value = term_row_value,
                                     .title = term_title, .np_state = term_np_state, .back_text = "<" };

/* ================================================================ blueprint
 * A technical drawing on drafting paper: thin 1 px line work, square corners, spaced monospace titles with a rule
 * under them, numbered rows with arrow marks, the pressed row outlined in the highlight colour (DECOR_1), crop marks
 * round the cover, and a Home laid out as a figure with crosshairs. */
static void blue_outline(lv_obj_t *o, lv_color_t c){
    kit_keep(o);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, c, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
}
static lv_obj_t *g_bnum_pending[16];   /* lists whose rows this styling pass touched: numbered when it ends */
static int g_bnum_n;
static const char g_bnum_tag = 0;      /* user_data of our number labels */
#define BLUE_SHIFTED  LV_OBJ_FLAG_USER_1   /* a row child moved right for the number (moved back exactly) */
#define BLUE_NARROWED LV_OBJ_FLAG_USER_2   /* ... and narrowed with it */
static void blue_renumber(lv_obj_t *par);
static void blue_queue(lv_obj_t *par);
static void blue_row(lv_obj_t *o){
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(o, 1, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_FULL, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(o, TC(DECOR_1), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
    /* numbered once the styling pass is done with the whole list (blue_pass_end) */
    lv_obj_t *par = lv_obj_get_parent(o);
    if(!par || lv_obj_get_parent(par) == lv_screen_active()) return;   /* a lone control on a screen, not a list */
    blue_queue(par);
}
/* Rows never leave a list one by one here: a list whose rows change is rebuilt (lv_obj_clean, then new rows; fixed
 * lists such as the Settings categories are built once), and new rows are styled, which queues the list again. So no watch on the list is needed (an event callback on it would also
 * make the list itself look tappable to the touch-area rules). The queue lives only within one styling pass. */
static void blue_queue(lv_obj_t *par){
    for(int k = 0; k < g_bnum_n; k++) if(g_bnum_pending[k] == par) return;
    if(g_bnum_n == (int)(sizeof g_bnum_pending / sizeof g_bnum_pending[0])){   /* full: number what is queued now */
        for(int k = 0; k < g_bnum_n; k++) blue_renumber(g_bnum_pending[k]);
        g_bnum_n = 0;
    }
    g_bnum_pending[g_bnum_n++] = par;
}
static int blue_is_row(lv_obj_t *c){ return lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_width(c) >= 200; }
static lv_obj_t *blue_number_of(lv_obj_t *row){
    for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++){
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        if(lv_obj_get_user_data(c) == (void *)&g_bnum_tag) return c;
    }
    return NULL;
}
static void blue_shift_left_half(lv_obj_t *row){           /* the row's left half moves for the number (text, icons, dots) */
    for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++){
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        if(lv_obj_get_user_data(c) == (void *)&g_bnum_tag || lv_obj_get_x(c) > lv_obj_get_width(row) / 2) continue;
        lv_obj_set_x(c, lv_obj_get_x(c) + 26);
        lv_obj_add_flag(c, BLUE_SHIFTED);
        int32_t w = lv_obj_get_style_width(c, 0);
        if(w != LV_SIZE_CONTENT && !LV_COORD_IS_PCT(w) && w > 86){ lv_obj_set_width(c, w - 26); lv_obj_add_flag(c, BLUE_NARROWED); }
    }
}
static void blue_unshift(lv_obj_t *row){                   /* exactly what blue_shift_left_half did, undone */
    for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++){
        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
        if(!lv_obj_has_flag(c, BLUE_SHIFTED)) continue;
        lv_obj_set_x(c, lv_obj_get_x(c) - 26);
        if(lv_obj_has_flag(c, BLUE_NARROWED)) lv_obj_set_width(c, lv_obj_get_style_width(c, 0) + 26);
        lv_obj_remove_flag(c, BLUE_SHIFTED | BLUE_NARROWED);
    }
}
/* The row numbers, drawing-index style, for a real list (three or more rows). A list with any two-line row (a title and
 * a subtitle) is unnumbered as a whole: the number column would cut the subtitles (blueprint's small face is
 * monospace). One walk over the list: every row's number is added, updated or removed so the list always agrees,
 * however its rows arrived (all at once or in batches). */
int playlistview_row_pos(lv_obj_t *row, int *rows, int *two_line);   /* playlistview.c */
static void blue_renumber(lv_obj_t *par){
    uint32_t cnt = lv_obj_get_child_count(par);
    int rows = 0, two_line = 0;
    /* a playlist streams its rows in: its shape and numbers come from the whole playlist, not the rows built yet */
    int streamed = par == playlistview_scroller();
    if(streamed) playlistview_row_pos(NULL, &rows, &two_line);
    else for(uint32_t j = 0; j < cnt; j++){
        lv_obj_t *row = lv_obj_get_child(par, (int32_t)j);
        if(!blue_is_row(row)) continue;
        rows++;
        int texts = 0;
        for(uint32_t i = 0; i < lv_obj_get_child_count(row); i++){
            lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
            if(!lv_obj_check_type(c, &lv_label_class) || lv_obj_get_user_data(c) == (void *)&g_bnum_tag) continue;
            if(lv_label_get_text(c)[0] && lv_obj_get_x(c) <= lv_obj_get_width(row) / 2) texts++;
        }
        if(texts >= 2) two_line = 1;
    }
    int want = rows >= 3 && !two_line, n = 0;
    for(uint32_t j = 0; j < cnt; j++){
        lv_obj_t *row = lv_obj_get_child(par, (int32_t)j);
        if(!blue_is_row(row)) continue;
        n++;
        lv_obj_t *num = blue_number_of(row);
        if(!want){
            if(num){ lv_obj_delete(num); blue_unshift(row); }
            continue;
        }
        if(!num){
            blue_shift_left_half(row);
            num = lv_label_create(row);
            lv_obj_set_user_data(num, (void *)&g_bnum_tag);        /* marks it as our number, not the row's own text */
            lv_obj_set_style_text_font(num, TF(UI_12), 0);
            lv_obj_set_style_text_color(num, TC(TEXT_MUTED), 0);
            lv_obj_align(num, LV_ALIGN_LEFT_MID, 8, 0);
            lv_obj_add_flag(num, LV_OBJ_FLAG_USER_4);             /* already styled: the pass leaves it as drawn here */
        }
        char b[8]; lv_snprintf(b, sizeof b, "%02d", streamed ? playlistview_row_pos(row, NULL, NULL) : n);
        if(strcmp(lv_label_get_text(num), b)) lv_label_set_text(num, b);
    }
}
static void blue_pass_end(void){
    for(int k = 0; k < g_bnum_n; k++) blue_renumber(g_bnum_pending[k]);   /* the pass that noted them has just ended */
    g_bnum_n = 0;
}
/* A drawing title: the wide tracking, tightened (then the next smaller face) only when the title would not fit the glass
 * at its row inside the rule's 10 px padding. Starts from the header face every time, so a shorter new title gets it
 * back. The box goes to its widest first: a long-dot label lays its text out in its CURRENT coords on each style
 * change, and new tracking in an old narrow box dotted "ADD TO PLAYLIST" down to "...". */
static void blue_title_fit(lv_obj_t *o, const char *text){
    /* the glass at the title's row once it has one: kit_edge_header sets the text before it places the label (y 0,
     * where the round screen is ~30 px wide) */
    int32_t y = lv_obj_get_y(o);
    int box = y >= 16 ? ui_round_width(y + 2, 10) : 250;
    if(box > 250) box = 250;
    const lv_font_t *f = TF(HEADER);
    int ls = 3;
    for(int k = 0; text && k < 12; k++){
        lv_point_t sz; lv_text_get_size(&sz, text, f, ls, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if(sz.x <= box - 20) break;
        if(ls > 0){ ls--; continue; }
        const lv_font_t *sm = theme_title_smaller(f);
        if(!sm) break;
        f = sm; ls = 3;
    }
    lv_obj_set_style_max_width(o, box, 0);
    lv_obj_set_width(o, box);
    lv_obj_update_layout(o);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_letter_space(o, ls, 0);
}
/* A top-layer title inherits the current screen's permanent figure number. */
static int blue_figure(lv_obj_t *o){
    for(lv_obj_t *p = o; p; p = lv_obj_get_parent(p))
        for(int id = SCR_HOME; id < SCR_COUNT; id++) if(p == screen_get_root(id)) return screen_figure_number(id);
    return screen_figure_number(screen_current());
}
static void blue_title_form(lv_obj_t *o, const char *text, char *out, size_t cap){
    /* EQ has slot arrows in the header band. Its figure is drawn beside the title at a smaller face. */
    if(blue_figure(o) == screen_figure_number(SCR_EQ)){ lv_snprintf(out, cap, "%s", text ? text : ""); return; }
    char full[192]; lv_snprintf(full, sizeof full, "fig.%d  %s", blue_figure(o), text ? text : "");
    /* First fit the complete title by stepping down. If it still exceeds the round glass,
     * omit the prefix so the original title keeps its full available width. */
    const lv_font_t *f = TF(HEADER);
    int fits = 0;
    int32_t y = lv_obj_get_y(o);
    int room = y >= 16 ? ui_round_width(y + 2, 10) : 250;
    if(room > 250) room = 250;
    room -= 20;  /* the rule's left and right padding */
    for(int k = 0; k < 12; k++){
        lv_point_t sz; lv_text_get_size(&sz, full, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if(sz.x <= room){ fits = 1; break; }
        const lv_font_t *next = theme_title_smaller(f);
        if(!next) break;
        f = next;
    }
    lv_snprintf(out, cap, "%s", fits ? full : text);
}
static void blue_title(lv_obj_t *o, const char *text){
    char shown[192]; blue_title_form(o, text, shown, sizeof shown);
    blue_title_fit(o, shown);
    lv_label_set_text(o, shown);
}
static void blue_mark(lv_layer_t *layer, int x1, int y1, int x2, int y2, int dashed){
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.p1 = (lv_point_precise_t){x1, y1}; d.p2 = (lv_point_precise_t){x2, y2};
    d.color = TC(ACCENT_PRIMARY); d.width = 1; d.opa = LV_OPA_COVER;
    if(dashed){ d.dash_width = 5; d.dash_gap = 4; }
    lv_draw_line(layer, &d);
}
static void blue_row_draw(lv_event_t *e){
    lv_area_t a; lv_obj_get_coords(lv_event_get_target(e), &a);
    for(int x = a.x1 + 3; x <= a.x2 - 3; x += a.x2 - a.x1 - 6)
        for(int y = a.y1 + 3; y <= a.y2 - 3; y += a.y2 - a.y1 - 6){
            blue_mark(lv_event_get_layer(e), x - 2, y, x + 2, y, 0);
            blue_mark(lv_event_get_layer(e), x, y - 2, x, y + 2, 0);
        }
}
static void blue_row_decor(lv_obj_t *row, lv_obj_t *title){
    if(!title) return;
    lv_obj_remove_event_cb(row, blue_row_draw);
    lv_obj_add_event_cb(row, blue_row_draw, LV_EVENT_DRAW_MAIN_END, NULL);
}
static lv_obj_t *g_blue_eq_fig;
static lv_font_t g_blue_eq_font;
static void blue_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_TITLE: {
        char txt[160]; lv_snprintf(txt, sizeof txt, "%s", lv_label_get_text(o));
        blue_title_fit(o, txt);                                   /* widest box first, then the padding */
        lv_obj_set_style_pad_hor(o, 10, 0);
        lv_label_set_text(o, txt);                                /* laid out again at the fitted face and box */
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_pad_bottom(o, 3, 0);
        lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);   /* the rule and padding add to the face, never cut it */
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);     /* the rule under a drawing title */
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_y(o, lv_obj_get_y(o) - 3);
        lv_obj_align(o, LV_ALIGN_TOP_MID, 0, lv_obj_get_y(o));
        if(lv_obj_get_parent(o) == screen_get_root(SCR_EQ)){
            if(!g_blue_eq_font.get_glyph_dsc){ g_blue_eq_font = *TF(UI_12); g_blue_eq_font.fallback = TF(UI_14); }
            lv_obj_set_style_text_font(o, &g_blue_eq_font, 0);
            lv_obj_set_style_text_letter_space(o, 1, 0);
            lv_obj_set_style_pad_bottom(o, 12, 0);  /* retain the common header box; ink ends above the arrows */
            lv_obj_set_style_border_width(o, 0, 0);
            lv_obj_set_size(o, LV_SIZE_CONTENT, 25);
            lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 37);
            if(!g_blue_eq_fig){
                g_blue_eq_fig = lv_label_create(lv_obj_get_parent(o));
                char number[16]; lv_snprintf(number, sizeof number, "fig.%d", blue_figure(o));
                lv_label_set_text(g_blue_eq_fig, number);
                lv_obj_set_style_text_font(g_blue_eq_fig, TF(UI_12), 0);
                lv_obj_set_style_text_color(g_blue_eq_fig, TC(DECOR_1), 0);
                kit_keep(g_blue_eq_fig);
            }
            lv_obj_align(g_blue_eq_fig, LV_ALIGN_TOP_MID, 0, 18);
        }
        break; }
    case KIT_ROW: blue_row(o); break;
    case KIT_ROW_CHEVRON:
        lv_label_set_text(o, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(o, TF(UI_12), 0);
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        break;
    case KIT_BUTTON: case KIT_ICON_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)) blue_outline(o, TC(ACCENT_PRIMARY));
        else if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && lv_color_eq(lv_obj_get_style_bg_color(o, 0), TC(ACTION_SURFACE))){
            /* a primary action is line work too, drawn in the accent (brand and danger buttons keep their fills) */
            blue_outline(o, TC(ACCENT_PRIMARY));
            for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++){
                lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
                if(lv_obj_check_type(c, &lv_label_class)) lv_obj_set_style_text_color(c, TC(ACCENT_PRIMARY), 0);
            }
        }
        else lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_bg_color(o, TC(SURFACE_RAISED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        break;
    case KIT_CARD: case KIT_POPUP:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){
            lv_obj_set_style_radius(o, 0, 0);
            lv_obj_set_style_bg_color(o, TC(SURFACE), 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_TOGGLE:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 0, LV_PART_KNOB);
        break;
    case KIT_SLIDER: case KIT_BAR:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        /* line work, not a solid block: the track is drawn as a 1 px outline and the level as a translucent wash of
         * the accent (a thick slider - Quick Settings' brightness - was a solid blue slab in a drawing of thin lines) */
        if(lv_obj_get_height(o) >= 20){
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), LV_PART_MAIN);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(o, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(o, LV_OPA_40, LV_PART_INDICATOR);
            for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++){   /* an icon on it (the sun): ink, not on-fill dark */
                lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
                if(lv_obj_check_type(c, &lv_label_class)){ lv_obj_set_style_text_color(c, TC(ACCENT_PRIMARY), 0); kit_keep(c); }
            }
        }
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_ITEMS);
        lv_obj_set_style_border_width(o, 1, LV_PART_ITEMS);
        lv_obj_set_style_border_color(o, TC(BORDER_STRONG), LV_PART_ITEMS);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_ITEMS);
        break;
    default: break;
    }
}
static lv_obj_t *blue_line(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t c){
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(l, x, y); lv_obj_set_size(l, w, h);
    lv_obj_set_style_bg_color(l, c, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    return l;
}
static lv_obj_t *g_blue_clock, *g_blue_mer;
static void blue_clock(const char *t){
    if(!g_blue_clock) return;
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    lv_label_set_text(g_blue_clock, hm);
    lv_label_set_text(g_blue_mer, mer);
    /* AM/PM just past the time's text (the block around it is wider) */
    lv_point_t tw; lv_text_get_size(&tw, hm, lv_obj_get_style_text_font(g_blue_clock, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_align_to(g_blue_mer, g_blue_clock, LV_ALIGN_OUT_RIGHT_BOTTOM, 2 - (192 - tw.x) / 2, -11);
}
static void blue_home(const home_parts_t *p){
    own_backgrounds();
    /* the figure: crosshairs on the sheet's own lines (the grid is 12 px, its centre rule at x 180), the time as fig.1 */
    lv_obj_t *h = blue_line(p->root, 0, 144, 360, 1, TC(BORDER_STRONG));
    lv_obj_t *v = blue_line(p->root, 180, 0, 1, 360, TC(BORDER_STRONG));
    lv_obj_move_to_index(h, 0); lv_obj_move_to_index(v, 0);
    lv_obj_t *fig = lv_label_create(p->root);
    lv_label_set_text(fig, "fig.1  TIME");
    lv_obj_set_style_text_font(fig, TF(UI_12), 0);
    lv_obj_set_style_text_color(fig, TC(DECOR_1), 0);
    lv_obj_set_pos(fig, 84, 68);                                     /* on the time block's left edge */
    lv_obj_set_pos(p->status, 0, 40);
    lv_obj_set_style_text_color(p->status, TC(TEXT_MUTED), 0);
    /* the time sits on the paper, over the lines, in a block of whole grid cells (16 x 4: x 84..276, y 84..132) */
    lv_obj_set_style_bg_color(p->clock, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(p->clock, LV_OPA_COVER, 0);
    lv_obj_set_size(p->clock, 192, 48);
    lv_obj_set_style_pad_all(p->clock, 0, 0);
    lv_obj_set_style_pad_top(p->clock, (48 - lv_font_get_line_height(lv_obj_get_style_text_font(p->clock, 0))) / 2, 0);
    lv_obj_set_style_text_align(p->clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(p->clock, 84, 84);
    g_blue_clock = p->clock;
    g_blue_mer = mer_label(p->root, TF(UI_12), TC(DECOR_1));
    blue_clock(lv_label_get_text(p->clock));
    lv_obj_set_pos(p->date, 0, 160);
    lv_obj_set_style_text_letter_space(p->date, 2, 0);
    lv_obj_set_style_text_color(p->date, TC(ACCENT_PRIMARY), 0);
    /* Library and Search as outlined callouts. Every edge on a grid line:
     * Library 72..228, Search 240..288 (the row centred), y 216..256; now playing 60..300, y 264..312 */
    lv_obj_set_pos(p->library, 72, 216); lv_obj_set_size(p->library, 156, 40);
    blue_outline(p->library, TC(ACCENT_PRIMARY));
    home_library_fit(p->library);
    lv_obj_set_pos(p->search, 240, 216); lv_obj_set_size(p->search, 48, 40);
    blue_outline(p->search, TC(ACCENT_PRIMARY));
    lv_obj_set_pos(p->np, 60, 264); lv_obj_set_size(p->np, 240, 48);
    blue_outline(p->np, TC(ACCENT_PRIMARY));
    lv_obj_set_style_bg_color(p->np, TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(p->np, LV_OPA_COVER, 0);
    home_set_art_enabled(0);
    lv_obj_set_pos(p->np_thumb, 8, 12); lv_obj_set_size(p->np_thumb, 24, 24);
    lv_obj_set_style_radius(p->np_thumb, 0, 0);
    lv_obj_set_style_bg_opa(p->np_thumb, LV_OPA_TRANSP, 0);
    lv_label_set_text(p->np_glyph, ">");
    lv_obj_set_style_text_font(p->np_glyph, TF(UI_16), 0);
    lv_obj_set_style_text_color(p->np_glyph, TC(DECOR_1), 0);
    lv_obj_center(p->np_glyph);
    lv_obj_set_pos(p->np_title, 36, 6);  lv_obj_set_width(p->np_title, 150);
    lv_obj_set_pos(p->np_artist, 36, 26); lv_obj_set_width(p->np_artist, 150);
    lv_obj_set_pos(p->pp, 190, 3); lv_obj_set_size(p->pp, 42, 42);   /* 8 px from the end, as the marker is from the start */
    lv_obj_set_style_text_color(p->pp_label, TC(ACCENT_PRIMARY), 0);
}
static void blue_np(const np_parts_t *p){
    own_backgrounds();
    lv_obj_set_style_arc_width(p->ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->ring, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->ring, TC(BORDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_pad_all(p->ring, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(p->cover, 0, 0);
    lv_obj_set_style_border_width(p->cover, 1, 0);
    lv_obj_set_style_border_color(p->cover, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_border_opa(p->cover, LV_OPA_COVER, 0);
    /* crop marks just outside the cover's corners */
    lv_obj_update_layout(p->root);
    lv_area_t a; lv_obj_get_coords(p->cover, &a);
    int cx[2] = { a.x1 - 10, a.x2 + 10 }, cy[2] = { a.y1 - 10, a.y2 + 10 };
    for(int i = 0; i < 4; i++){
        int x = cx[i & 1], y = cy[i >> 1];
        blue_line(p->root, x - 7, y, 15, 1, TC(DECOR_1));
        blue_line(p->root, x, y - 7, 1, 15, TC(DECOR_1));
    }
    lv_obj_set_style_text_letter_space(p->title, 1, 0);
    lv_obj_set_size(p->pp, 48, 48);
    lv_obj_set_style_radius(p->pp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p->pp, TC(CANVAS), 0);                 /* the grid stops at the circle: a clear key */
    lv_obj_set_style_bg_opa(p->pp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->pp, 2, 0);
    lv_obj_set_style_border_color(p->pp, TC(DECOR_1), 0);
    lv_obj_set_style_border_opa(p->pp, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(p->pp, TC(DECOR_1), 0);
    lv_obj_set_style_text_align(p->pp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(p->pp, TF(UI_28), 0);                 /* a size down: the glyph clears the 2 px ring */
    lv_obj_set_style_pad_top(p->pp, 8, 0);                           /* ui_pp_glyph re-centres it by its ink */
    lv_obj_align(p->pp, LV_ALIGN_TOP_MID, 0, 264);
    lv_obj_set_ext_click_area(p->pp, 4);
    lv_obj_align(p->prev, LV_ALIGN_TOP_MID, -62, 274);
    lv_obj_align(p->next, LV_ALIGN_TOP_MID, 62, 274);
    lv_obj_set_style_text_color(p->prev, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_text_color(p->next, TC(ACCENT_PRIMARY), 0);
    np_text_rows(p, 191, 219, 241);                          /* the album line clears the ring at 264, no two rows touch */
    np_sides_on_cover(p, 0);                                 /* times stay at 318: the play ring ends at 312 */
    /* Dimension rules sit under the time labels, split around the page dots. The existing
     * circular seek remains the full input surface and its progress stays visible. */
    for(int i = 0; i < 2; i++){
        int x = i ? 198 : 112;
        blue_line(p->root, x, 340, 50, 1, TC(ACCENT_PRIMARY));
        blue_line(p->root, x, 337, 1, 7, TC(ACCENT_PRIMARY));
        blue_line(p->root, x + 49, 337, 1, 7, TC(ACCENT_PRIMARY));
    }
}
static void blue_cover_draw(lv_event_t *e){
    lv_obj_t *cover = lv_event_get_target(e);
    if(!lv_obj_has_flag(cover, LV_OBJ_FLAG_USER_3)) return;
    lv_area_t a; lv_obj_get_coords(cover, &a);
    int x1 = a.x1 + 4, y1 = a.y1 + 4, x2 = a.x2 - 4, y2 = a.y2 - 4;
    lv_layer_t *layer = lv_event_get_layer(e);
    blue_mark(layer, x1, y1, x2, y1, 1); blue_mark(layer, x2, y1, x2, y2, 1);
    blue_mark(layer, x2, y2, x1, y2, 1); blue_mark(layer, x1, y2, x1, y1, 1);
    blue_mark(layer, x1 + 10, y1 + 10, x2 - 10, y2 - 10, 0);
    blue_mark(layer, x2 - 10, y1 + 10, x1 + 10, y2 - 10, 0);
}
static void blue_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    lv_obj_remove_event_cb(cover, blue_cover_draw);
    lv_obj_add_event_cb(cover, blue_cover_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    if(show && !loading){ lv_obj_add_flag(cover, LV_OBJ_FLAG_USER_3); lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_remove_flag(cover, LV_OBJ_FLAG_USER_3); if(show) lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN); }
    lv_obj_invalidate(cover);
}
static void blue_empty_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_user_data(e);
    const char *saved = lv_obj_get_user_data(o);
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if(!saved || strcmp(saved, lv_label_get_text(o))) return; /* Lyrics reuses this label for real lyrics */
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_point_t sz; lv_text_get_size(&sz, saved, lv_obj_get_style_text_font(o, 0), 0, 2, 256, LV_TEXT_FLAG_NONE);
    int fw = sz.x + 24; if(fw < 100) fw = 100; if(fw > 280) fw = 280;
    int x = (a.x1 + a.x2 - fw) / 2;
    a.x1 = x; a.x2 = x + fw;
    if(!lv_obj_has_flag(o, LV_OBJ_FLAG_USER_2)){ a.y1 -= 12; a.y2 += 12; }
    lv_layer_t *layer = lv_event_get_layer(e);
    blue_mark(layer, a.x1, a.y1, a.x2, a.y1, 1);
    blue_mark(layer, a.x2, a.y1, a.x2, a.y2, 1);
    blue_mark(layer, a.x2, a.y2, a.x1, a.y2, 1);
    blue_mark(layer, a.x1, a.y2, a.x1, a.y1, 1);
}
static void blue_empty_deleted(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    lv_obj_remove_event_cb_with_user_data(lv_obj_get_parent(o), blue_empty_draw, o);
    free(lv_obj_get_user_data(o));
}
static void blue_empty(kit_empty_kind_t kind, lv_obj_t *message){
    if(!message) return;
    const char *s = lv_label_get_text(message);
    lv_obj_set_style_text_font(message, TF(UI_14), 0);
    lv_obj_set_style_text_color(message, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    if(kind == KIT_EMPTY_LYRICS){
        lv_obj_remove_flag(message, LV_OBJ_FLAG_USER_2);
        lv_obj_set_style_pad_ver(message, 0, 0);
        lv_obj_update_layout(message);
        lv_area_t a; lv_obj_get_coords(message, &a);
        lv_obj_set_style_translate_y(message, 180 - (a.y1 + a.y2) / 2, 0);
    } else {
        lv_obj_add_flag(message, LV_OBJ_FLAG_USER_2);
        lv_obj_set_style_pad_ver(message, 12, 0);
        if(kind == KIT_EMPTY_UPNEXT){
            lv_obj_t *parent = lv_obj_get_parent(message);
            lv_obj_update_layout(parent);
            lv_area_t label, area;
            lv_obj_get_coords(message, &label);
            lv_obj_get_coords(parent, &area);
            lv_obj_set_style_translate_y(message,
                (area.y1 + area.y2 - label.y1 - label.y2) / 2, 0);
        }
    }
    free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(s));
    lv_obj_t *parent = lv_obj_get_parent(message);
    lv_obj_remove_event_cb_with_user_data(parent, blue_empty_draw, message);
    lv_obj_add_event_cb(parent, blue_empty_draw, LV_EVENT_DRAW_MAIN_END, message);
    lv_obj_invalidate(parent);
    lv_obj_remove_event_cb(message, blue_empty_deleted);
    lv_obj_add_event_cb(message, blue_empty_deleted, LV_EVENT_DELETE, NULL);
}
static void blue_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    (void)toggle;
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, on ? TC(DECOR_1) : TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(c, on ? 1 : 0, 0);            /* a lit tile is a double circle */
    lv_obj_set_style_outline_pad(c, 3, 0);
    lv_obj_set_style_outline_color(c, TC(DECOR_1), 0);
    lv_obj_set_style_outline_opa(c, LV_OPA_COVER, 0);
    if(g) lv_obj_set_style_text_color(g, on ? TC(DECOR_1) : TC(ACCENT_PRIMARY), 0);
    if(cap){
        char up[32]; lv_label_set_text(cap, theme_upper(up, sizeof up, lv_label_get_text(cap)));
        lv_obj_set_style_text_font(cap, TF(UI_12), 0);
        lv_obj_set_style_text_color(cap, on ? TC(DECOR_1) : TC(TEXT_SECONDARY), 0);
    }
}
const theme_kit_t theme_kit_blue = { .id = "blueprint", .header = kit_edge_header, .style = blue_style, .home = blue_home,
                                     .title_fit = blue_title_fit,
                                     .home_clock = blue_clock,
                                     .nowplaying = blue_np, .np_cover_state = blue_cover, .empty_state = blue_empty,
                                     .row_decor = blue_row_decor, .title = blue_title,
                                     .qs_tile = blue_qs_tile,
                                     .pass_end = blue_pass_end };

/* ================================================================ stone
 * Soft and tactile: everything is a rounded pill sitting on the surface, with a solid 3 px edge underneath in the
 * shadow colour (DOT_GRID). The edge is below the pill, not inside it: the pill keeps its whole shape and its content
 * stays centred. It is drawn as the pill's own shape, filled and dropped 3 px, just before the pill (one more rounded
 * fill: LVGL's shadow path would rebuild a corner buffer on every draw here, with no shadow cache in this build).
 * Chunky rounded controls; the Home time as two overlapping pills, hours and minutes. */
/* Three overlapping rounded fills give each end a different shoulder. All drawing stays inside the
 * original rectangular object, so its input box and the position of its children are unchanged. */
static void stone_blob(lv_layer_t *layer, const lv_area_t *box, lv_color_t color, lv_opa_t opa){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = color; d.bg_opa = opa; d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = *box;
    a.x1 += 5; a.x2 -= 5;
    lv_draw_rect(layer, &d, &a);
    a = *box; a.x2 -= 11; a.y1 += 3; a.y2 -= 8;
    lv_draw_rect(layer, &d, &a);
    a = *box; a.x1 += 11; a.y1 += 8; a.y2 -= 2;
    lv_draw_rect(layer, &d, &a);
}
static void stone_edge_cb(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    int drop = lv_obj_has_state(o, LV_STATE_PRESSED) ? 1 : (int)(intptr_t)lv_event_get_user_data(e);   /* pressed: sinks */
    if(lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE){ lv_event_set_ext_draw_size(e, 4); return; }
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_area_move(&a, 0, drop);
    stone_blob(lv_event_get_layer(e), &a, TC(DOT_GRID), lv_obj_get_style_opa_recursive(o, 0));
    lv_obj_get_coords(o, &a);
    stone_blob(lv_event_get_layer(e), &a, lv_obj_get_style_bg_color(o, LV_PART_MAIN),
               lv_obj_get_style_opa_recursive(o, 0));
}
static void stone_edge(lv_obj_t *o, int drop){
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_remove_event_cb_with_user_data(o, stone_edge_cb, (void *)(intptr_t)3);   /* restyled: one edge, not two */
    lv_obj_remove_event_cb_with_user_data(o, stone_edge_cb, (void *)(intptr_t)4);
    lv_obj_add_event_cb(o, stone_edge_cb, LV_EVENT_DRAW_MAIN_BEGIN, (void *)(intptr_t)drop);
    lv_obj_add_event_cb(o, stone_edge_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, (void *)(intptr_t)drop);
    lv_obj_refresh_ext_draw_size(o);
}
static void stone_pill(lv_obj_t *o, lv_color_t bg){
    kit_keep(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_STATE_PRESSED);
    stone_edge(o, 3);
    lv_obj_set_style_translate_y(o, 2, LV_STATE_PRESSED);            /* pressed: it sinks into the surface */
}
static void stone_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_ROW:
        stone_pill(o, TC(SURFACE));
        lv_obj_set_style_bg_color(o, TC(SURFACE_RAISED), LV_STATE_PRESSED);
        break;
    case KIT_ROW_CHEVRON:
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        break;
    case KIT_BUTTON: case KIT_ICON_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) stone_pill(o, lv_obj_get_style_bg_color(o, 0));
        else lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        break;
    case KIT_CARD:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) lv_obj_set_style_radius(o, 22, 0);
        break;
    case KIT_POPUP:
        lv_obj_set_style_radius(o, 26, 0);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) stone_edge(o, 4);
        break;
    case KIT_SLIDER: case KIT_BAR:
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        lv_obj_set_style_radius(o, 18, 0);
        lv_obj_set_style_radius(o, 14, LV_PART_SELECTED);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 12, LV_PART_ITEMS);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
        lv_obj_set_style_border_width(o, 2, LV_PART_ITEMS);
        lv_obj_set_style_border_color(o, TC(DOT_GRID), LV_PART_ITEMS);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    default: break;
    }
}
static void stone_empty_draw(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE){ lv_event_set_ext_draw_size(e, 26); return; }
    lv_obj_t *message = lv_event_get_target(e);
    const char *saved = lv_obj_get_user_data(message);
    if(!saved || strcmp(saved, lv_label_get_text(message))) return; /* Lyrics can reuse this label. */
    lv_point_t size;
    lv_text_get_size(&size, saved, lv_obj_get_style_text_font(message, 0), 0, 2,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = size.x + 32; if(w < 112) w = 112; if(w > 282) w = 282;
    int h = lv_obj_get_height(message) + 28; if(h < 46) h = 46;
    lv_area_t label; lv_obj_get_coords(message, &label);
    int cx = (label.x1 + label.x2) / 2, cy = (label.y1 + label.y2) / 2;
    lv_area_t pebble = { cx - w/2, cy - h/2, cx + (w-1)/2, cy + (h-1)/2 };
    stone_blob(lv_event_get_layer(e), &pebble, TC(SURFACE_RAISED), LV_OPA_COVER);
}
static void stone_empty_deleted(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static int stone_empty_center(lv_obj_t *message){
    lv_obj_t *parent = lv_obj_get_parent(message);
    lv_obj_update_layout(parent);
    lv_area_t a; lv_obj_get_coords(parent, &a);
    return a.y1 + lv_obj_get_style_pad_top(parent, 0) + lv_obj_get_content_height(parent)/2;
}
static void stone_empty_at(lv_obj_t *message, int center){
    lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_y(message, center - (a.y1 + a.y2)/2, 0);
}
static void stone_empty(kit_empty_kind_t kind, lv_obj_t *message){
    if(!message) return;
    lv_obj_set_style_text_color(message, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(message, TF(UI_14), 0);
    lv_obj_update_layout(message);
    if(kind == KIT_EMPTY_LYRICS) lv_obj_align(message, LV_ALIGN_CENTER, 0, 0);
    else stone_empty_at(message, stone_empty_center(message));
    lv_obj_remove_event_cb(message, stone_empty_draw);
    lv_obj_remove_event_cb(message, stone_empty_deleted);
    free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(lv_label_get_text(message)));
    lv_obj_add_event_cb(message, stone_empty_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(message, stone_empty_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(message);
    lv_obj_add_event_cb(message, stone_empty_deleted, LV_EVENT_DELETE, NULL);
}
static void zine_scrap(lv_obj_t *o, lv_color_t bg);
void kit_stone_empty_scan(lv_obj_t *message, lv_obj_t *button){
    int zine = !strcmp(theme_kit()->id, "zine");
    if((strcmp(theme_kit()->id, "stone") && !zine) || !message || !button) return;
    if(zine){   /* a yellow scrap like Home's search button: it reads as something to press, not a second message */
        zine_scrap(button, TC(DECOR_1));
        lv_obj_set_style_bg_color(button, TC(DECOR_1), LV_STATE_PRESSED);
    }
    int center = stone_empty_center(message);
    int pebble_h = lv_obj_get_height(message) + (zine ? 0 : 28);
    if(pebble_h < 46) pebble_h = 46;
    int gap = 20, button_h = lv_obj_get_height(button);
    int top = center - (pebble_h + gap + button_h)/2;
    stone_empty_at(message, top + pebble_h/2);
    stone_empty_at(button, top + pebble_h + gap + button_h/2);
    /* the scrap's press nudge is +2 px from where it now sits (a bare pressed translate_y would replace the
     * placement and drop the button back under the message) */
    if(zine) lv_obj_set_style_translate_y(button, lv_obj_get_style_translate_y(button, 0) + 2, LV_STATE_PRESSED);
}
static void stone_cover_draw(lv_event_t *e){
    lv_obj_t *cover = lv_event_get_target(e);
    lv_obj_t *note = lv_event_get_user_data(e);
    if(!note || lv_obj_has_flag(note, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(cover, &a);
    stone_blob(lv_event_get_layer(e), &a, TC(ART_PLACEHOLDER), LV_OPA_COVER);
}
static void stone_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    lv_obj_set_style_bg_opa(cover, LV_OPA_TRANSP, 0);
    lv_label_set_text(note, loading ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, TF(UI_20), 0); /* this face has LVGL's audio symbol */
    lv_obj_set_style_text_color(note, TC(TEXT_PRIMARY), 0);
    if(show) lv_obj_center(note);
}
static lv_obj_t *g_stone_arc[5];
static int g_stone_arc_bucket = -1;
static void stone_progress(int value){
    if(!g_stone_arc[0]) return;
    int bucket = (value * 143 + 500) / 1000; /* two-degree steps over the 286-degree sweep */
    if(bucket == g_stone_arc_bucket) return;
    g_stone_arc_bucket = bucket;
    int end = bucket * 2;
    for(int i = 0; i < 5; i++){
        int from = i == 0 ? 0 : end - (5-i)*5;
        int to = i == 0 ? end - 20 : from + 5;
        if(from < 0) from = 0;
        if(to > end) to = end;
        if(to <= from){ lv_obj_add_flag(g_stone_arc[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_arc_set_angles(g_stone_arc[i], from, to);
        lv_obj_remove_flag(g_stone_arc[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static void stone_progress_create(lv_obj_t *ring){
    for(int i = 0; i < 5; i++){
        lv_obj_t *arc = lv_arc_create(lv_obj_get_parent(ring));
        lv_obj_set_size(arc, lv_obj_get_width(ring), lv_obj_get_height(ring));
        lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_rotation(arc, 127);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, i == 0 ? LV_OPA_COVER : (lv_opa_t)(190 - 43*(i-1)), LV_PART_INDICATOR);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
        g_stone_arc[i] = arc;
    }
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    g_stone_arc_bucket = -1;
}
/* a clock drawn as separate hours and minutes (12-hour: AM/PM is dropped here; the date line keeps the day) */
static void split_clock(const char *t, lv_obj_t *hl, lv_obj_t *ml){
    if(!hl || !ml) return;
    char hh[8] = "--", mm[16] = "--";
    const char *c = t ? strchr(t, ':') : NULL;
    if(c){ int n = (int)(c - t); if(n > 0 && n < 4){ memcpy(hh, t, n); hh[n] = 0; } lv_snprintf(mm, sizeof mm, "%s", c + 1); }
    char *sp = strchr(mm, ' '); if(sp) *sp = 0;
    lv_label_set_text(hl, hh);
    lv_label_set_text(ml, mm);
}
static lv_obj_t *g_stone_hh, *g_stone_mm, *g_stone_mer;
static void stone_clock(const char *t){
    split_clock(t, g_stone_hh, g_stone_mm);
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    char low[8]; if(g_stone_mer) lv_label_set_text(g_stone_mer, theme_lower(low, sizeof low, mer));
}
static lv_obj_t *stone_clock_pill(lv_obj_t *root, lv_color_t bg, lv_color_t fg, int x, int y){
    lv_obj_t *p = lv_obj_create(root);
    lv_obj_remove_style_all(p);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(p, 130, 76);
    lv_obj_set_pos(p, x, y);
    stone_pill(p, bg);
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, TF(CLOCK), 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return l;
}
static void stone_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_set_pos(p->status, 0, 30);
    lv_obj_add_flag(p->clock, LV_OBJ_FLAG_HIDDEN);                /* the time is the two pills */
    /* the two pills together are centred (62..298); their vertical step is the design */
    g_stone_hh = stone_clock_pill(p->root, TC(SURFACE_RAISED), TC(TEXT_PRIMARY), 62, 56);
    g_stone_mm = stone_clock_pill(p->root, TC(ACCENT_PRIMARY), TC(ON_ACCENT), 168, 84);
    g_stone_mer = mer_label(p->root, TF(UI_16), TC(TEXT_SECONDARY));   /* "pm" just past the minutes pill */
    lv_obj_align_to(g_stone_mer, lv_obj_get_parent(g_stone_mm), LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);
    stone_clock(lv_label_get_text(p->clock));
    lv_obj_set_pos(p->date, 0, 168);
    lv_obj_set_style_text_color(p->date, TC(TEXT_SECONDARY), 0);
    /* Library + 8 + Search share the now-playing pill's edges (62..298) */
    lv_obj_set_pos(p->library, 62, 222); lv_obj_set_size(p->library, 186, 42);
    stone_pill(p->library, TC(SURFACE));
    home_library_fit(p->library);
    lv_obj_set_pos(p->search, 256, 222); lv_obj_set_size(p->search, 42, 42);
    stone_pill(p->search, TC(SURFACE));
    lv_obj_set_pos(p->np, 62, 274); lv_obj_set_size(p->np, 236, 48);
    stone_pill(p->np, TC(SURFACE));
    lv_obj_set_pos(p->np_thumb, 8, 6); lv_obj_set_size(p->np_thumb, 38, 38);
    lv_obj_set_pos(p->np_title, 54, 6);  lv_obj_set_width(p->np_title, 130);
    lv_obj_set_pos(p->np_artist, 54, 26); lv_obj_set_width(p->np_artist, 130);
    lv_obj_set_pos(p->pp, 184, 1); lv_obj_set_size(p->pp, 44, 44);   /* 8 px from the pill's end, as the art is from its start */
    lv_obj_set_style_bg_color(p->pp, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(p->pp, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(p->pp_label, TC(ON_ACCENT), 0);
}
static void stone_np(const np_parts_t *p){
    own_backgrounds();
    lv_obj_set_style_arc_width(p->ring, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(p->ring, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(p->ring, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(p->ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(p->ring, TC(SURFACE_RAISED), LV_PART_MAIN);
    stone_progress_create(p->ring);
    lv_obj_set_style_bg_opa(p->cover, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(p->cover, stone_cover_draw, LV_EVENT_DRAW_MAIN_BEGIN, p->cover_note);
    lv_obj_set_style_pad_all(p->ring, 2, LV_PART_KNOB);
    lv_obj_set_style_radius(p->cover, 26, 0);
    lv_obj_set_size(p->pp, 64, 44);
    stone_pill(p->pp, TC(ACCENT_PRIMARY));
    ui_np_pp_filled(1);                                      /* accent changes recolour the fill, not the glyph */
    lv_obj_set_style_text_color(p->pp, TC(ON_ACCENT), 0);
    lv_obj_set_style_text_align(p->pp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p->pp, 5, 0);
    lv_obj_set_style_text_font(p->pp, TF(UI_28), 0);
    lv_obj_align(p->pp, LV_ALIGN_TOP_MID, 0, 266);
    lv_obj_set_ext_click_area(p->pp, 4);
    lv_obj_align(p->prev, LV_ALIGN_TOP_MID, -70, 274);
    lv_obj_align(p->next, LV_ALIGN_TOP_MID, 70, 274);
    np_sides_on_cover(p, 0);                                 /* times stay at 318: the play pill ends at 310 */
}
static void stone_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    (void)toggle;
    lv_obj_set_size(c, 70, 50);
    stone_pill(c, on ? TC(ACCENT_PRIMARY) : TC(SURFACE));
    if(g) lv_obj_set_style_text_color(g, on ? TC(ON_ACCENT) : TC(TEXT_PRIMARY), 0);
    if(cap) lv_obj_set_style_text_color(cap, TC(TEXT_SECONDARY), 0);
}
const theme_kit_t theme_kit_stone = { .id = "stone", .style = stone_style, .home = stone_home, .home_clock = stone_clock,
                                      .nowplaying = stone_np, .qs_tile = stone_qs_tile, .np_progress = stone_progress,
                                      .empty_state = stone_empty, .np_cover_state = stone_cover };

/* ================================================================ bauhaus
 * Poster geometry: square blocks, bold upper-case type, the three primaries. Titles are solid caps of yellow, blue or
 * red across the top of the round screen; Home is two giant numerals with its controls as shapes (a yellow circle is
 * Library, a blue square is Search); Now Playing is split in half by a red rule. DOT_GRID is the ink colour. */
static lv_color_t bauh_cap_color(const char *title, lv_color_t *ink){
    unsigned h = 0; for(const char *c = title; c && *c; c++) h = h * 31 + (unsigned char)*c;
    switch(h % 3){
    case 0:  *ink = TC(DOT_GRID);  return TC(DECOR_2);         /* yellow, ink type */
    case 1:  *ink = TC(ON_ACCENT); return TC(DECOR_1);         /* blue, white type */
    default: *ink = TC(ON_ACCENT); return TC(ACCENT_PRIMARY);  /* red, white type */
    }
}
static void bauh_title(lv_obj_t *label, const char *text){
    lv_label_set_text(label, text);
    lv_color_t ink, cap = bauh_cap_color(text, &ink);
    lv_obj_set_style_bg_color(label, cap, 0);
    lv_obj_set_style_text_color(label, ink, 0);
}
static lv_obj_t *bauh_header(lv_obj_t *root, const char *title, lv_event_cb_t back_cb){
    lv_obj_t *t = kit_edge_header(root, title, back_cb);
    /* the cap: a full-width block the round screen cuts into an arc; the title sits in its lower part. 60 px: the
     * screens' own content starts at y 64 (their first rows, a toggle card), so the cap ends clear of it */
    enum { CAP_H = 60 };
    lv_obj_set_pos(t, 0, 0); lv_obj_set_size(t, 360, CAP_H);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(t, CAP_H - lv_font_get_line_height(TF(HEADER)) - 2, 0);
    int ty = CAP_H - lv_font_get_line_height(TF(HEADER)) - 2;  /* the title's top row inside the cap */
    lv_obj_set_style_pad_hor(t, (360 - ui_round_width(ty + 4, 12)) / 2, 0);   /* the text stays on the glass */
    lv_obj_set_style_text_font(t, TF(HEADER), 0);         /* fitted again below, to the cap's own width */
    theme_title_text(t, title);                           /* set last, from the original title: the label lays it out
                                                           * at its final size (a long-dot label edits its own copy) */
    return t;
}
static void bauh_block(lv_obj_t *o, lv_color_t bg){
    kit_keep(o);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
}
/* Reserve a fixed bullet slot. Only the leading content moves; right values and controls stay put. */
#define BAUH_ROW_MARK LV_OBJ_FLAG_USER_1
static void bauh_triangle(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e); lv_area_t a; lv_obj_get_coords(o, &a);
    lv_draw_triangle_dsc_t d; lv_draw_triangle_dsc_init(&d);
    d.bg_color = lv_obj_get_style_bg_color(o, 0); d.bg_opa = LV_OPA_COVER;
    d.p[0] = (lv_point_precise_t){a.x2, a.y2};
    d.p[1] = (lv_point_precise_t){a.x1, a.y2};
    d.p[2] = (lv_point_precise_t){(a.x1 + a.x2)/2, a.y1};
    lv_draw_triangle(lv_event_get_layer(e), &d);
}
static lv_obj_t *bauh_shape(lv_obj_t *parent, int x, int y, int w, int h, int kind, lv_color_t color){
    lv_obj_t *o = lv_obj_create(parent); lv_obj_remove_style_all(o); kit_keep(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, color, 0);
    if(kind == 2) lv_obj_add_event_cb(o, bauh_triangle, LV_EVENT_DRAW_MAIN_END, NULL);
    else { lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
           lv_obj_set_style_radius(o, kind == 0 ? LV_RADIUS_CIRCLE : 0, 0); }
    return o;
}
static void bauh_row_press(lv_event_t *e){
    lv_obj_t *row = lv_event_get_target(e);
    lv_obj_t *mark = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
    int kind = lv_obj_get_index(row) % 3;
    lv_color_t color = kind == 0 ? TC(ACCENT_PRIMARY) : kind == 1 ? TC(DECOR_2) : TC(DECOR_1);
    lv_obj_set_style_bg_color(mark, lv_event_get_code(e) == LV_EVENT_PRESSED ? TC(DOT_GRID) : color, 0);
    lv_obj_invalidate(mark);
}
static void bauh_row_decor(lv_obj_t *row, lv_obj_t *title){
    if(!title || lv_obj_has_flag(row, BAUH_ROW_MARK)) return;
    kit_row_layout_t layout = kit_row_layout_get(row);
    const char *s = lv_label_get_text(title);
    if(!s || !*s) return;
    /* kit_pass laid out the screen once before styling. Use those coordinates:
     * flushing here repeats the entire growing list's flex layout for each row. */
    lv_area_t r, t; lv_obj_get_coords(row, &r); lv_obj_get_coords(title, &t);
    int inset = t.x1 - r.x1;
    if(lv_obj_get_width(row) < 260) return;
    if(layout == KIT_ROW_PLAIN && (inset < 11 || inset > 24 || (unsigned char)s[0] == 0xEF)) return;
    uint32_t count = lv_obj_get_child_count(row);
    for(uint32_t i = 0; i < count; i++){
        lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
        lv_area_t a; lv_obj_get_coords(child, &a);
        int x = a.x1 - r.x1;
        if(layout == KIT_ROW_ICON){
            if(i == 0) lv_obj_set_x(child, 24);
            else if(i == 1){ lv_obj_set_x(child, 50); lv_obj_set_width(child, lv_obj_get_width(child) - 10); }
        } else if(layout == KIT_ROW_SWATCH){
            if(i < 3) lv_obj_set_x(child, x + 6);
            else if(i == 3){
                lv_obj_set_x(child, x + 6);
                lv_obj_set_width(child, 148);
                lv_label_set_long_mode(child, LV_LABEL_LONG_DOT);
            }
        } else if(lv_obj_check_type(child, &lv_label_class) && x >= 11 && x <= 24){
            int shift = 24 - x;
            if(shift > 0){
                lv_obj_set_x(child, 24);
                if(lv_label_get_long_mode(child) != LV_LABEL_LONG_WRAP)
                    lv_obj_set_width(child, lv_obj_get_width(child) - shift);
            }
        }
    }
    int kind = lv_obj_get_index(row) % 3;
    lv_color_t c = kind == 0 ? TC(ACCENT_PRIMARY) : kind == 1 ? TC(DECOR_2) : TC(DECOR_1);
    bauh_shape(row, 8, (lv_obj_get_height(row) - 8)/2, 8, 8, kind, c);
    lv_obj_add_event_cb(row, bauh_row_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(row, bauh_row_press, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(row, bauh_row_press, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_flag(row, BAUH_ROW_MARK);
}
static void bauh_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_ROW:
        bauh_block(o, TC(SURFACE));
        lv_obj_set_style_bg_color(o, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        break;
    /* row words keep their own case (titles are the capitals): upper-casing only the rows set in UI faces made
     * Settings shout while Library and the menus did not */
    case KIT_ROW_CHEVRON:
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        break;
    case KIT_BUTTON: case KIT_ICON_BUTTON:
        lv_obj_set_style_radius(o, r == KIT_ICON_BUTTON && lv_obj_get_width(o) < 70 ? LV_RADIUS_CIRCLE : 0, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        break;
    case KIT_CARD:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) lv_obj_set_style_radius(o, 0, 0);
        break;
    case KIT_POPUP:
        lv_obj_set_style_radius(o, 0, 0);
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){
            lv_obj_set_style_border_width(o, 3, 0);
            lv_obj_set_style_border_color(o, TC(TEXT_PRIMARY), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_TOGGLE:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 0, LV_PART_KNOB);
        break;
    case KIT_SLIDER: case KIT_BAR:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_bg_color(o, TC(DECOR_2), LV_PART_KNOB);
        break;
    case KIT_ROLLER: case KIT_TEXTAREA:
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_radius(o, 0, LV_PART_SELECTED);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 0, LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    default: break;
    }
}
static lv_obj_t *g_bauh_hh, *g_bauh_mm, *g_bauh_mer;
static lv_obj_t *g_bauh_date;   /* the right column under the square */
static void bauh_clock(const char *t){
    split_clock(t, g_bauh_hh, g_bauh_mm);
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    if(g_bauh_mer) lv_label_set_text(g_bauh_mer, mer);
    /* 12-hour: PM takes the top of the column (right under the square, which ends at 195), and the date steps
     * down to clear it (they collided by 7 px) */
    if(g_bauh_mer) lv_obj_set_y(g_bauh_mer, mer[0] ? 196 : 200);
    if(g_bauh_date) lv_obj_set_y(g_bauh_date, mer[0] ? 222 : 218);
}
static lv_obj_t *bauh_numeral(lv_obj_t *root, lv_color_t c, int x, int y){
    lv_obj_t *l = lv_label_create(root);
    lv_obj_set_style_text_font(l, TF(CLOCK), 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_letter_space(l, -6, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}
static void bauh_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_set_pos(p->status, 0, 14);
    lv_obj_add_flag(p->clock, LV_OBJ_FLAG_HIDDEN);
    /* place the numerals by their ink, not their line box: the digit tops sit at y 40 and y 146 */
    const lv_font_t *f = TF(CLOCK);
    lv_font_glyph_dsc_t g; int top = 0;
    if(lv_font_get_glyph_dsc(f, &g, '3', 0))
        top = (lv_font_get_line_height(f) - f->base_line) - (g.ofs_y + g.box_h);   /* ink top inside the label box */
    g_bauh_hh = bauh_numeral(p->root, TC(TEXT_PRIMARY), 62, 44 - top);          /* inside the glass at its corner */
    /* each numeral sits against the round edge at its own height: the circle is wider where the minutes are */
    g_bauh_mm = bauh_numeral(p->root, TC(ACCENT_PRIMARY), 34, 146 - top);
    g_bauh_mer = mer_label(p->root, TF(DATE), TC(ACCENT_PRIMARY));             /* PM, beside the red minutes */
    lv_obj_set_width(g_bauh_mer, 100); lv_obj_set_style_text_align(g_bauh_mer, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_bauh_mer, 197, 200);                                       /* the right column: x 197..297, under the square */
    bauh_clock(lv_label_get_text(p->clock));
    /* Library: the yellow circle; Search: the blue square */
    lv_obj_set_pos(p->library, 226, 44); lv_obj_set_size(p->library, 84, 84);
    bauh_block(p->library, TC(DECOR_2));
    lv_obj_set_style_radius(p->library, LV_RADIUS_CIRCLE, 0);
    for(uint32_t i = 0; i < lv_obj_get_child_count(p->library); i++){
        lv_obj_t *c = lv_obj_get_child(p->library, (int32_t)i);
        if(!lv_obj_check_type(c, &lv_label_class)) continue;
        lv_obj_set_style_text_color(c, TC(DOT_GRID), 0);
        if(strlen(lv_label_get_text(c)) == 3) lv_obj_align(c, LV_ALIGN_CENTER, 0, -10);
        else { char up[24]; lv_label_set_text(c, theme_upper(up, sizeof up, lv_label_get_text(c)));
               lv_obj_set_style_text_font(c, TF(UI_14), 0); lv_obj_align(c, LV_ALIGN_CENTER, 0, 16); }
    }
    lv_obj_set_pos(p->search, 239, 140); lv_obj_set_size(p->search, 58, 58);   /* centred under the Library circle (268) */
    bauh_block(p->search, TC(DECOR_1));
    home_search_glyph(p->search, TC(ON_ACCENT));
    lv_obj_set_style_text_align(p->date, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(p->date, 197, 218); lv_obj_set_width(p->date, 100);         /* right edge = the square's (297) */
    g_bauh_date = p->date;                                                       /* bauh_clock moves it in 12-hour */
    lv_obj_set_style_text_font(p->date, TF(UI_16), 0);
    bauh_clock(lv_label_get_text(p->clock));                                     /* 12-hour: step the column clear of PM */
    /* now playing: an ink band with a red play disc */
    lv_obj_set_pos(p->np, 54, 262); lv_obj_set_size(p->np, 252, 46);   /* its corners inside the round glass */
    bauh_block(p->np, TC(TEXT_PRIMARY));
    home_set_art_enabled(0);
    lv_obj_add_flag(p->np_thumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(p->np_title, 14, 4);  lv_obj_set_width(p->np_title, 180);
    lv_obj_set_pos(p->np_artist, 14, 24); lv_obj_set_width(p->np_artist, 180);
    lv_obj_set_style_text_color(p->np_title, TC(CANVAS), 0);
    lv_obj_set_style_text_color(p->np_artist, TC(CANVAS), 0);
    lv_obj_set_pos(p->pp, 202, 2); lv_obj_set_size(p->pp, 42, 42);   /* 8 px from the band's end */
    bauh_block(p->pp, TC(ACCENT_PRIMARY));
    lv_obj_set_style_radius(p->pp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_color(p->pp_label, TC(ON_ACCENT), 0);
    lv_obj_add_flag(p->hint, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *g_bauh_triangle;
static void bauh_np(const np_parts_t *p){
    own_backgrounds();
    /* the split: the art block on the left, a red rule, everything else on the right; seek is a straight bar */
    lv_obj_add_flag(p->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(p->cover, 176, 360); lv_obj_align(p->cover, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(p->cover, 0, 0);
    lv_obj_set_style_bg_color(p->cover, TC(DECOR_1), 0);
    lv_obj_align(p->cover_img, LV_ALIGN_CENTER, 0, 0);
    lv_image_set_scale(p->cover_img, 623); /* 148 px art fills the 360 px height; parent clips its blue half */
    lv_obj_clear_flag(p->cover, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    g_bauh_triangle = bauh_shape(p->cover, 27, 110, 122, 140, 2, TC(ACCENT_PRIMARY));
    lv_obj_t *rule = lv_obj_create(p->root);
    lv_obj_remove_style_all(rule);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(rule, 176, 0); lv_obj_set_size(rule, 5, 360);
    lv_obj_set_style_bg_color(rule, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_t *txt[3] = { p->title, p->artist, p->album };
    for(int i = 0; i < 3; i++){
        lv_obj_set_style_text_align(txt[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(txt[i], 140);
        lv_obj_align(txt[i], LV_ALIGN_TOP_LEFT, 194, i == 0 ? 74 : 106 + i * 18);
    }
    /* the queue position gets its own line under the album (owner 2026-10-07: "Album - 1605/3285" was cut off) */
    lv_obj_remove_flag(p->pos, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_align(p->pos, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(p->pos, 140);
    lv_obj_align(p->pos, LV_ALIGN_TOP_LEFT, 194, 160);
    lv_obj_set_style_text_font(p->title, TF(UI_20), 0);
    lv_obj_set_width(p->title, 128);   /* a 2-line title starts at y 72, where the glass ends at x ~335: 194+128 fits */
    lv_label_set_long_mode(p->title, LV_LABEL_LONG_WRAP);
    /* the title stands on the artist: its last line ends at y 122 whether it takes one line or two (moved up 18 px
     * into the free top so the position fits on its own line) */
    lv_obj_set_height(p->title, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(p->title, 50, 0);
    lv_obj_align(p->title, LV_ALIGN_BOTTOM_LEFT, 194, -(360 - 122));
    lv_obj_align(p->prev, LV_ALIGN_TOP_LEFT, 196, 204);
    lv_obj_align(p->next, LV_ALIGN_TOP_LEFT, 300, 204);
    lv_obj_set_ext_click_area(p->prev, 8); lv_obj_set_ext_click_area(p->next, 8);   /* never into the play disc */
    lv_obj_set_ext_click_area(p->cover, 0);                                        /* the art stops at the rule */
    lv_obj_set_size(p->pp, 54, 54);
    bauh_block(p->pp, TC(ACCENT_PRIMARY));
    ui_np_pp_filled(1);
    lv_obj_set_style_radius(p->pp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_color(p->pp, TC(ON_ACCENT), 0);
    lv_obj_set_style_text_align(p->pp, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(p->pp, 10, 0);
    lv_obj_align(p->pp, LV_ALIGN_TOP_LEFT, 234, 194);
    lv_obj_set_ext_click_area(p->pp, 4);
    lv_obj_t *bar = lv_bar_create(p->root);
    lv_obj_set_size(bar, 116, 8);
    lv_obj_set_pos(bar, 194, 264);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN); lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, TC(SURFACE_STRONG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    ui_np_seek_line(bar);
    lv_obj_align(p->elapsed, LV_ALIGN_TOP_LEFT, 194, 276);
    lv_obj_set_style_text_align(p->elapsed, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(p->remain, LV_ALIGN_TOP_LEFT, 230, 276);
    lv_obj_set_style_text_align(p->remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(p->sep, LV_OBJ_FLAG_HIDDEN);
    /* shuffle and heart (or moon) in the free top of the right column, fully on the glass */
    lv_obj_set_size(p->mode, 40, 36); lv_obj_align(p->mode, LV_ALIGN_TOP_LEFT, 196, 34);   /* y 34: the heart's box */
    lv_obj_set_size(p->fav, 40, 36);  lv_obj_align(p->fav, LV_ALIGN_TOP_LEFT, 244, 34);    /* still ends inside the */
    lv_obj_set_size(p->sleep, 40, 36); lv_obj_align(p->sleep, LV_ALIGN_TOP_LEFT, 244, 34); /* glass (chord x 285)  */
    lv_obj_add_flag(p->dots[0], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(p->dots[1], LV_OBJ_FLAG_HIDDEN);
}
static void bauh_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    if(g_bauh_triangle){
        if(show && !loading) lv_obj_remove_flag(g_bauh_triangle, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_bauh_triangle, LV_OBJ_FLAG_HIDDEN);
    }
    if(show && !loading) lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN);
    else if(show){ lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN); lv_obj_center(note); }
}
static void bauh_empty_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    const char *saved = lv_obj_get_user_data(o);
    if(!saved || strcmp(saved, lv_label_get_text(o))) return;
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = TC(DECOR_2); d.bg_opa = LV_OPA_COVER;
    lv_draw_rect(lv_event_get_layer(e), &d, &a);
}
static void bauh_empty_deleted(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void bauh_empty(kit_empty_kind_t kind, lv_obj_t *message){
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(USER_16), 0);
    lv_obj_set_style_text_color(message, TC(DOT_GRID), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(message), TF(USER_16), 0, 0, 240, LV_TEXT_FLAG_NONE);
    int w = sz.x + 28; if(w > 270) w = 270; if(w < 80) w = 80;
    lv_obj_set_width(message, w);
    lv_obj_set_height(message, LV_SIZE_CONTENT);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_hor(message, 14, 0); lv_obj_set_style_pad_ver(message, 12, 0);
    lv_obj_set_style_translate_x(message, 0, 0); lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_x(message, 180 - (a.x1 + a.x2)/2, 0);
    lv_obj_set_style_translate_y(message, (kind == KIT_EMPTY_LIBRARY ? 205 : 180) - (a.y1 + a.y2)/2, 0);
    if(lv_obj_has_flag(message, LV_OBJ_FLAG_USER_2)) free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(lv_label_get_text(message)));
    if(!lv_obj_has_flag(message, LV_OBJ_FLAG_USER_2)){
        lv_obj_add_flag(message, LV_OBJ_FLAG_USER_2);
        lv_obj_add_event_cb(message, bauh_empty_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        lv_obj_add_event_cb(message, bauh_empty_deleted, LV_EVENT_DELETE, NULL);
    }
}
static void bauh_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    /* switches are squares, actions are circles; a lit switch is a solid red block */
    lv_obj_set_style_radius(c, toggle ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, on ? TC(ACCENT_PRIMARY) : TC(CANVAS), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, on ? 0 : 3, 0);
    lv_obj_set_style_border_color(c, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    if(g) lv_obj_set_style_text_color(g, on ? TC(ON_ACCENT) : TC(TEXT_PRIMARY), 0);
    if(cap){ char up[32]; lv_label_set_text(cap, theme_upper(up, sizeof up, lv_label_get_text(cap)));
             lv_obj_set_style_text_color(cap, on ? TC(ACCENT_PRIMARY) : TC(TEXT_SECONDARY), 0); }
}
const theme_kit_t theme_kit_bauh = { .id = "bauhaus", .header = bauh_header, .style = bauh_style, .home = bauh_home,
                                     .home_clock = bauh_clock, .nowplaying = bauh_np, .qs_tile = bauh_qs_tile,
                                     .title = bauh_title, .row_decor = bauh_row_decor,
                                     .np_cover_state = bauh_cover, .empty_state = bauh_empty };

/* ================================================================ zine
 * A photocopied music zine: paper grain, typewriter text, glitch-face titles in magenta, a yellow highlighter
 * (DECOR_1) with ink type (DECOR_2) for the pressed row, stamped paper scraps for buttons and popups, the Home time as
 * cut-out digit tiles, the cover taped on. Paper is light in both variants (dark: a light scrap on black). */
static lv_color_t zine_paper(void){ return theme_variant() ? TC(SURFACE) : TC(TEXT_PRIMARY); }
static lv_color_t zine_ink(void){ return theme_variant() ? TC(TEXT_PRIMARY) : TC(CANVAS); }
static void zine_rect(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t color){
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.bg_color = color; d.bg_opa = LV_OPA_COVER;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}
static void zine_title_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    if(lv_obj_get_y(o) >= 100) return; /* the Now Playing song title is not a screen header */
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_point_t text;
    lv_text_get_size(&text, lv_label_get_text(o), lv_obj_get_style_text_font(o, 0),
                     lv_obj_get_style_text_letter_space(o, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = text.x + 18;
    if(w > a.x2 - a.x1 - 2) w = a.x2 - a.x1 - 2;
    if(w < 28) w = 28;
    int x = (a.x1 + a.x2 - w)/2, y = a.y1 + 1;
    /* An offset at each end reads as hand-cut tape; all ink stays on top. */
    zine_rect(lv_event_get_layer(e), x, y + 2, x + w - 1, a.y2 - 2, TC(DECOR_1));
    zine_rect(lv_event_get_layer(e), x + 4, y, x + w - 5, a.y2, TC(DECOR_1));
}
static void zine_title(lv_obj_t *o, const char *text){
    int box = lv_obj_get_style_width(o, 0); /* final style width; layout may still report the old 360 px */
    int room = box > 0 && box != LV_SIZE_CONTENT && !LV_COORD_IS_PCT(box) ? box - 12 : 0;
    if(room > 0){
        const lv_font_t *f = lv_obj_get_style_text_font(o, 0);
        for(int k = 0; k < 6; k++){
            lv_point_t size;
            lv_text_get_size(&size, text, f, lv_obj_get_style_text_letter_space(o, 0),
                             0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if(size.x <= room) break;
            const lv_font_t *smaller = theme_title_smaller(f);
            if(!smaller || smaller == f) break;
            f = smaller;
        }
        lv_obj_set_style_text_font(o, f, 0);
    }
    lv_label_set_text(o, text);
}
static void zine_row_scroll(lv_event_t *e){
    lv_obj_t *row = lv_event_get_user_data(e);
    lv_obj_add_flag(row, LV_OBJ_FLAG_USER_1);
    lv_obj_invalidate(row);
}
static void zine_row_scroll_hooks(lv_obj_t *row, int on){
    for(lv_obj_t *p = row; p; p = lv_obj_get_parent(p)){
        if(!lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) continue;
        lv_obj_remove_event_cb_with_user_data(p, zine_row_scroll, row);
        if(on) lv_obj_add_event_cb(p, zine_row_scroll, LV_EVENT_SCROLL_BEGIN, row);
    }
}
static void zine_row_event(lv_event_t *e){
    lv_obj_t *row = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_DELETE && code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED &&
       code != LV_EVENT_PRESS_LOST) return;
    if(code == LV_EVENT_DELETE || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
        zine_row_scroll_hooks(row, 0);
    if(code == LV_EVENT_PRESSED){
        lv_obj_remove_flag(row, LV_OBJ_FLAG_USER_1);
        zine_row_scroll_hooks(row, 1);
    }
    if(code != LV_EVENT_DELETE) lv_obj_invalidate(row);
}
static void zine_row_draw(lv_event_t *e){
    lv_obj_t *row = lv_event_get_target(e), *title = lv_event_get_user_data(e);
    if(!title || !lv_obj_has_state(row, LV_STATE_PRESSED) || lv_obj_has_flag(row, LV_OBJ_FLAG_USER_1)) return;
    lv_area_t a, t; lv_obj_get_coords(row, &a); lv_obj_get_coords(title, &t);
    lv_point_t text;
    lv_text_get_size(&text, lv_label_get_text(title), lv_obj_get_style_text_font(title, 0),
                     lv_obj_get_style_text_letter_space(title, 0), 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int x1 = t.x1 + 2, x2 = x1 + (text.x < 116 ? text.x : 116);
    if(x2 > a.x2 - 16) x2 = a.x2 - 16;
    if(x2 > x1 + 8){
        int y = t.y2 - 2;
        lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
        d.color = TC(ACCENT_PRIMARY); d.opa = LV_OPA_COVER; d.width = 2;
        d.p1 = (lv_point_precise_t){x1, y}; d.p2 = (lv_point_precise_t){x1 + (x2-x1)*2/3, y + 1};
        lv_draw_line(lv_event_get_layer(e), &d);
        d.p1 = d.p2; d.p2 = (lv_point_precise_t){x2, y - 1};
        lv_draw_line(lv_event_get_layer(e), &d);
    }
}
static void zine_row_decor(lv_obj_t *row, lv_obj_t *title){
    if(!title) return;
    lv_obj_remove_event_cb(row, zine_row_event);
    lv_obj_remove_event_cb(row, zine_row_draw);
    lv_obj_add_event_cb(row, zine_row_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(row, zine_row_draw, LV_EVENT_DRAW_POST, title);
}
static void zine_scrap(lv_obj_t *o, lv_color_t bg){      /* a stamped paper scrap with an ink edge */
    kit_keep(o);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, TC(DECOR_2), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_x(o, 2, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(o, 2, LV_STATE_PRESSED);
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++){       /* ink type on the scrap */
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if(lv_obj_check_type(c, &lv_label_class)) lv_obj_set_style_text_color(c, zine_ink(), 0);
        /* drawn glyph marks (the playlist menu dots) are small filled objects: ink them too, or they vanish on paper */
        else if(lv_obj_check_type(c, &lv_obj_class) && lv_obj_get_style_width(c, 0) <= 6 && lv_obj_get_style_height(c, 0) <= 6
                && lv_obj_get_style_bg_opa(c, 0) > LV_OPA_50) lv_obj_set_style_bg_color(c, zine_ink(), 0);
    }
}
static void zine_row(lv_obj_t *o){
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, TC(DECOR_1), LV_STATE_PRESSED);    /* the highlighter */
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
    /* the row's text inherits the row's colour, so a pressed row turns its type to ink */
    lv_obj_set_style_text_color(o, TC(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_color(o, TC(DECOR_2), LV_STATE_PRESSED);
    for(uint32_t i = 0; i < lv_obj_get_child_count(o); i++){
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if(lv_obj_check_type(c, &lv_label_class) && lv_color_eq(lv_obj_get_style_text_color(c, 0), TC(TEXT_PRIMARY)))
            lv_obj_remove_local_style_prop(c, LV_STYLE_TEXT_COLOR, 0);
    }
}
static void zine_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_TITLE:
        lv_obj_set_style_text_color(o, lv_obj_get_y(o) < 100 ? TC(DECOR_2) : TC(ACCENT_PRIMARY), 0);
        if(lv_obj_get_y(o) < 100){
            lv_obj_remove_event_cb(o, zine_title_draw);
            lv_obj_add_event_cb(o, zine_title_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
        }
        break;
    case KIT_ROW: zine_row(o); break;
    /* row words keep their own case: lower-casing only some rows (menu words in UI faces, not library names in user
     * faces) read as a mistake, and it made "Auto (by IP)" into "auto (by ip)". The typewriter face is the voice. */
    case KIT_ROW_CHEVRON:
        lv_label_set_text(o, ">>");
        lv_obj_set_style_text_font(o, TF(UI_14), 0);
        lv_obj_set_style_text_color(o, TC(ACCENT_PRIMARY), 0);
        break;
    case KIT_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)) zine_scrap(o, zine_paper());
        else if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10){ lv_obj_set_style_radius(o, 0, 0); }
        break;
    case KIT_ICON_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)) zine_scrap(o, zine_paper());
        else lv_obj_set_style_radius(o, 0, 0);
        break;
    case KIT_CARD:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){
            lv_obj_set_style_radius(o, 0, 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_POPUP:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && lv_obj_get_width(o) < 340) zine_scrap(o, zine_paper());
        break;
    case KIT_TOGGLE:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 0, LV_PART_KNOB);
        break;
    case KIT_SLIDER: case KIT_BAR:
        lv_obj_set_style_radius(o, 0, LV_PART_MAIN); lv_obj_set_style_radius(o, 0, LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 0, LV_PART_KNOB);
        lv_obj_set_style_bg_color(o, TC(DECOR_2), LV_PART_KNOB);
        lv_obj_set_style_border_width(o, 2, LV_PART_KNOB);
        lv_obj_set_style_border_color(o, TC(DECOR_1), LV_PART_KNOB);
        break;
    case KIT_TEXTAREA:
        /* Match the search scrap's paper/ink pair, including editing states. */
        lv_obj_set_style_bg_color(o, zine_paper(), 0);
        lv_obj_set_style_text_color(o, zine_ink(), 0);
        lv_obj_set_style_text_color(o, zine_ink(), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_border_color(o, zine_ink(), LV_PART_CURSOR);
        lv_obj_set_style_border_width(o, 2, LV_PART_CURSOR);
        lv_obj_set_style_border_side(o, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
        lv_obj_set_style_bg_color(o, zine_ink(), LV_PART_CURSOR);
        lv_obj_set_style_text_color(o, zine_paper(), LV_PART_CURSOR);
        /* fall through */
    case KIT_ROLLER:
        lv_obj_set_style_radius(o, 0, 0);
        lv_obj_set_style_radius(o, 0, LV_PART_SELECTED);
        lv_obj_set_style_bg_color(o, TC(DECOR_1), LV_PART_SELECTED);
        lv_obj_set_style_text_color(o, TC(DECOR_2), LV_PART_SELECTED);
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 0, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(o, zine_paper(), LV_PART_ITEMS);
        lv_obj_set_style_text_color(o, zine_ink(), LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    default: break;
    }
}
/* the Home time as cut-out tiles: one per digit, alternating paper / magenta / yellow, set a little off the line */
static lv_obj_t *g_zine_digit[4], *g_zine_colon, *g_zine_mer;
static void zine_clock(const char *t){
    if(!g_zine_digit[0]) return;
    char hm[16], mer[8]; clock_parts(t, hm, sizeof hm, mer, sizeof mer);
    char low[8]; lv_label_set_text(g_zine_mer, theme_lower(low, sizeof low, mer));
    if(mer[0]) lv_obj_clear_flag(g_zine_mer, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_zine_mer, LV_OBJ_FLAG_HIDDEN);
    t = hm;
    char d[4] = { '-', '-', '-', '-' };
    const char *c = t ? strchr(t, ':') : NULL;
    if(c){
        int n = (int)(c - t);
        d[0] = n >= 2 ? t[n - 2] : ' '; d[1] = n >= 1 ? t[n - 1] : '-';
        d[2] = c[1] ? c[1] : '-'; d[3] = c[1] && c[2] ? c[2] : '-';
    }
    static const int X[4] = { 48, 112, 192, 256 };
    int shift = d[0] == ' ' ? -32 : 0;                          /* a one-digit hour: the tiles close up, centred */
    for(int i = 0; i < 4; i++){
        char s[2] = { d[i], 0 };
        lv_label_set_text(g_zine_digit[i], s);
        lv_obj_t *tile = lv_obj_get_parent(g_zine_digit[i]);
        if(i == 0 && d[0] == ' ') lv_obj_add_flag(tile, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(tile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(tile, X[i] + shift);
    }
    lv_obj_set_x(g_zine_colon, 165 + shift);             /* its 30 px cell centred between the pairs */
    lv_obj_align_to(g_zine_mer, lv_obj_get_parent(g_zine_digit[3]), LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 4);
}
static void zine_home(const home_parts_t *p){
    own_backgrounds();
    lv_obj_set_pos(p->status, 0, 22);
    lv_obj_add_flag(p->clock, LV_OBJ_FLAG_HIDDEN);
    /* 264 px wide, centred: 8 px between a pair, 24 for the colon; low enough that no corner meets the round glass.
     * Mirrored about the centre; the jitter is vertical */
    static const int X[4] = { 48, 112, 192, 256 }, Y[4] = { 64, 58, 66, 60 };
    lv_color_t bg[4] = { zine_paper(), TC(ACCENT_PRIMARY), TC(DECOR_1), zine_paper() };
    lv_color_t fg[4] = { zine_ink(), TC(ON_ACCENT), TC(DECOR_2), zine_ink() };
    for(int i = 0; i < 4; i++){
        lv_obj_t *tile = lv_obj_create(p->root);
        lv_obj_remove_style_all(tile);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(tile, 56, 70);
        lv_obj_set_pos(tile, X[i], Y[i]);
        lv_obj_set_style_bg_color(tile, bg[i], 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        kit_keep(tile);
        g_zine_digit[i] = lv_label_create(tile);
        lv_obj_set_style_text_font(g_zine_digit[i], TF(CLOCK), 0);
        lv_obj_set_style_text_color(g_zine_digit[i], fg[i], 0);
        lv_obj_center(g_zine_digit[i]);
    }
    lv_obj_t *colon = lv_label_create(p->root);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_font(colon, TF(CLOCK), 0);
    lv_obj_set_style_text_color(colon, TC(TEXT_PRIMARY), 0);
    lv_obj_set_pos(colon, 165, 68);
    g_zine_colon = colon;
    g_zine_mer = mer_label(p->root, TF(UI_12), TC(DECOR_2));
    lv_obj_set_style_bg_color(g_zine_mer, TC(ACCENT_PRIMARY), 0);           /* "pm" on a strip of magenta tape */
    lv_obj_set_style_bg_opa(g_zine_mer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(g_zine_mer, 4, 0);
    zine_clock(lv_label_get_text(p->clock));
    /* the date on a highlighter stroke */
    lv_obj_set_style_bg_color(p->date, TC(DECOR_1), 0);
    lv_obj_set_style_bg_opa(p->date, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(p->date, TC(DECOR_2), 0);
    lv_obj_set_style_pad_hor(p->date, 14, 0);
    lv_obj_set_style_pad_ver(p->date, 2, 0);
    lv_obj_set_width(p->date, LV_SIZE_CONTENT);
    lv_obj_align(p->date, LV_ALIGN_TOP_MID, -6, 146);
    lv_obj_set_pos(p->library, 66, 210); lv_obj_set_size(p->library, 150, 42);
    zine_scrap(p->library, zine_paper());
    home_library_fit(p->library);
    lv_obj_set_pos(p->search, 232, 210); lv_obj_set_size(p->search, 56, 42);
    zine_scrap(p->search, TC(DECOR_1));
    home_search_glyph(p->search, TC(DECOR_2));
    /* now playing: a paper scrap with a strip of magenta tape */
    lv_obj_set_pos(p->np, 58, 266); lv_obj_set_size(p->np, 236, 44);
    zine_scrap(p->np, zine_paper());
    home_set_art_enabled(0);
    lv_obj_set_pos(p->np_thumb, 8, 12); lv_obj_set_size(p->np_thumb, 22, 20);
    lv_obj_set_style_radius(p->np_thumb, 0, 0);
    lv_obj_set_style_bg_opa(p->np_thumb, LV_OPA_TRANSP, 0);
    lv_label_set_text(p->np_glyph, ">>");
    lv_obj_set_style_text_font(p->np_glyph, TF(UI_14), 0);
    lv_obj_set_style_text_color(p->np_glyph, TC(ACCENT_PRIMARY), 0);
    lv_obj_center(p->np_glyph);
    lv_obj_set_pos(p->np_title, 34, 4);  lv_obj_set_width(p->np_title, 150);
    lv_obj_set_pos(p->np_artist, 34, 23); lv_obj_set_width(p->np_artist, 150);
    lv_obj_set_style_text_color(p->np_title, zine_ink(), 0);
    lv_obj_set_style_text_color(p->np_artist, zine_ink(), 0);
    lv_obj_set_pos(p->pp, 186, 0); lv_obj_set_size(p->pp, 42, 38);
    lv_obj_set_style_text_color(p->pp_label, TC(ACCENT_PRIMARY), 0);
    lv_obj_t *tape = lv_obj_create(p->root);
    lv_obj_remove_style_all(tape);
    lv_obj_clear_flag(tape, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tape, 34, 10); lv_obj_set_pos(tape, 50, 260);
    lv_obj_set_style_bg_color(tape, TC(ACCENT_PRIMARY), 0);
    lv_obj_set_style_bg_opa(tape, LV_OPA_80, 0);
    lv_label_set_text(p->hint, ">>");
    lv_obj_set_style_text_font(p->hint, TF(UI_12), 0);
}
static void zine_tape(lv_obj_t *root, int x, int y){
    lv_obj_t *t = lv_obj_create(root);
    lv_obj_remove_style_all(t);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(t, 40, 12); lv_obj_set_pos(t, x, y);
    lv_obj_set_style_bg_color(t, TC(DECOR_1), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_90, 0);
}
static void zine_np(const np_parts_t *p){
    own_backgrounds();
    lv_obj_add_flag(p->ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_radius(p->cover, 0, 0);
    lv_obj_align(p->cover, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_update_layout(p->root);
    lv_area_t a; lv_obj_get_coords(p->cover, &a);
    zine_tape(p->root, a.x1 + 6, a.y1 - 6);                 /* taped on at two corners */
    zine_tape(p->root, a.x2 - 44, a.y2 - 6);
    lv_obj_set_style_text_font(p->title, TF(HEADER), 0);
    lv_obj_set_style_text_color(p->title, TC(ACCENT_PRIMARY), 0);
    lv_obj_align(p->title, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_align(p->artist, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_add_flag(p->album, LV_OBJ_FLAG_HIDDEN);          /* the artist line says it; the album is on Song Info */
    lv_obj_t *bar = lv_bar_create(p->root);                 /* the highlighter stroke is the seek bar */
    lv_obj_set_size(bar, 200, 10);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 246);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN); lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, TC(BORDER_STRONG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, TC(ACCENT_PRIMARY), LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    ui_np_seek_line(bar);
    lv_obj_align(p->elapsed, LV_ALIGN_TOP_LEFT, 80, 258);
    lv_obj_set_style_text_align(p->elapsed, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(p->remain, LV_ALIGN_TOP_RIGHT, -80, 258);
    lv_obj_set_style_text_align(p->remain, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(p->sep, LV_OBJ_FLAG_HIDDEN);
    /* stamped keys: prev / play / next on paper, play in magenta */
    lv_obj_t *tr[3] = { p->prev, p->pp, p->next };
    for(int i = 0; i < 3; i++){
        lv_obj_set_style_text_font(tr[i], TF(UI_20), 0);
        lv_obj_set_size(tr[i], i == 1 ? 56 : 48, 44);
        lv_obj_set_style_text_align(tr[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(tr[i], 6, 0);                  /* 44 - 6 - 2 x 2 border = 34 >= the glyph face */
        zine_scrap(tr[i], i == 1 ? TC(ACCENT_PRIMARY) : zine_paper());
        lv_obj_set_style_text_color(tr[i], i == 1 ? TC(ON_ACCENT) : zine_ink(), 0);
        lv_obj_align(tr[i], LV_ALIGN_TOP_MID, (i - 1) * 66, 280);
        lv_obj_set_ext_click_area(tr[i], 4);
    }
    ui_np_pp_filled(1);
    /* shuffle / heart (or moon) out towards the rim, clear of the taped cover (cover 106..254) */
    lv_obj_set_size(p->mode, 40, 36); lv_obj_align(p->mode, LV_ALIGN_TOP_MID, -118, 136);
    lv_obj_set_size(p->fav, 40, 36);  lv_obj_align(p->fav, LV_ALIGN_TOP_MID, 118, 136);
    lv_obj_set_size(p->sleep, 40, 36); lv_obj_align(p->sleep, LV_ALIGN_TOP_MID, 118, 136);
    lv_obj_add_flag(p->dots[0], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(p->dots[1], LV_OBJ_FLAG_HIDDEN);
}
static void zine_qs_tile(lv_obj_t *c, lv_obj_t *g, lv_obj_t *cap, int on, int toggle){
    (void)toggle;
    zine_scrap(c, on ? TC(DECOR_1) : zine_paper());
    if(g) lv_obj_set_style_text_color(g, on ? TC(DECOR_2) : zine_ink(), 0);
    if(cap){ char low[32]; lv_label_set_text(cap, theme_lower(low, sizeof low, lv_label_get_text(cap)));
             /* on: the word gets a stroke of highlighter, like the date - readable on dark and on paper */
             lv_obj_set_style_text_color(cap, on ? TC(DECOR_2) : TC(TEXT_SECONDARY), 0);
             lv_obj_set_style_bg_color(cap, TC(DECOR_1), 0);
             lv_obj_set_style_bg_opa(cap, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
             lv_obj_set_style_pad_hor(cap, 3, 0); }
}
static void zine_empty_draw(lv_event_t *e){
    lv_obj_t *o = lv_event_get_target(e);
    const char *saved = lv_obj_get_user_data(o);
    if(!saved || strcmp(saved, lv_label_get_text(o))) return; /* Lyrics reuses its label. */
    lv_area_t a; lv_obj_get_coords(o, &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    zine_rect(layer, a.x1 + 4, a.y1 + 5, a.x2, a.y2, TC(ACCENT_PRIMARY));
    zine_rect(layer, a.x1 + 2, a.y1 + 2, a.x2 - 5, a.y2 - 5, zine_paper());
    zine_rect(layer, a.x1 + 7, a.y1, a.x2 - 9, a.y2 - 3, zine_paper());
    zine_rect(layer, a.x1, a.y1 + 8, a.x2 - 4, a.y2 - 9, zine_paper());
}
static void zine_empty_deleted(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void zine_empty(kit_empty_kind_t kind, lv_obj_t *message){
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(UI_14), 0);
    lv_obj_set_style_text_color(message, zine_ink(), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(message), TF(UI_14), 0, 0, 246, LV_TEXT_FLAG_NONE);
    int w = sz.x + 40; if(w < 114) w = 114; if(w > 286) w = 286;
    lv_obj_set_width(message, w);
    lv_obj_set_height(message, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(message, 20, 0);
    lv_obj_set_style_pad_ver(message, 13, 0);
    lv_obj_set_style_translate_x(message, 0, 0);
    lv_obj_set_style_translate_y(message, 0, 0);
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_x(message, 180 - (a.x1 + a.x2)/2, 0);
    stone_empty_at(message, kind == KIT_EMPTY_LYRICS ? 180 : stone_empty_center(message));
    lv_obj_remove_event_cb(message, zine_empty_draw);
    lv_obj_remove_event_cb(message, zine_empty_deleted);
    free(lv_obj_get_user_data(message));
    lv_obj_set_user_data(message, strdup(lv_label_get_text(message)));
    lv_obj_add_event_cb(message, zine_empty_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(message, zine_empty_deleted, LV_EVENT_DELETE, NULL);
}
static void zine_cover_draw(lv_event_t *e){
    lv_obj_t *cover = lv_event_get_target(e);
    lv_obj_t *note = lv_event_get_user_data(e);
    if(!note || lv_obj_has_flag(note, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(cover, &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    zine_rect(layer, a.x1 + 3, a.y1 + 1, a.x2 - 4, a.y2 - 2, zine_paper());
    zine_rect(layer, a.x1, a.y1 + 7, a.x2 - 2, a.y2 - 8, zine_paper());
    zine_rect(layer, a.x1 + 8, a.y1, a.x2 - 10, a.y2, zine_paper());
    zine_rect(layer, a.x1 + 5, a.y1 + 3, a.x2 - 7, a.y2 - 5, zine_paper());
}
static void zine_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    lv_obj_set_style_bg_opa(cover, LV_OPA_TRANSP, 0);
    lv_label_set_text(note, loading ? LV_SYMBOL_REFRESH : LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, TF(UI_32), 0);
    lv_obj_set_style_text_color(note, TC(ACCENT_PRIMARY), 0);
    if(show) lv_obj_center(note);
    lv_obj_remove_event_cb(cover, zine_cover_draw);
    lv_obj_add_event_cb(cover, zine_cover_draw, LV_EVENT_DRAW_MAIN_BEGIN, note);
}
const theme_kit_t theme_kit_zine = { .id = "zine", .header = kit_edge_header, .style = zine_style, .home = zine_home,
                                     .home_clock = zine_clock, .nowplaying = zine_np, .qs_tile = zine_qs_tile,
                                     .empty_state = zine_empty, .np_cover_state = zine_cover,
                                     .row_decor = zine_row_decor, .title = zine_title, .back_text = "<<" };

/* ================================================================ something
 * The owner's design (assets/reference/something/): dot-grid paper, dot-matrix titles, flat hairline rows with accent
 * chevrons, one red. The trait code (theme.c, home.c, ui.c, quicksettings.c) draws Home, Now Playing and Quick
 * Settings; this styler carries the same language to every other screen and popup. */
static void some_style(lv_obj_t *o, kit_role_t r){
    switch(r){
    case KIT_ROW: theme_list_row(o); break;
    case KIT_CARD:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){   /* list cards: flat, a hairline */
            lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(o, 0, 0);
            lv_obj_set_style_border_side(o, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_BUTTON:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && is_neutral_fill(o)){     /* a hairline pill */
            lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(o, TC(SURFACE), 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(o, TC(ACCENT_PRIMARY), LV_STATE_PRESSED);
        } else if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10) lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        break;
    case KIT_POPUP:
        if(lv_obj_get_style_bg_opa(o, 0) > LV_OPA_10 && lv_obj_get_width(o) < 340){
            lv_obj_set_style_radius(o, 24, 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, TC(BORDER_STRONG), 0);
            lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
        }
        break;
    case KIT_KEYBOARD:
        lv_obj_set_style_radius(o, 10, LV_PART_ITEMS);
        break;
    case KIT_TEXT:
        if(is_large_default_text(o)) lv_obj_set_style_text_font(o, TF(UI_24), 0);
        break;
    case KIT_ROW_CHEVRON:
        kit_some_dot_arrow(o, 0);
        break;
    default: break;
    }
}
void kit_some_dot_arrow(lv_obj_t *glyph, int left){
    if(!glyph || lv_obj_get_child_count(glyph)) return;
    static const uint8_t x_right[5] = {0, 5, 10, 5, 0};
    lv_obj_set_style_text_opa(glyph, LV_OPA_TRANSP, 0);
    for(int i = 0; i < 5; i++){
        lv_obj_t *dot = lv_obj_create(glyph);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_pos(dot, left ? 10 - x_right[i] : x_right[i], i * 5);
        lv_obj_set_style_bg_color(dot, TC(ACCENT_PRIMARY), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}
static void some_empty(kit_empty_kind_t kind, lv_obj_t *message){
    (void)kind;
    if(!message) return;
    lv_obj_set_style_text_font(message, TF(UI_16), 0);
    lv_obj_set_style_text_color(message, TC(TEXT_MUTED), 0);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_translate_y(message, 0, 0); /* repeated list/search refreshes must not shift the message */
    lv_obj_update_layout(message);
    lv_area_t a; lv_obj_get_coords(message, &a);
    lv_obj_set_style_translate_y(message, 180 - (a.y1 + a.y2) / 2, 0);
}
static void some_cover(lv_obj_t *cover, lv_obj_t *note, int show, int loading){
    if(!cover || !note) return;
    static lv_obj_t *pattern;
    static uint8_t pixels[140 * 140 * 4];
    if(!pattern){
        pattern = lv_canvas_create(cover);
        lv_canvas_set_buffer(pattern, pixels, 140, 140, LV_COLOR_FORMAT_ARGB8888);
        lv_obj_center(pattern);
        lv_obj_clear_flag(pattern, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        uint32_t dim = theme_rgb(THEME_CLR_BORDER), red = theme_rgb(THEME_CLR_ACCENT_PRIMARY);
        for(int y = 0; y < 14; y++) for(int x = 0; x < 14; x++){
            for(int yy = 0; yy < 2; yy++) for(int xx = 0; xx < 2; xx++){
                uint8_t *p = pixels + ((y * 10 + yy + 4) * 140 + x * 10 + xx + 4) * 4;
                p[0] = dim & 255; p[1] = dim >> 8 & 255; p[2] = dim >> 16 & 255; p[3] = 100;
            }
        }
        static const uint8_t note_xy[][2] = {
            {7,3},{8,3},{9,3},{7,4},{9,4},{7,5},{9,5},{7,6},{9,6},{7,7},{9,7},
            {6,8},{7,8},{8,8},{9,8},{5,9},{6,9},{8,9},{9,9},{5,10},{6,10},{8,10},{9,10}
        };
        for(unsigned i = 0; i < sizeof note_xy / sizeof note_xy[0]; i++){
            for(int yy = 0; yy < 5; yy++) for(int xx = 0; xx < 5; xx++){
                uint8_t *p = pixels + ((note_xy[i][1] * 8 + 14 + yy) * 140 + note_xy[i][0] * 8 + 14 + xx) * 4;
                p[0] = red & 255; p[1] = red >> 8 & 255; p[2] = red >> 16 & 255; p[3] = 255;
            }
        }
        lv_obj_invalidate(pattern);
    }
    lv_obj_set_style_radius(cover, 0, 0);
    if(show && !loading){ lv_obj_remove_flag(pattern, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(pattern, LV_OBJ_FLAG_HIDDEN); if(show) lv_obj_remove_flag(note, LV_OBJ_FLAG_HIDDEN); }
}
static void some_np(const np_parts_t *p){ np_sides_on_cover(p, 0); }   /* times stay: the play circle ends at 316 */
const theme_kit_t theme_kit_something = { .id = "something", .style = some_style, .nowplaying = some_np,
                                          .empty_state = some_empty, .np_cover_state = some_cover };

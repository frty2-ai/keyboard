/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* bwv4d - BlackWidow V4 Pro dial and macro daemon. Shared declarations. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <X11/Xlib.h>
#include <cairo/cairo.h>

#define MAX_SKILLS 32
#define MAX_APPS 8

struct config {
	int osd_ms;            /* how long the corner popup stays */
	int skill_commit_ms;   /* idle time after which a typed skill is final */
	int alt_hold_ms;       /* app switcher: Alt released this long after the last click */
	int long_press_ms;     /* dial press longer than this = back to first profile */
	bool dial_invert, roller_invert;
	char skill_suffix[8];
	char skill_apps[MAX_APPS][32]; int n_skill_apps;
	char skills[MAX_SKILLS][64]; int n_skills;
	int clip_items, clip_mb, clip_item_mb, clip_commit_ms;
	int led_mode;          /* 0 off, 1 flash on profile change, 2 hold while running */
	long led_base;         /* -1 = leave other lamps alone, else 0xRRGGBB */
	int led_dial_lamp;
	uint32_t color[5];     /* per profile: popup accent + dial LED */
	/* optional per-key overrides: m1..m5 = 0..4, side1..side3 = 5..7 */
	char run[8][256];      /* program + args (no shell) */
	char focus[8][64];     /* raise a window whose title contains this instead, if one exists */
};

struct clip_row { bool image; const char *text; char meta[96]; cairo_surface_t *thumb; };

/* config.c */
void config_load(struct config *c);
void state_load(int *profile, int *skill);
void state_save(int profile, int skill);

/* device.c */
int  device_find(char *kbd, char *vendor, char *mouse_ev, char *lamp, size_t n);
int  vendor_set_mode(int fd, int mode);
int  vendor_get_mode(int fd);

/* uinput.c */
int  out_init(void);
void out_close(void);
void out_key(int code, int down);
void out_tap(int code);
void out_chord(int nmods, const int *mods, int key);
int  out_type(const char *s);
void out_backspace(int n);
void out_scroll(int v);

/* x11.c */
bool     x_ensure(void);
Display *x_dpy(void);
Window   x_active(void);
void     x_class(Window w, char *buf, size_t n);
void     x_title(Window w, char *buf, size_t n);
int      x_list(bool (*pred)(const char *cls), Window *out, int max, bool stacking);
void     x_activate(Window w);
bool     x_alive(Window w);
void     x_monitor_rect(Window w, int *x, int *y, int *wd, int *ht);
void     x_process(void);
void     x_on_fatal(void (*fn)(void));

/* clip.c */
bool clip_init(int items, int mb, int item_mb);
void clip_event(XEvent *e);
int  clip_count(void);
void clip_hold(bool on);
void clip_row(int i, struct clip_row *r);
bool clip_choose(int i);

/* led.c */
int  led_open(const char *path);
void led_host(bool on);
void led_fill(uint32_t rgb);
void led_lamp(int id, uint32_t rgb);
void led_close(void);

/* osd.c */
void osd_accent(uint32_t rgb);
void osd_list_begin(int n, void (*get)(int, struct clip_row *), uint32_t accent);
void osd_list_select(int sel);
bool osd_tick(void);
void osd_show(const char *label, const char *text, const char *aside, int dots, int active);
void osd_hide(void);
void osd_expose(Window w);

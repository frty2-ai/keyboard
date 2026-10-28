/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* Corner popup: an override-redirect, click-through, translucent window in the bottom-right of the
 * monitor you are working on. No notification daemon, no sound, no focus change. Created on first use.
 * Two looks: a compact card (profile, skill, action) and an expanded list (clipboard browsing) whose rows
 * glide under a fixed highlight. Animation frames are only drawn while the list is moving. */
#define _GNU_SOURCE
#include "bwv4d.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <cairo/cairo-xlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static Window win;
static Visual *vis;
static int depth, W, H;
static bool argb, mapped;
static cairo_surface_t *surf;
static double ar = 0.27, ag = 0.84, ab = 0.17;              /* accent (Razer green by default) */

/* card */
static char label[64], text[160], aside[32];
static int ndots, adot;

/* list */
#define LMAX 64
#define ROWS 5
#define ROWH 56
#define LIST_W 560
static bool list;
static int ln, lsel;
static double lpos;
static struct { bool image; char text[200]; char meta[96]; cairo_surface_t *thumb; } L[LMAX];

#define PAD 16
#define FS_LABEL 11
#define FS_TEXT 19
#define FS_ASIDE 12
#define DOT_R 3.5
#define DOT_GAP 12
#define MARGIN 28

static void create(Display *d)
{
	int scr = DefaultScreen(d);
	XVisualInfo vi;
	if (XMatchVisualInfo(d, scr, 32, TrueColor, &vi)) { vis = vi.visual; depth = 32; argb = true; }
	else { vis = DefaultVisual(d, scr); depth = DefaultDepth(d, scr); }
	XSetWindowAttributes a = {0};
	a.override_redirect = True;
	a.colormap = XCreateColormap(d, RootWindow(d, scr), vis, AllocNone);
	a.border_pixel = 0;
	a.background_pixel = 0;
	a.event_mask = ExposureMask;
	win = XCreateWindow(d, RootWindow(d, scr), 0, 0, 10, 10, 0, depth, InputOutput, vis,
	                    CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &a);
	XShapeCombineRectangles(d, win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);  /* clicks pass through */
	Atom type = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False), notif = XInternAtom(d, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
	XChangeProperty(d, win, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&notif, 1);
	XStoreName(d, win, "bwv4d");
	surf = cairo_xlib_surface_create(d, win, vis, 10, 10);
}

static void font(cairo_t *c, double size, bool bold)
{
	cairo_select_font_face(c, "Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(c, size);
}

static double text_w(cairo_t *c, const char *s, double size, bool bold)
{
	font(c, size, bold);
	cairo_text_extents_t e;
	cairo_text_extents(c, s, &e);
	return e.x_advance;
}

/* Shorten s in place (UTF-8 safe) with an ellipsis until it fits maxw. */
static void fit(cairo_t *c, char *s, size_t cap, double size, bool bold, double maxw)
{
	if (text_w(c, s, size, bold) <= maxw) return;
	size_t n = strlen(s);
	char tmp[256];
	while (n > 0) {
		n--;
		while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
		snprintf(tmp, sizeof tmp, "%.*s\xe2\x80\xa6", (int)n, s);
		if (text_w(c, tmp, size, bold) <= maxw) break;
	}
	snprintf(s, cap, "%s", tmp);
}

static void rounded(cairo_t *c, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(c);
	cairo_arc(c, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(c, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(c, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(c, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(c);
}

static void card_bg(cairo_t *c, double alpha)
{
	cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(c, 0, 0, 0, 0);
	cairo_paint(c);
	cairo_set_operator(c, CAIRO_OPERATOR_OVER);
	if (argb) rounded(c, 0, 0, W, H, 10); else cairo_rectangle(c, 0, 0, W, H);
	cairo_set_source_rgba(c, 0.07, 0.07, 0.08, argb ? alpha : 1);
	cairo_fill(c);
	cairo_save(c);
	if (argb) { rounded(c, 0, 0, W, H, 10); cairo_clip(c); }
	cairo_rectangle(c, 0, 0, 4, H);
	cairo_set_source_rgb(c, ar, ag, ab);
	cairo_fill(c);
	cairo_restore(c);
}

static void header(cairo_t *c, const char *l, const char *a)
{
	double y = PAD + FS_LABEL;
	font(c, FS_LABEL, true);
	cairo_set_source_rgb(c, 0.62, 0.64, 0.67);
	cairo_move_to(c, PAD + 4, y);
	cairo_show_text(c, l);
	if (a && a[0]) {
		double aw = text_w(c, a, FS_ASIDE, false);
		cairo_move_to(c, W - PAD - aw, y);
		cairo_show_text(c, a);
	}
}

static void draw_card(cairo_t *c)
{
	card_bg(c, 0.92);
	header(c, label, aside);
	double y = PAD + FS_LABEL + 8 + FS_TEXT;
	font(c, FS_TEXT, true);
	cairo_set_source_rgb(c, 0.96, 0.96, 0.96);
	cairo_move_to(c, PAD + 4, y);
	cairo_show_text(c, text);
	if (ndots > 0) {
		y += 14;
		for (int i = 0; i < ndots; i++) {
			cairo_arc(c, PAD + 4 + DOT_R + i * DOT_GAP, y, DOT_R, 0, 2 * M_PI);
			if (i == adot) cairo_set_source_rgb(c, ar, ag, ab);
			else cairo_set_source_rgb(c, 0.32, 0.33, 0.35);
			cairo_fill(c);
		}
	}
}

static void draw_list(cairo_t *c)
{
	card_bg(c, 0.90);                                        /* translucent, but text behind must not bleed through */
	char a[32];
	snprintf(a, sizeof a, "%d / %d", lsel + 1, ln);
	header(c, "CLIPBOARD", a);
	double top = PAD + FS_LABEL + 10, mid = top + ROWS * ROWH / 2.0;
	/* fixed highlight in the middle; rows glide under it */
	cairo_set_source_rgba(c, 1, 1, 1, 0.08);
	rounded(c, 10, mid - ROWH / 2.0 + 3, W - 20, ROWH - 6, 7);
	cairo_fill(c);
	cairo_set_source_rgb(c, ar, ag, ab);
	rounded(c, 10, mid - ROWH / 2.0 + 3, 3, ROWH - 6, 1.5);
	cairo_fill(c);
	cairo_save(c);
	cairo_rectangle(c, 0, top, W, ROWS * ROWH);
	cairo_clip(c);
	int from = (int)floor(lpos) - 3, to = (int)ceil(lpos) + 3;
	for (int i = from < 0 ? 0 : from; i <= to && i < ln; i++) {
		double cy = mid + (i - lpos) * ROWH, d = fabs(i - lpos);
		double alpha = d < 0.5 ? 1.0 : 1.0 - fmin(1.0, d / 2.6) * 0.8;
		double x = PAD + 10;
		if (L[i].image && L[i].thumb) {
			int tw = cairo_image_surface_get_width(L[i].thumb), th = cairo_image_surface_get_height(L[i].thumb);
			cairo_save(c);
			cairo_rectangle(c, x, cy - th / 2.0, tw, th);
			cairo_clip(c);
			cairo_set_source_surface(c, L[i].thumb, x, cy - th / 2.0);
			cairo_paint_with_alpha(c, alpha);
			cairo_restore(c);
			x += 64 + 12;
		} else if (L[i].image) {
			cairo_set_source_rgba(c, 0.4, 0.42, 0.45, alpha);
			rounded(c, x, cy - 18, 52, 36, 4);
			cairo_stroke(c);
			x += 64 + 12;
		}
		font(c, 14, d < 0.5);
		cairo_set_source_rgba(c, 0.96, 0.96, 0.96, alpha);
		cairo_move_to(c, x, cy - 3);
		cairo_show_text(c, L[i].text);
		font(c, 11, false);
		cairo_set_source_rgba(c, 0.62, 0.64, 0.67, alpha);
		cairo_move_to(c, x, cy + 15);
		cairo_show_text(c, L[i].meta);
	}
	cairo_restore(c);
	font(c, 10, false);
	cairo_set_source_rgb(c, 0.45, 0.47, 0.50);
	const char *hint = "stop turning to paste  \xc2\xb7  any key cancels";
	cairo_move_to(c, W - PAD - text_w(c, hint, 10, false), H - 12);
	cairo_show_text(c, hint);
}

static void draw(void)
{
	if (!surf) return;
	cairo_t *c = cairo_create(surf);
	if (list) draw_list(c); else draw_card(c);
	cairo_destroy(c);
	cairo_surface_flush(surf);
	XFlush(x_dpy());
}

static void place_and_map(Display *d)
{
	int mx, my, mw, mh;
	x_monitor_rect(x_active(), &mx, &my, &mw, &mh);
	XMoveResizeWindow(d, win, mx + mw - W - MARGIN, my + mh - H - MARGIN, W, H);
	cairo_xlib_surface_set_size(surf, W, H);
	if (!mapped) { XMapRaised(d, win); mapped = true; }
	else { XRaiseWindow(d, win); draw(); }
	XFlush(d);
}

static Display *ready(void)
{
	Display *d = x_dpy();
	if (!d && (!x_ensure() || !(d = x_dpy()))) return NULL;
	if (!win) create(d);
	return d;
}

void osd_accent(uint32_t rgb) { ar = ((rgb >> 16) & 255) / 255.0; ag = ((rgb >> 8) & 255) / 255.0; ab = (rgb & 255) / 255.0; }

void osd_show(const char *l, const char *t, const char *a, int dots, int active)
{
	Display *d = ready();
	if (!d) return;
	list = false;
	snprintf(label, sizeof label, "%s", l ? l : "");
	snprintf(text, sizeof text, "%s", t ? t : "");
	snprintf(aside, sizeof aside, "%s", a ? a : "");
	ndots = dots;
	adot = active;
	cairo_t *c = cairo_create(surf);
	double tw = text_w(c, text, FS_TEXT, true);
	double lw = text_w(c, label, FS_LABEL, true) + (aside[0] ? 24 + text_w(c, aside, FS_ASIDE, false) : 0);
	cairo_destroy(c);
	double inner = tw > lw ? tw : lw;
	W = (int)(PAD * 2 + 4 + inner);
	if (W < 240) W = 240;
	if (W > 640) W = 640;
	H = PAD + FS_LABEL + 8 + FS_TEXT + (ndots ? 18 : 0) + PAD;
	place_and_map(d);
}

void osd_list_begin(int n, void (*get)(int, struct clip_row *), uint32_t accent)
{
	Display *d = ready();
	if (!d) return;
	osd_accent(accent);
	ln = n > LMAX ? LMAX : n;
	cairo_t *c = cairo_create(surf);
	for (int i = 0; i < ln; i++) {
		struct clip_row r;
		get(i, &r);
		L[i].image = r.image;
		L[i].thumb = r.thumb;
		snprintf(L[i].text, sizeof L[i].text, "%s", r.text ? r.text : "");
		snprintf(L[i].meta, sizeof L[i].meta, "%s", r.meta);
		double room = LIST_W - 2 * PAD - 20 - (r.image ? 76 : 0);
		fit(c, L[i].text, sizeof L[i].text, 14, true, room);
		fit(c, L[i].meta, sizeof L[i].meta, 11, false, room);
	}
	cairo_destroy(c);
	list = true;
	lsel = 0;
	lpos = 0;
	W = LIST_W;
	H = PAD + FS_LABEL + 10 + ROWS * ROWH + 26;
	place_and_map(d);
}

void osd_list_select(int sel)
{
	if (!list) return;
	if (!mapped) lpos = sel;
	lsel = sel < 0 ? 0 : sel >= ln ? ln - 1 : sel;
	draw();
}

/* One animation frame; true while the list is still moving towards the selection. */
bool osd_tick(void)
{
	if (!list || !mapped) return false;
	double d = lsel - lpos;
	if (fabs(d) < 0.01) { lpos = lsel; draw(); return false; }
	lpos += d * 0.38;
	draw();
	return true;
}

void osd_expose(Window w) { if (w == win && mapped) draw(); }

void osd_hide(void)
{
	if (!win || !mapped) return;
	XUnmapWindow(x_dpy(), win);
	XFlush(x_dpy());
	mapped = false;
	list = false;
}

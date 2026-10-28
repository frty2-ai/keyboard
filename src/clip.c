/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* Clipboard history. Event-driven via XFixes selection-owner notifications (no polling).
 * Entries live in RAM only, never on disk. Copies flagged by password managers
 * (x-kde-passwordManagerHint) are skipped. Text (UTF-8) and PNG images, any size up to the
 * per-item cap, both directions via the ICCCM INCR protocol for large transfers. */
#define _GNU_SOURCE
#include "bwv4d.h"
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLIP_MAX 64
#define CHUNK (256 * 1024)

struct entry {
	uint8_t *data;
	size_t len;
	bool image;
	int w, h;
	cairo_surface_t *thumb;
	char preview[200];
	char info[64];
	time_t ts;
};

static struct entry H[CLIP_MAX], pending;
static int N, max_items = 30;
static size_t total, max_bytes = 32u << 20, max_item = 16u << 20;
static bool held, has_pending;

static Display *dpy;
static Window cw;
static int fx_base = -1;
static Atom CLIPBOARD, TARGETS, UTF8, TEXTPLAIN, TEXT_A, PNG, INCR, PROP, PWHINT;

/* incoming */
static enum { F_IDLE, F_TARGETS, F_DATA, F_INCR } fst;
static bool fimage, ftoo_big;
static uint8_t *fbuf;
static size_t flen, fcap;

/* outgoing */
static uint8_t *sdata;
static size_t slen;
static bool simage;
static struct { Window req; Atom prop, type; size_t off; bool on, done; } T[4];

/* ---------- entries ---------- */

static void entry_free(struct entry *e)
{
	free(e->data);
	if (e->thumb) cairo_surface_destroy(e->thumb);
	memset(e, 0, sizeof *e);
}

static void utf8_preview(const uint8_t *s, size_t n, char *out, size_t cap, int *lines, size_t *chars)
{
	size_t o = 0;
	bool space = false;
	*lines = 1;
	*chars = 0;
	for (size_t i = 0; i < n; i++) {
		uint8_t c = s[i];
		if ((c & 0xc0) != 0x80) (*chars)++;
		if (c == '\n') (*lines)++;
		if (o + 5 >= cap) continue;                          /* keep counting, stop writing */
		if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { space = o > 0; continue; }
		if (c < 0x20 || c == 0x7f) continue;
		int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		bool ok = len && i + len <= n;
		for (int k = 1; ok && k < len; k++) ok = (s[i + k] & 0xc0) == 0x80;
		if (space) { out[o++] = ' '; space = false; }
		if (!ok) { out[o++] = '?'; continue; }
		memcpy(out + o, s + i, len);
		o += len;
		i += len - 1;
	}
	out[o] = 0;
}

static void commas(size_t v, char *out, size_t n)
{
	char t[32];
	int l = snprintf(t, sizeof t, "%zu", v), o = 0;
	for (int i = 0; i < l && o < (int)n - 1; i++) {
		if (i && (l - i) % 3 == 0) out[o++] = ',';
		out[o++] = t[i];
	}
	out[o] = 0;
}

struct mem { const uint8_t *p; size_t n, off; };
static cairo_status_t mem_read(void *c, unsigned char *buf, unsigned int len)
{
	struct mem *m = c;
	if (m->off + len > m->n) return CAIRO_STATUS_READ_ERROR;
	memcpy(buf, m->p + m->off, len);
	m->off += len;
	return CAIRO_STATUS_SUCCESS;
}

static void describe(struct entry *e)
{
	if (e->image) {
		if (e->len > 24 && !memcmp(e->data, "\x89PNG", 4)) {
			e->w = (e->data[16] << 24) | (e->data[17] << 16) | (e->data[18] << 8) | e->data[19];
			e->h = (e->data[20] << 24) | (e->data[21] << 16) | (e->data[22] << 8) | e->data[23];
		}
		/* thumbnail only for images up to ~9 MP: decoding is transient (freed at once) but
		   a 4K screenshot already needs ~33 MB while it lasts */
		if (e->w > 0 && e->h > 0 && (long)e->w * e->h <= 9000000L) {
			struct mem m = {e->data, e->len, 0};
			cairo_surface_t *img = cairo_image_surface_create_from_png_stream(mem_read, &m);
			if (cairo_surface_status(img) == CAIRO_STATUS_SUCCESS) {
				double s = 40.0 / e->h;
				if (e->w * s > 64) s = 64.0 / e->w;
				int tw = (int)(e->w * s) ? (int)(e->w * s) : 1, th = (int)(e->h * s) ? (int)(e->h * s) : 1;
				e->thumb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, tw, th);
				cairo_t *c = cairo_create(e->thumb);
				cairo_scale(c, s, s);
				cairo_set_source_surface(c, img, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
				cairo_paint(c);
				cairo_destroy(c);
			}
			cairo_surface_destroy(img);
		}
		snprintf(e->preview, sizeof e->preview, "Image");
		double kb = e->len / 1024.0;
		if (e->w) snprintf(e->info, sizeof e->info, "%d \xc3\x97 %d  \xc2\xb7  %.*f %s", e->w, e->h,
		                   kb >= 1024 ? 1 : 0, kb >= 1024 ? kb / 1024 : kb, kb >= 1024 ? "MB" : "KB");
		else snprintf(e->info, sizeof e->info, "PNG");
	} else {
		int lines;
		size_t chars;
		char c[32];
		utf8_preview(e->data, e->len, e->preview, sizeof e->preview, &lines, &chars);
		commas(chars, c, sizeof c);
		if (lines > 1) snprintf(e->info, sizeof e->info, "%s chars  \xc2\xb7  %d lines", c, lines);
		else snprintf(e->info, sizeof e->info, "%s chars", c);
		if (!e->preview[0]) snprintf(e->preview, sizeof e->preview, "(whitespace)");
	}
}

static void insert(struct entry *e)
{
	for (int i = 0; i < N; i++)                               /* same content: move to front */
		if (H[i].image == e->image && H[i].len == e->len && !memcmp(H[i].data, e->data, e->len)) {
			struct entry keep = H[i];
			memmove(H + 1, H, i * sizeof *H);
			H[0] = keep;
			H[0].ts = e->ts;
			entry_free(e);
			return;
		}
	describe(e);
	if (N == CLIP_MAX || N >= max_items) { total -= H[N - 1].len; entry_free(&H[N - 1]); N--; }
	memmove(H + 1, H, N * sizeof *H);
	H[0] = *e;
	N++;
	total += e->len;
	while (N > 1 && total > max_bytes) { total -= H[N - 1].len; entry_free(&H[N - 1]); N--; }
	memset(e, 0, sizeof *e);
}

static void add(uint8_t *data, size_t len, bool image)
{
	struct entry e = {.data = data, .len = len, .image = image, .ts = time(NULL)};
	if (held) {                                               /* browsing: apply when done */
		if (has_pending) entry_free(&pending);
		pending = e;
		has_pending = true;
		return;
	}
	insert(&e);
}

/* ---------- incoming transfer ---------- */

static void fetch_reset(void) { free(fbuf); fbuf = NULL; flen = fcap = 0; ftoo_big = false; fst = F_IDLE; }

static void append(const uint8_t *p, size_t n)
{
	if (ftoo_big || flen + n > max_item) { ftoo_big = true; return; }
	if (flen + n > fcap) {
		size_t c = fcap ? fcap : 4096;
		while (c < flen + n) c *= 2;
		uint8_t *nb = realloc(fbuf, c);
		if (!nb) { ftoo_big = true; return; }
		fbuf = nb;
		fcap = c;
	}
	memcpy(fbuf + flen, p, n);
	flen += n;
}

static void finish(void)
{
	if (!ftoo_big && flen > 0) {
		uint8_t *d = realloc(fbuf, flen);                     /* trim to size */
		add(d ? d : fbuf, flen, fimage);
		fbuf = NULL;
	}
	fetch_reset();
}

static int read_prop(Window w, bool del, Atom *type)
{
	int fmt;
	unsigned long n, after;
	unsigned char *p = NULL;
	if (XGetWindowProperty(dpy, w, PROP, 0, 0, False, AnyPropertyType, type, &fmt, &n, &after, &p) != Success) return -1;
	if (p) XFree(p);
	if (*type == INCR) { XDeleteProperty(dpy, w, PROP); return 1; }
	if (after > max_item) { ftoo_big = true; XDeleteProperty(dpy, w, PROP); return 0; }
	if (XGetWindowProperty(dpy, w, PROP, 0, (long)(after + 3) / 4, del, AnyPropertyType, type, &fmt, &n, &after, &p) != Success) return -1;
	if (p) { if (fmt == 8) append(p, n); XFree(p); }
	return (int)(n == 0 ? 2 : 0);                             /* 2 = empty (INCR end) */
}

static void on_notify(XSelectionEvent *s)
{
	if (s->requestor != cw || s->selection != CLIPBOARD) return;
	if (s->property == None) { fetch_reset(); return; }
	if (fst == F_TARGETS) {
		Atom t;
		int fmt;
		unsigned long n, after;
		unsigned char *p = NULL;
		if (XGetWindowProperty(dpy, cw, PROP, 0, 1024, True, XA_ATOM, &t, &fmt, &n, &after, &p) != Success || !p) { fetch_reset(); return; }
		Atom *a = (Atom *)p, want = None;
		bool has_utf8 = false, has_plain = false, has_png = false, secret = false;
		for (unsigned long i = 0; i < n; i++) {
			if (a[i] == PWHINT) secret = true;
			else if (a[i] == UTF8) has_utf8 = true;
			else if (a[i] == TEXTPLAIN) has_plain = true;
			else if (a[i] == PNG) has_png = true;
		}
		XFree(p);
		if (secret) { fetch_reset(); return; }
		if (has_utf8) want = UTF8; else if (has_plain) want = TEXTPLAIN; else if (has_png) want = PNG;
		if (want == None) { fetch_reset(); return; }
		fimage = want == PNG;
		fst = F_DATA;
		XConvertSelection(dpy, CLIPBOARD, want, PROP, cw, CurrentTime);
		XFlush(dpy);
	} else if (fst == F_DATA) {
		Atom t;
		int r = read_prop(cw, true, &t);
		if (r == 1) fst = F_INCR;                              /* chunks follow as PropertyNotify */
		else finish();
	}
}

/* ---------- outgoing transfer ---------- */

static void serve_drop(void)
{
	free(sdata);
	sdata = NULL;
	slen = 0;
	for (int i = 0; i < 4; i++) if (T[i].on) { XSelectInput(dpy, T[i].req, NoEventMask); T[i].on = false; }
}

static bool dbg;
static void on_request(XSelectionRequestEvent *r)
{
	if (dbg) { char *n = XGetAtomName(dpy, r->target); fprintf(stderr, "clip: request %s from 0x%lx\n", n, r->requestor); XFree(n); }
	XSelectionEvent ev = {.type = SelectionNotify, .display = r->display, .requestor = r->requestor,
	                      .selection = r->selection, .target = r->target, .property = None, .time = r->time};
	Atom prop = r->property ? r->property : r->target;
	if (sdata && r->selection == CLIPBOARD) {
		bool text_t = r->target == UTF8 || r->target == TEXTPLAIN || r->target == XA_STRING || r->target == TEXT_A;
		if (r->target == TARGETS) {
			Atom list[6] = {TARGETS};
			int n = 1;
			if (simage) list[n++] = PNG;
			else { list[n++] = UTF8; list[n++] = TEXTPLAIN; list[n++] = XA_STRING; list[n++] = TEXT_A; }
			XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, n);
			ev.property = prop;
		} else if ((simage && r->target == PNG) || (!simage && text_t)) {
			Atom type = r->target == TEXT_A ? UTF8 : r->target;
			if (slen <= CHUNK) {
				XChangeProperty(dpy, r->requestor, prop, type, 8, PropModeReplace, sdata, (int)slen);
				ev.property = prop;
			} else {
				for (int i = 0; i < 4; i++) if (!T[i].on) {
					T[i].req = r->requestor; T[i].prop = prop; T[i].type = type; T[i].off = 0; T[i].on = true; T[i].done = false;
					XSelectInput(dpy, r->requestor, PropertyChangeMask);
					long sz = (long)slen;
					XChangeProperty(dpy, r->requestor, prop, INCR, 32, PropModeReplace, (unsigned char *)&sz, 1);
					ev.property = prop;
					break;
				}
			}
		}
	}
	XSendEvent(dpy, r->requestor, False, NoEventMask, (XEvent *)&ev);
	XFlush(dpy);
}

static void on_property(XPropertyEvent *p)
{
	if (p->window == cw && p->atom == PROP && p->state == PropertyNewValue && fst == F_INCR) {
		Atom t;
		if (read_prop(cw, true, &t) == 2) finish();
		return;
	}
	if (p->state != PropertyDelete) return;
	for (int i = 0; i < 4; i++) {
		if (!T[i].on || T[i].req != p->window || T[i].prop != p->atom) continue;
		if (T[i].done || !sdata) { XSelectInput(dpy, T[i].req, NoEventMask); T[i].on = false; break; }
		size_t n = slen - T[i].off < CHUNK ? slen - T[i].off : CHUNK;
		XChangeProperty(dpy, T[i].req, T[i].prop, T[i].type, 8, PropModeReplace, sdata + T[i].off, (int)n);
		T[i].off += n;
		if (n == 0) T[i].done = true;                         /* zero-length chunk ends INCR */
		XFlush(dpy);
		break;
	}
}

/* ---------- API ---------- */

bool clip_init(int items, int mb, int item_mb)
{
	if (dpy) return true;
	if (!(dpy = x_dpy())) return false;
	int ev, err;
	if (!XFixesQueryExtension(dpy, &ev, &err)) { fprintf(stderr, "bwv4d: no XFixes; clipboard history off\n"); dpy = NULL; return false; }
	fx_base = ev;
	dbg = getenv("BWV4D_DEBUG") != NULL;
	if (items > 0) max_items = items < CLIP_MAX ? items : CLIP_MAX;
	if (mb > 0) max_bytes = (size_t)mb << 20;
	if (item_mb > 0) max_item = (size_t)item_mb << 20;
	CLIPBOARD = XInternAtom(dpy, "CLIPBOARD", False);
	TARGETS = XInternAtom(dpy, "TARGETS", False);
	UTF8 = XInternAtom(dpy, "UTF8_STRING", False);
	TEXTPLAIN = XInternAtom(dpy, "text/plain;charset=utf-8", False);
	TEXT_A = XInternAtom(dpy, "TEXT", False);
	PNG = XInternAtom(dpy, "image/png", False);
	INCR = XInternAtom(dpy, "INCR", False);
	PROP = XInternAtom(dpy, "BWV4D_CLIP", False);
	PWHINT = XInternAtom(dpy, "x-kde-passwordManagerHint", False);
	XSetWindowAttributes a = {.event_mask = PropertyChangeMask};
	cw = XCreateWindow(dpy, DefaultRootWindow(dpy), -10, -10, 1, 1, 0, CopyFromParent, InputOnly, CopyFromParent, CWEventMask, &a);
	XFixesSelectSelectionInput(dpy, cw, CLIPBOARD, XFixesSetSelectionOwnerNotifyMask);
	XFlush(dpy);
	return true;
}

void clip_event(XEvent *e)
{
	if (!dpy) return;
	if (e->type == fx_base + XFixesSelectionNotify) {
		XFixesSelectionNotifyEvent *s = (XFixesSelectionNotifyEvent *)e;
		if (dbg && s->selection == CLIPBOARD) fprintf(stderr, "clip: owner -> 0x%lx%s\n", s->owner, s->owner == cw ? " (us)" : "");
		if (s->selection != CLIPBOARD || s->owner == cw || s->owner == None) return;
		fetch_reset();
		fst = F_TARGETS;
		XConvertSelection(dpy, CLIPBOARD, TARGETS, PROP, cw, s->selection_timestamp);
		XFlush(dpy);
		return;
	}
	switch (e->type) {
	case SelectionNotify: on_notify(&e->xselection); break;
	case SelectionRequest: on_request(&e->xselectionrequest); break;
	case PropertyNotify: on_property(&e->xproperty); break;
	case SelectionClear: if (e->xselectionclear.selection == CLIPBOARD) { if (dbg) fprintf(stderr, "clip: lost ownership\n"); serve_drop(); } break;
	}
}

int clip_count(void) { return N; }

void clip_hold(bool on)
{
	held = on;
	if (!on && has_pending) { has_pending = false; insert(&pending); }
}

void clip_row(int i, struct clip_row *r)
{
	memset(r, 0, sizeof *r);
	if (i < 0 || i >= N) return;
	r->image = H[i].image;
	r->text = H[i].preview;
	r->thumb = H[i].thumb;
	long age = (long)(time(NULL) - H[i].ts);
	char a[24];
	if (age < 60) snprintf(a, sizeof a, "just now");
	else if (age < 3600) snprintf(a, sizeof a, "%ldm ago", age / 60);
	else if (age < 86400) snprintf(a, sizeof a, "%ldh ago", age / 3600);
	else snprintf(a, sizeof a, "%ldd ago", age / 86400);
	snprintf(r->meta, sizeof r->meta, "%s  \xc2\xb7  %s", H[i].info, a);
}

/* Make entry i the clipboard (we serve it) and move it to the front. */
bool clip_choose(int i)
{
	if (!dpy || i < 0 || i >= N) return false;
	struct entry keep = H[i];
	memmove(H + 1, H, i * sizeof *H);
	H[0] = keep;
	H[0].ts = time(NULL);
	serve_drop();
	if (!(sdata = malloc(H[0].len))) return false;
	memcpy(sdata, H[0].data, H[0].len);
	slen = H[0].len;
	simage = H[0].image;
	XSetSelectionOwner(dpy, CLIPBOARD, cw, CurrentTime);
	XSync(dpy, False);
	return XGetSelectionOwner(dpy, CLIPBOARD) == cw;
}

/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* X11 helpers: focused window, window lists, activation, monitor geometry. EWMH only, no events on root. */
#define _GNU_SOURCE
#include "bwv4d.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *dpy;
static Window root;
static Atom A_ACTIVE, A_LIST, A_STACK, A_NAME, A_UTF8;
static void (*fatal_cb)(void);

static int on_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }  /* windows vanish; ignore */
static int on_io_error(Display *d) { (void)d; if (fatal_cb) fatal_cb(); _exit(0); }

void x_on_fatal(void (*fn)(void)) { fatal_cb = fn; }
Display *x_dpy(void) { return dpy; }

bool x_ensure(void)
{
	if (dpy) return true;
	if (!(dpy = XOpenDisplay(NULL))) return false;
	XSetErrorHandler(on_error);
	XSetIOErrorHandler(on_io_error);
	root = DefaultRootWindow(dpy);
	A_ACTIVE = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	A_LIST = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	A_STACK = XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False);
	A_NAME = XInternAtom(dpy, "_NET_WM_NAME", False);
	A_UTF8 = XInternAtom(dpy, "UTF8_STRING", False);
	return true;
}

static int windows_prop(Atom a, Window *out, int max)
{
	Atom t; int f; unsigned long n = 0, rest; unsigned char *p = NULL;
	if (XGetWindowProperty(dpy, root, a, 0, 1024, False, XA_WINDOW, &t, &f, &n, &rest, &p) != Success || !p) return 0;
	if (t != XA_WINDOW || f != 32) { XFree(p); return 0; }
	int k = (int)(n < (unsigned long)max ? n : (unsigned long)max);
	for (int i = 0; i < k; i++) out[i] = ((Window *)p)[i];
	XFree(p);
	return k;
}

Window x_active(void)
{
	if (!x_ensure()) return 0;
	Window w = 0;
	return windows_prop(A_ACTIVE, &w, 1) ? w : 0;
}

void x_class(Window w, char *buf, size_t n)
{
	buf[0] = 0;
	if (!w || !x_ensure()) return;
	XClassHint h = {0};
	if (XGetClassHint(dpy, w, &h)) {
		snprintf(buf, n, "%s", h.res_class ? h.res_class : (h.res_name ? h.res_name : ""));
		if (h.res_name) XFree(h.res_name);
		if (h.res_class) XFree(h.res_class);
	}
}

void x_title(Window w, char *buf, size_t n)
{
	buf[0] = 0;
	if (!w || !x_ensure()) return;
	Atom t; int f; unsigned long len = 0, rest; unsigned char *p = NULL;
	if (XGetWindowProperty(dpy, w, A_NAME, 0, 256, False, A_UTF8, &t, &f, &len, &rest, &p) == Success && p) {
		snprintf(buf, n, "%s", (char *)p);
		XFree(p);
		return;
	}
	char *name = NULL;
	if (XFetchName(dpy, w, &name) && name) { snprintf(buf, n, "%s", name); XFree(name); }
}

/* stacking=true: bottom-to-top order (last = most recently raised). */
int x_list(bool (*pred)(const char *cls), Window *out, int max, bool stacking)
{
	if (!x_ensure()) return 0;
	Window all[256];
	int n = windows_prop(stacking ? A_STACK : A_LIST, all, 256), k = 0;
	char cls[128];
	for (int i = 0; i < n && k < max; i++) {
		x_class(all[i], cls, sizeof cls);
		if (pred(cls)) out[k++] = all[i];
	}
	return k;
}

bool x_alive(Window w)
{
	if (!w || !x_ensure()) return false;
	Window all[256];
	int n = windows_prop(A_LIST, all, 256);
	for (int i = 0; i < n; i++) if (all[i] == w) return true;
	return false;
}

void x_activate(Window w)
{
	if (!w || !x_ensure()) return;
	XEvent e = {0};
	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = A_ACTIVE;
	e.xclient.format = 32;
	e.xclient.data.l[0] = 2;            /* source: pager, so the WM honours it */
	e.xclient.data.l[1] = CurrentTime;
	XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
	XFlush(dpy);
}

/* Monitor that contains the centre of window w (or the primary one). */
void x_monitor_rect(Window w, int *x, int *y, int *wd, int *ht)
{
	*x = *y = 0;
	*wd = DisplayWidth(dpy, DefaultScreen(dpy));
	*ht = DisplayHeight(dpy, DefaultScreen(dpy));
	int cx = -1, cy = -1;
	if (w) {
		XWindowAttributes a;
		Window child;
		if (XGetWindowAttributes(dpy, w, &a) && XTranslateCoordinates(dpy, w, root, 0, 0, &cx, &cy, &child)) {
			cx += a.width / 2;
			cy += a.height / 2;
		}
	}
	int n = 0;
	XRRMonitorInfo *m = XRRGetMonitors(dpy, root, True, &n);
	if (!m) return;
	int pick = -1;
	for (int i = 0; i < n; i++) {
		if (cx >= m[i].x && cx < m[i].x + m[i].width && cy >= m[i].y && cy < m[i].y + m[i].height) { pick = i; break; }
		if (m[i].primary && pick < 0) pick = i;
	}
	if (pick < 0 && n) pick = 0;
	if (pick >= 0) { *x = m[pick].x; *y = m[pick].y; *wd = m[pick].width; *ht = m[pick].height; }
	XRRFreeMonitors(m);
}

void x_process(void)
{
	if (!dpy) return;
	while (XPending(dpy)) {
		XEvent e;
		XNextEvent(dpy, &e);
		if (e.type == Expose) { if (e.xexpose.count == 0) osd_expose(e.xexpose.window); }
		else clip_event(&e);
	}
}

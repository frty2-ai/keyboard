/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* bwv4d - Razer BlackWidow V4 Pro dial and macro daemon.
 *
 * One thread, one epoll loop, no polling: it sleeps until the keyboard, a timer, a hotplug
 * event or the X server has something. It reads
 *   IF1 hidraw  - report ID 4 special-key codes (M1-M5, dial press, side keys, media)
 *                 and report ID 1 only to notice "a key was typed" (never stored or logged)
 *   IF2 evdev   - grabbed; dial rotation (REL_HWHEEL) and volume roller (REL_WHEEL)
 *   IF3 hidraw  - vendor channel: driver mode on connect, normal mode on exit
 * and writes through two uinput devices. See docs/PROBE_PLAN.md for how all of this was verified. */
#define _GNU_SOURCE
#include "bwv4d.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum { P_SKILLS, P_CLIP, P_VSCODE, P_APPS, P_TABS, P_COUNT };
static const char *PNAME[P_COUNT] = {"Skills", "Clipboard", "VS Code windows", "App switcher", "Tabs"};
enum { T_KBD = 1, T_MOUSE, T_SIG, T_NL, T_X, T_OSD, T_ALT, T_SKILL, T_RESCAN, T_CLIP, T_ANIM, T_LED };
enum app { A_OTHER, A_CLAUDE, A_VSCODE, A_TERM };

static struct config cfg;
static int ep, t_osd, t_alt, t_skill, t_rescan, t_clip, t_anim, t_led, sigfd, nlfd = -1;
static bool led_on;
static int kfd = -1, mfd = -1, vfd = -1;
static bool x_in_ep, connected;
static int rescan_tries;
static uint8_t down[32];  /* ID4 codes currently held (bitset) */

static struct {
	int profile, prev_profile, skill;
	bool alt_held, skill_active, clip_active;
	int clip_sel;
	Window skill_win, prev_win;
	int skill_len;
	struct timespec press_ts;
} S;

static const int CTRL = KEY_LEFTCTRL, SHIFT = KEY_LEFTSHIFT, ALT = KEY_LEFTALT, META = KEY_LEFTMETA;
#define CHORD(key, ...) do { const int m_[] = {__VA_ARGS__}; out_chord(sizeof m_ / sizeof *m_, m_, key); } while (0)

/* ---------- small helpers ---------- */

static void arm(int fd, int ms)
{
	struct itimerspec it = {0};
	it.it_value.tv_sec = ms / 1000;
	it.it_value.tv_nsec = (long)(ms % 1000) * 1000000L;
	timerfd_settime(fd, 0, &it, NULL);
}
static void arm_every(int fd, int ms)
{
	struct itimerspec it = {0};
	it.it_value.tv_nsec = it.it_interval.tv_nsec = (long)ms * 1000000L;
	timerfd_settime(fd, 0, &it, NULL);
}
static void disarm(int fd) { struct itimerspec it = {0}; timerfd_settime(fd, 0, &it, NULL); }
static void drain(int fd) { uint64_t x; while (read(fd, &x, sizeof x) > 0) {} }
static int mod(int a, int n) { return n ? ((a % n) + n) % n : 0; }

static void add_fd(int fd, uint32_t tag)
{
	struct epoll_event e = {.events = EPOLLIN, .data.u32 = tag};
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
}

static void ensure_x_in_loop(void)
{
	if (!x_in_ep && x_dpy()) {
		add_fd(ConnectionNumber(x_dpy()), T_X);
		x_in_ep = true;
		clip_init(cfg.clip_items, cfg.clip_mb, cfg.clip_item_mb);
	}
}

static void osd(const char *label, const char *text, const char *aside, int dots, int active)
{
	osd_accent(cfg.color[S.profile]);
	osd_show(label, text, aside, dots, active);
	ensure_x_in_loop();
	arm(t_osd, cfg.osd_ms);
}

static void osd_profile(void) { osd("DIAL PROFILE", PNAME[S.profile], NULL, P_COUNT, S.profile); }
static void paste(int a);

static bool is_claude(const char *c) { return strcasestr(c, "claude") != NULL; }
static bool is_vscode(const char *c) { return !strcasecmp(c, "code") || !strncasecmp(c, "code_", 5) || strcasestr(c, "vscode"); }
static bool is_term(const char *c)
{
	static const char *t[] = {"ghostty", "gnome-terminal", "kitty", "alacritty", "xterm", "konsole", "tilix",
	                          "terminator", "wezterm", "foot", "ptyxis"};
	for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (strcasestr(c, t[i])) return true;
	return false;
}

static enum app app_of(Window w)
{
	char c[128];
	x_class(w, c, sizeof c);
	if (is_claude(c)) return A_CLAUDE;
	if (is_vscode(c)) return A_VSCODE;
	if (is_term(c)) return A_TERM;
	return A_OTHER;
}

static void launch(const char *desktop_id)
{
	char *argv[] = {"gtk-launch", (char *)desktop_id, NULL};
	posix_spawnattr_t a;
	posix_spawnattr_init(&a);
	posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSID);
	pid_t pid;
	if (posix_spawnp(&pid, "gtk-launch", NULL, &a, argv, environ)) fprintf(stderr, "bwv4d: cannot run gtk-launch\n");
	posix_spawnattr_destroy(&a);
}

/* Configured override for a key: raise a matching window, else run the command. */
static bool any_window(const char *c) { (void)c; return true; }

static void spawn_line(const char *line)
{
	char buf[256], *argv[17];
	int n = 0;
	snprintf(buf, sizeof buf, "%s", line);
	for (char *t = strtok(buf, " \t"); t && n < 16; t = strtok(NULL, " \t")) argv[n++] = t;
	argv[n] = NULL;
	if (!n) return;
	posix_spawnattr_t a;
	posix_spawnattr_init(&a);
	posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSID);
	pid_t pid;
	if (posix_spawnp(&pid, argv[0], NULL, &a, argv, environ)) fprintf(stderr, "bwv4d: cannot run %s\n", argv[0]);
	posix_spawnattr_destroy(&a);
}

static bool custom_key(int idx)
{
	if (!cfg.run[idx][0] && !cfg.focus[idx][0]) return false;
	if (cfg.focus[idx][0]) {
		Window st[256];
		char t[256];
		int n = x_list(any_window, st, 256, true);
		for (int i = n - 1; i >= 0; i--) {                    /* topmost first */
			x_title(st[i], t, sizeof t);
			if (strcasestr(t, cfg.focus[idx])) { x_activate(st[i]); return true; }
		}
	}
	if (cfg.run[idx][0]) {
		spawn_line(cfg.run[idx]);
		osd("OPENING", cfg.focus[idx][0] ? cfg.focus[idx] : cfg.run[idx], NULL, 0, 0);
	}
	return true;
}

/* ---------- app-aware clipboard ---------- */
/* VS Code can't be told apart editor-vs-terminal from X11, so it gets private chords that
   packaging/vscode-keybindings.json resolves with "when": "terminalFocus". */
static void select_all_copy(enum app a)
{
	if (a == A_TERM) { CHORD(KEY_A, CTRL, SHIFT); usleep(20000); CHORD(KEY_C, CTRL, SHIFT); }
	else if (a == A_VSCODE) { CHORD(KEY_INSERT, CTRL, ALT, SHIFT); usleep(30000); CHORD(KEY_END, CTRL, ALT, SHIFT); }
	else { CHORD(KEY_A, CTRL); usleep(20000); CHORD(KEY_C, CTRL); }
}
static void copy_sel(enum app a)
{
	if (a == A_TERM) CHORD(KEY_C, CTRL, SHIFT);
	else if (a == A_VSCODE) CHORD(KEY_END, CTRL, ALT, SHIFT);
	else CHORD(KEY_C, CTRL);
}
static void paste(int a)
{
	if (a == A_TERM) CHORD(KEY_V, CTRL, SHIFT);
	else if (a == A_VSCODE) CHORD(KEY_HOME, CTRL, ALT, SHIFT);
	else CHORD(KEY_V, CTRL);
}

/* ---------- profiles ---------- */

static void alt_release(void)
{
	if (S.alt_held) { out_key(ALT, 0); S.alt_held = false; }
	disarm(t_alt);
}

static void skill_commit(void)
{
	if (!S.skill_active) return;
	S.skill_active = false;
	disarm(t_skill);
	state_save(S.profile, S.skill);
}

static bool skill_app_ok(Window w)
{
	char c[128];
	x_class(w, c, sizeof c);
	if (!c[0]) return false;
	for (int i = 0; i < cfg.n_skill_apps; i++) if (strcasestr(c, cfg.skill_apps[i])) return true;
	return false;
}

static void skills_turn(int dir)
{
	Window w = x_active();
	if (!skill_app_ok(w)) { out_scroll(-dir); return; }   /* elsewhere: plain vertical scroll */
	if (!cfg.n_skills) { osd("SKILLS", "Add skills to ~/.config/bwv4d/config", NULL, 0, 0); return; }
	if (S.skill_active && S.skill_win == w) {
		S.skill = mod(S.skill + dir, cfg.n_skills);
		out_backspace(S.skill_len);
	} else {                                              /* new insertion starts at the remembered skill */
		S.skill = mod(S.skill, cfg.n_skills);
		S.skill_active = true;
		S.skill_win = w;
	}
	char t[96];
	snprintf(t, sizeof t, "%s%s", cfg.skills[S.skill], cfg.skill_suffix);
	S.skill_len = out_type(t);
	arm(t_skill, cfg.skill_commit_ms);
	char a[16];
	snprintf(a, sizeof a, "%d/%d", S.skill + 1, cfg.n_skills);
	osd("SKILL", cfg.skills[S.skill], a, 0, 0);
}

static void project_of(Window w, char *out, size_t n)
{
	char t[256];
	x_title(w, t, sizeof t);
	char *e = strstr(t, " - Visual Studio Code");
	if (e) *e = 0;
	char *last = t;
	for (char *q = t; (q = strstr(q, " - ")); q += 3) last = q + 3;
	if (!strncmp(last, "\xe2\x97\x8f ", 4)) last += 4;   /* unsaved-changes dot */
	snprintf(out, n, "%s", *last ? last : "VS Code");
}

static void vscode_step(int dir)
{
	Window list[64], stack[64];
	int n = x_list(is_vscode, list, 64, false);
	if (!n) { osd("VS CODE", "No windows open", NULL, 0, 0); return; }
	Window a = x_active();
	int cur = -1;
	for (int i = 0; i < n; i++) if (list[i] == a) cur = i;
	if (cur < 0) {                                        /* not in VS Code: jump to the most recent one */
		int m = x_list(is_vscode, stack, 64, true);
		Window top = m ? stack[m - 1] : list[0];
		for (int i = 0; i < n; i++) if (list[i] == top) cur = i;
		if (cur < 0) cur = 0;
	} else cur = mod(cur + dir, n);
	x_activate(list[cur]);
	char p[160], c[16];
	project_of(list[cur], p, sizeof p);
	snprintf(c, sizeof c, "%d/%d", cur + 1, n);
	osd("VS CODE", p, c, 0, 0);
}

static void apps_turn(int dir)
{
	if (!S.alt_held) { out_key(ALT, 1); S.alt_held = true; }
	if (dir < 0) { out_key(SHIFT, 1); out_tap(KEY_TAB); out_key(SHIFT, 0); }
	else out_tap(KEY_TAB);
	arm(t_alt, cfg.alt_hold_ms);
}

/* Dial LED: hold = host mode while running (others at led_base unless "keep");
   flash = colour for a moment on each change, then the firmware's lighting again. */
static void led_show(void)
{
	if (!led_on) return;
	uint32_t col = cfg.color[S.profile];
	if (cfg.led_mode == 1) {
		led_host(true);
		if (cfg.led_base >= 0) led_fill((uint32_t)cfg.led_base);
		led_lamp(cfg.led_dial_lamp, col);
		arm(t_led, cfg.osd_ms + 300);
	} else led_lamp(cfg.led_dial_lamp, col);
}

static void led_connect(const char *path)
{
	led_on = false;
	if (!cfg.led_mode || !path[0]) return;
	if (led_open(path) < 0) { fprintf(stderr, "bwv4d: no access to %s; dial LED off\n", path); return; }
	led_on = true;
	if (cfg.led_mode == 2) {
		led_host(true);
		if (cfg.led_base >= 0) led_fill((uint32_t)cfg.led_base);
	}
	led_show();
}

/* Clipboard profile: turning browses the history in the expanded popup (nothing is typed);
   stop for clip_commit_ms and the highlighted entry becomes the clipboard and is pasted. */
static void clip_cancel(void)
{
	if (!S.clip_active) return;
	S.clip_active = false;
	disarm(t_clip);
	disarm(t_anim);
	osd_hide();
	clip_hold(false);
}

static void clip_commit(void)
{
	if (!S.clip_active) return;
	int sel = S.clip_sel;
	S.clip_active = false;
	disarm(t_clip);
	disarm(t_anim);
	osd_hide();
	bool ok = clip_choose(sel);
	clip_hold(false);
	if (ok) paste(app_of(x_active()));
	else osd("CLIPBOARD", "Could not take the clipboard", NULL, 0, 0);
}

static void clip_turn(int dir)
{
	int n = clip_count();
	if (!n) { osd("CLIPBOARD", "Nothing copied yet", NULL, 0, 0); return; }
	if (!S.clip_active) {
		S.clip_active = true;
		clip_hold(true);                                  /* freeze the list while browsing */
		disarm(t_osd);
		osd_list_begin(n, clip_row, cfg.color[P_CLIP]);
		ensure_x_in_loop();
		S.clip_sel = (dir > 0 && n > 1) ? 1 : 0;          /* entry 0 is already the clipboard */
	} else {
		S.clip_sel += dir;
		if (S.clip_sel < 0) S.clip_sel = 0;
		if (S.clip_sel >= n) S.clip_sel = n - 1;
	}
	osd_list_select(S.clip_sel);
	arm_every(t_anim, 16);
	arm(t_clip, cfg.clip_commit_ms);
}

static void dial_turn(int dir)
{
	switch (S.profile) {
	case P_SKILLS: skills_turn(dir); break;
	case P_CLIP:   clip_turn(dir); break;
	case P_VSCODE: vscode_step(dir); break;
	case P_APPS:   apps_turn(dir); break;
	case P_TABS:   CHORD(dir > 0 ? KEY_PAGEDOWN : KEY_PAGEUP, CTRL); break;
	}
}

static void set_profile(int p)
{
	alt_release();
	skill_commit();
	clip_cancel();
	S.profile = p;
	state_save(S.profile, S.skill);
	osd_profile();
	led_show();
}

/* ---------- buttons ---------- */

static Window claude_window(void)
{
	Window st[16];
	int n = x_list(is_claude, st, 16, true);
	return n ? st[n - 1] : 0;
}

static void mkey(int m)
{
	alt_release();
	skill_commit();
	clip_cancel();
	Window w = x_active();
	enum app a = app_of(w);
	switch (m) {
	case 1: select_all_copy(a); osd("M1", "Copied all", NULL, 0, 0); break;
	case 2: paste(a); break;
	case 3: {                                             /* send selection to Claude */
		if (a == A_CLAUDE) { osd("M3", "Already in Claude", NULL, 0, 0); break; }
		copy_sel(a);
		usleep(150000);
		Window c = claude_window();
		if (!c) { launch("com.anthropic.Claude"); osd("M3", "Starting Claude, copied", NULL, 0, 0); break; }
		S.prev_win = w;
		x_activate(c);
		usleep(300000);
		CHORD(KEY_V, CTRL);
		osd("M3", "Sent to Claude", NULL, 0, 0);
		break;
	}
	case 4: {                                             /* Claude <-> previous window */
		if (a == A_CLAUDE) {
			if (x_alive(S.prev_win)) x_activate(S.prev_win);
			break;
		}
		Window c = claude_window();
		S.prev_win = w;
		if (c) x_activate(c);
		else { launch("com.anthropic.Claude"); osd("M4", "Starting Claude", NULL, 0, 0); }
		break;
	}
	case 5: vscode_step(+1); break;
	}
}

static void on_code(int code, bool press)
{
	int key = code >= 0x20 && code <= 0x24 ? code - 0x20 : code >= 0x63 && code <= 0x65 ? code - 0x63 + 5 : -1;
	if (key >= 0 && (cfg.run[key][0] || cfg.focus[key][0])) {
		if (press) { alt_release(); skill_commit(); clip_cancel(); custom_key(key); }
		return;
	}
	switch (code) {
	case 0x60:                                            /* dial press */
		if (press) clock_gettime(CLOCK_MONOTONIC, &S.press_ts);
		else {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long ms = (now.tv_sec - S.press_ts.tv_sec) * 1000 + (now.tv_nsec - S.press_ts.tv_nsec) / 1000000;
			set_profile(ms >= cfg.long_press_ms ? 0 : (S.profile + 1) % P_COUNT);
		}
		break;
	case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
		if (press) mkey(code - 0x1f);
		break;
	case 0x63: if (press) { alt_release(); CHORD(KEY_S, META, SHIFT); } break;   /* Win+Shift+S, as in normal mode */
	case 0x64: if (press) out_tap(KEY_SYSRQ); break;                              /* GNOME screenshot UI */
	case 0x65:                                                                    /* skills on/off */
		if (press) {
			if (S.profile != P_SKILLS) { S.prev_profile = S.profile; set_profile(P_SKILLS); }
			else set_profile(S.prev_profile != P_SKILLS ? S.prev_profile : P_VSCODE);
		}
		break;
	case 0x50: out_key(KEY_VOLUMEDOWN, press); break;
	case 0x51: out_key(KEY_VOLUMEUP, press); break;
	case 0x52: out_key(KEY_MUTE, press); break;
	case 0x53: out_key(KEY_NEXTSONG, press); break;
	case 0x54: out_key(KEY_PREVIOUSSONG, press); break;
	case 0x55: out_key(KEY_PLAYPAUSE, press); break;
	default: break;                                       /* 0x01 = Fn, nothing to do */
	}
}

/* ---------- device ---------- */

static void release_all_codes(void)
{
	for (int c = 0; c < 256; c++)
		if (down[c >> 3] & (1 << (c & 7))) { down[c >> 3] &= ~(1 << (c & 7)); if (c >= 0x50 && c <= 0x55) on_code(c, false); }
}

static void disconnect(bool restore)
{
	if (restore && vfd >= 0 && vendor_set_mode(vfd, 0) == 0) fprintf(stderr, "bwv4d: keyboard back in normal mode\n");
	if (kfd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, kfd, NULL); close(kfd); }
	if (mfd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, mfd, NULL); close(mfd); }  /* closing releases the grab */
	if (vfd >= 0) close(vfd);
	kfd = mfd = vfd = -1;
	release_all_codes();
	alt_release();
	skill_commit();
	clip_cancel();
	if (led_on) { led_close(); led_on = false; }
	connected = false;
}

static int try_connect(void)
{
	char kp[64], vp[64], mp[64], lp[64];
	if (device_find(kp, vp, mp, lp, sizeof kp) < 0) return -1;
	kfd = open(kp, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	vfd = open(vp, O_RDWR | O_CLOEXEC);
	mfd = open(mp, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (kfd < 0 || vfd < 0 || mfd < 0) {
		fprintf(stderr, "bwv4d: cannot open %s / %s / %s: %s (udev rule installed?)\n", kp, vp, mp, strerror(errno));
		disconnect(false);
		return -2;
	}
	if (ioctl(mfd, EVIOCGRAB, 1) < 0) fprintf(stderr, "bwv4d: warning: could not grab %s: %s\n", mp, strerror(errno));
	if (vendor_set_mode(vfd, 3) < 0 || vendor_get_mode(vfd) != 3) {
		fprintf(stderr, "bwv4d: keyboard did not enter driver mode\n");
		disconnect(true);
		return -3;
	}
	memset(down, 0, sizeof down);
	add_fd(kfd, T_KBD);
	add_fd(mfd, T_MOUSE);
	connected = true;
	rescan_tries = 0;
	fprintf(stderr, "bwv4d: connected (%s, %s, %s), driver mode on\n", kp, mp, vp);
	led_connect(lp);
	return 0;
}

static void read_kbd(void)
{
	uint8_t b[64];
	ssize_t r;
	while ((r = read(kfd, b, sizeof b)) > 0) {
		if (b[0] == 4) {
			uint8_t now[32] = {0};
			for (ssize_t i = 1; i < r; i++) if (b[i]) now[b[i] >> 3] |= 1 << (b[i] & 7);
			for (int c = 1; c < 256; c++) {
				bool was = down[c >> 3] & (1 << (c & 7)), is = now[c >> 3] & (1 << (c & 7));
				if (was != is) { if (is) down[c >> 3] |= 1 << (c & 7); else down[c >> 3] &= ~(1 << (c & 7)); on_code(c, is); }
			}
		} else if (b[0] == 1 && (S.skill_active || S.clip_active)) {
			for (ssize_t i = 1; i < r; i++) if (b[i]) { skill_commit(); clip_cancel(); break; }   /* you typed */
		}
	}
	if (r < 0 && errno != EAGAIN) { fprintf(stderr, "bwv4d: keyboard gone\n"); disconnect(false); }
}

static void read_mouse(void)
{
	struct input_event ev[16];
	ssize_t r;
	while ((r = read(mfd, ev, sizeof ev)) > 0) {
		for (unsigned i = 0; i < r / sizeof *ev; i++) {
			if (ev[i].type != EV_REL) continue;
			int v = ev[i].value;
			if (ev[i].code == REL_HWHEEL) {
				if (cfg.dial_invert) v = -v;
				for (int k = 0; k < abs(v); k++) dial_turn(v > 0 ? 1 : -1);
			} else if (ev[i].code == REL_WHEEL) {
				if (cfg.roller_invert) v = -v;
				for (int k = 0; k < abs(v); k++) out_tap(v > 0 ? KEY_VOLUMEUP : KEY_VOLUMEDOWN);
			}
		}
	}
	if (r < 0 && errno != EAGAIN) { fprintf(stderr, "bwv4d: dial node gone\n"); disconnect(false); }
}

static void read_netlink(void)
{
	char buf[4096];
	ssize_t r;
	while ((r = recv(nlfd, buf, sizeof buf - 1, 0)) > 0) {
		buf[r] = 0;
		if (!strncmp(buf, "add@", 4) && strcasestr(buf, "1532:028D") && strstr(buf, "hidraw") && !connected) arm(t_rescan, 1000);
	}
}

/* ---------- lifecycle ---------- */

static void cleanup(void)
{
	alt_release();
	disconnect(true);
	out_close();
}

static int restore_only(void)
{
	char kp[64], vp[64], mp[64], lp[64];
	device_find(kp, vp, mp, lp, sizeof kp);
	if (!vp[0]) { fprintf(stderr, "bwv4d: keyboard not found\n"); return 1; }
	if (lp[0] && led_open(lp) == 0) led_close();          /* lighting back to the firmware */
	int fd = open(vp, O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror(vp); return 1; }
	int ok = vendor_set_mode(fd, 0) == 0 && vendor_get_mode(fd) == 0;
	close(fd);
	fprintf(stderr, ok ? "bwv4d: normal mode restored\n" : "bwv4d: restore failed; replug the keyboard\n");
	return ok ? 0 : 1;
}

/* Shows each popup once without touching the keyboard: for checking looks and placement. */
static void demo_wait(int ms)
{
	for (int t = 0; t < ms; t += 20) { x_process(); usleep(20000); }
}
static void demo_row(int i, struct clip_row *r)
{
	static const char *t[] = {"git rebase -i --autosquash origin/main", "Image", "const handler = async (req, res) => { const body = await req.json(); return res.status(200).json({ ok: true, items: body.items.map(normalize) }) }",
	                          "https://github.com/openrazer/openrazer/issues/2228", "Image", "SELECT id, name FROM users WHERE created_at > now() - interval '7 days';",
	                          "Thanks, that works. Ship it.", "sudo udevadm control --reload && sudo udevadm trigger"};
	static const char *m[] = {"38 chars", "1920 \xc3\x97 1080  \xc2\xb7  412 KB", "3,412 chars  \xc2\xb7  87 lines", "47 chars", "3840 \xc3\x97 2160  \xc2\xb7  2.1 MB",
	                          "71 chars", "28 chars", "54 chars"};
	memset(r, 0, sizeof *r);
	r->image = i == 1 || i == 4;
	r->text = t[i];
	snprintf(r->meta, sizeof r->meta, "%s  \xc2\xb7  %dm ago", m[i], i * 3 + 1);
}

/* Clipboard self-test: watch for `secs`, print the history, then serve entry `pick` for 4 s. */
static int clip_test(int secs, int pick)
{
	config_load(&cfg);
	if (!x_ensure() || !clip_init(cfg.clip_items, cfg.clip_mb, cfg.clip_item_mb)) return 1;
	int last = -1;
	for (int t = 0; t < secs * 50; t++) {
		x_process();
		usleep(20000);
		if (clip_count() != last) {
			last = clip_count();
			struct clip_row r;
			clip_row(0, &r);
			fprintf(stderr, "history=%d  newest: [%s] %.60s | %s\n", last, r.image ? "img" : "txt", r.text, r.meta);
		}
	}
	fprintf(stderr, "choose %d: %s\n", pick, clip_choose(pick) ? "owner" : "FAILED");
	for (int t = 0; t < 200; t++) { x_process(); usleep(20000); }
	return 0;
}

static int demo(void)
{
	config_load(&cfg);
	if (!x_ensure()) { fprintf(stderr, "bwv4d: no X display\n"); return 1; }
	for (int p = 0; p < P_COUNT; p++) { osd_show("DIAL PROFILE", PNAME[p], NULL, P_COUNT, p); demo_wait(900); }
	osd_show("SKILL", cfg.n_skills ? cfg.skills[0] : "/review", "1/10", 0, 0); demo_wait(900);
	osd_show("VS CODE", "razer-synapse-linux", "2/4", 0, 0); demo_wait(900);
	osd_show("M3", "Sent to Claude", NULL, 0, 0); demo_wait(900);
	osd_list_begin(8, demo_row, cfg.color[P_CLIP]);
	for (int s = 0; s < 8; s++) { osd_list_select(s); for (int f = 0; f < 20 && osd_tick(); f++) demo_wait(16); demo_wait(500); }
	for (int s = 6; s >= 2; s--) { osd_list_select(s); for (int f = 0; f < 20 && osd_tick(); f++) demo_wait(16); demo_wait(250); }
	demo_wait(1200);
	osd_hide();
	x_process();
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	if (argc > 1 && !strcmp(argv[1], "--restore")) return restore_only();
	if (argc > 1 && !strcmp(argv[1], "--demo")) return demo();
	if (argc > 2 && !strcmp(argv[1], "--press")) {                /* run a key's configured override, no keyboard needed */
		static const char *keys[8] = {"m1", "m2", "m3", "m4", "m5", "side1", "side2", "side3"};
		config_load(&cfg);
		if (!x_ensure()) return 1;
		for (int i = 0; i < 8; i++)
			if (!strcmp(argv[2], keys[i])) { bool ok = custom_key(i); for (int t = 0; t < 75; t++) { x_process(); usleep(20000); } return ok ? 0 : 3; }
		return 2;
	}
	if (argc > 3 && !strcmp(argv[1], "--clip-test")) return clip_test(atoi(argv[2]), atoi(argv[3]));
	if (argc > 1) { fprintf(stderr, "usage: bwv4d [--restore | --demo]\n"); return 2; }

	config_load(&cfg);
	state_load(&S.profile, &S.skill);
	S.profile = mod(S.profile, P_COUNT);
	S.prev_profile = P_VSCODE;
	if (out_init() < 0) { fprintf(stderr, "bwv4d: cannot use /dev/uinput: %s (udev rule installed?)\n", strerror(errno)); return 1; }

	sigset_t ss;
	sigemptyset(&ss);
	sigaddset(&ss, SIGTERM); sigaddset(&ss, SIGINT); sigaddset(&ss, SIGHUP);
	sigprocmask(SIG_BLOCK, &ss, NULL);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	sigfd = signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC);

	ep = epoll_create1(EPOLL_CLOEXEC);
	add_fd(sigfd, T_SIG);
	t_osd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); add_fd(t_osd, T_OSD);
	t_alt = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); add_fd(t_alt, T_ALT);
	t_skill = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); add_fd(t_skill, T_SKILL);
	t_rescan = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); add_fd(t_rescan, T_RESCAN);
	t_clip = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); add_fd(t_clip, T_CLIP);
	t_anim = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); add_fd(t_anim, T_ANIM);
	t_led = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC); add_fd(t_led, T_LED);

	nlfd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	struct sockaddr_nl sa = {.nl_family = AF_NETLINK, .nl_groups = 1};
	if (nlfd >= 0 && bind(nlfd, (struct sockaddr *)&sa, sizeof sa) == 0) add_fd(nlfd, T_NL);

	x_on_fatal(cleanup);                                  /* X gone (logout): restore before exiting */
	if (x_ensure()) ensure_x_in_loop();
	else fprintf(stderr, "bwv4d: no X display yet; popups and window actions wait for it\n");

	int rc = try_connect();
	if (rc == 0) osd_profile();
	else if (rc == -2) arm(t_rescan, 3000);
	else fprintf(stderr, "bwv4d: keyboard not present; waiting for it\n");

	for (;;) {
		struct epoll_event ev[8];
		int n = epoll_wait(ep, ev, 8, -1);
		if (n < 0) { if (errno == EINTR) continue; break; }
		for (int i = 0; i < n; i++) {
			uint32_t tag = ev[i].data.u32;
			bool hup = ev[i].events & (EPOLLHUP | EPOLLERR);
			switch (tag) {
			case T_KBD:   if (hup) disconnect(false); else if (kfd >= 0) read_kbd(); break;
			case T_MOUSE: if (hup) disconnect(false); else if (mfd >= 0) read_mouse(); break;
			case T_NL:    read_netlink(); break;
			case T_X:     break;                          /* handled by x_process below */
			case T_OSD:   drain(t_osd); osd_hide(); break;
			case T_ALT:   drain(t_alt); alt_release(); break;
			case T_SKILL: drain(t_skill); skill_commit(); break;
			case T_CLIP:  drain(t_clip); clip_commit(); break;
			case T_ANIM:  drain(t_anim); if (!osd_tick()) disarm(t_anim); break;
			case T_LED:   drain(t_led); if (led_on && cfg.led_mode == 1) led_host(false); break;
			case T_RESCAN:
				drain(t_rescan);
				if (!connected) {
					int r = try_connect();
					if (r == 0) osd_profile();
					else if (r == -2 && ++rescan_tries < 10) arm(t_rescan, 3000);
				}
				break;
			case T_SIG: {
				struct signalfd_siginfo si;
				if (read(sigfd, &si, sizeof si) > 0) { fprintf(stderr, "bwv4d: signal %u, exiting\n", si.ssi_signo); cleanup(); return 0; }
				break;
			}
			}
		}
		ensure_x_in_loop();
		x_process();
	}
	cleanup();
	return 1;
}

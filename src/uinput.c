/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* Virtual keyboard + wheel-only mouse through /dev/uinput. */
#define _GNU_SOURCE
#include "bwv4d.h"
#include <fcntl.h>
#include <linux/uinput.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int kfd = -1, mfd = -1;

static void emit(int fd, int type, int code, int val)
{
	struct input_event ev = {.type = type, .code = code, .value = val};
	if (write(fd, &ev, sizeof ev) < 0) { /* device gone; nothing sensible to do */ }
}

static int make(const char *name, int mouse)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return -1;
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	if (mouse) {
		int btn[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
		int rel[] = {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES};
		for (unsigned i = 0; i < 3; i++) ioctl(fd, UI_SET_KEYBIT, btn[i]);
		ioctl(fd, UI_SET_EVBIT, EV_REL);
		for (unsigned i = 0; i < 6; i++) ioctl(fd, UI_SET_RELBIT, rel[i]);
	} else {
		for (int k = 1; k < 249; k++) ioctl(fd, UI_SET_KEYBIT, k);
	}
	struct uinput_setup us = {0};
	us.id.bustype = BUS_VIRTUAL;
	strncpy(us.name, name, UINPUT_MAX_NAME_SIZE - 1);
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { close(fd); return -1; }
	return fd;
}

int out_init(void)
{
	kfd = make("bwv4d keyboard", 0);
	mfd = make("bwv4d wheel", 1);
	if (kfd < 0 || mfd < 0) return -1;
	usleep(150000);  /* let the X server pick the devices up before the first event */
	return 0;
}

void out_close(void)
{
	if (kfd >= 0) { ioctl(kfd, UI_DEV_DESTROY); close(kfd); kfd = -1; }
	if (mfd >= 0) { ioctl(mfd, UI_DEV_DESTROY); close(mfd); mfd = -1; }
}

void out_key(int code, int down)
{
	emit(kfd, EV_KEY, code, down);
	emit(kfd, EV_SYN, SYN_REPORT, 0);
}

void out_tap(int code)
{
	out_key(code, 1);
	out_key(code, 0);
	usleep(800);
}

void out_chord(int nmods, const int *mods, int key)
{
	for (int i = 0; i < nmods; i++) out_key(mods[i], 1);
	out_tap(key);
	for (int i = nmods - 1; i >= 0; i--) out_key(mods[i], 0);
	usleep(800);
}

/* US layout. Returns the number of characters actually typed (unknown ones are skipped). */
static int keyof(char c, int *shift)
{
	static const char *lo = "abcdefghijklmnopqrstuvwxyz";
	static const int row[] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
	                          KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
	static const int dig[] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
	static const struct { char c; int k, s; } sym[] = {
		{' ', KEY_SPACE, 0}, {'-', KEY_MINUS, 0}, {'_', KEY_MINUS, 1}, {'=', KEY_EQUAL, 0}, {'+', KEY_EQUAL, 1},
		{'/', KEY_SLASH, 0}, {'?', KEY_SLASH, 1}, {'.', KEY_DOT, 0}, {'>', KEY_DOT, 1}, {',', KEY_COMMA, 0},
		{'<', KEY_COMMA, 1}, {';', KEY_SEMICOLON, 0}, {':', KEY_SEMICOLON, 1}, {'\'', KEY_APOSTROPHE, 0},
		{'"', KEY_APOSTROPHE, 1}, {'[', KEY_LEFTBRACE, 0}, {'{', KEY_LEFTBRACE, 1}, {']', KEY_RIGHTBRACE, 0},
		{'}', KEY_RIGHTBRACE, 1}, {'\\', KEY_BACKSLASH, 0}, {'|', KEY_BACKSLASH, 1}, {'`', KEY_GRAVE, 0},
		{'~', KEY_GRAVE, 1}, {'!', KEY_1, 1}, {'@', KEY_2, 1}, {'#', KEY_3, 1}, {'$', KEY_4, 1}, {'%', KEY_5, 1},
		{'^', KEY_6, 1}, {'&', KEY_7, 1}, {'*', KEY_8, 1}, {'(', KEY_9, 1}, {')', KEY_0, 1}};
	*shift = 0;
	if (c >= 'a' && c <= 'z') return row[strchr(lo, c) - lo];
	if (c >= 'A' && c <= 'Z') { *shift = 1; return row[c - 'A']; }
	if (c >= '0' && c <= '9') return dig[c - '0'];
	for (unsigned i = 0; i < sizeof sym / sizeof *sym; i++)
		if (sym[i].c == c) { *shift = sym[i].s; return sym[i].k; }
	return 0;
}

int out_type(const char *s)
{
	int n = 0;
	for (; *s; s++) {
		int shift, k = keyof(*s, &shift);
		if (!k) continue;
		if (shift) out_key(KEY_LEFTSHIFT, 1);
		out_tap(k);
		if (shift) out_key(KEY_LEFTSHIFT, 0);
		n++;
	}
	return n;
}

void out_backspace(int n)
{
	while (n-- > 0) out_tap(KEY_BACKSPACE);
}

void out_scroll(int v)
{
	emit(mfd, EV_REL, REL_WHEEL, v);
	emit(mfd, EV_REL, REL_WHEEL_HI_RES, v * 120);
	emit(mfd, EV_SYN, SYN_REPORT, 0);
}

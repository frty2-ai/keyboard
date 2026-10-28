/* SPDX-FileCopyrightText: 2026 frty2.ai and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* ~/.config/bwv4d/config (key = value, one "skill =" line per skill) and ~/.local/state/bwv4d/state. */
#define _GNU_SOURCE
#include "bwv4d.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void path(char *out, size_t n, const char *xdg, const char *fallback, const char *file)
{
	const char *base = getenv(xdg), *home = getenv("HOME");
	if (base && *base) snprintf(out, n, "%s/bwv4d/%s", base, file);
	else snprintf(out, n, "%s/%s/bwv4d/%s", home ? home : ".", fallback, file);
}

static long hex(const char *v, long dflt)
{
	if (*v == '#') v++;
	char *end;
	long x = strtol(v, &end, 16);
	return (end != v && !*end && strlen(v) == 6) ? x : dflt;
}

static char *trim(char *s)
{
	while (isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	if (e - s >= 2 && *s == '"' && e[-1] == '"') { e[-1] = 0; s++; }  /* "quoted" keeps spaces */
	return s;
}

void config_load(struct config *c)
{
	memset(c, 0, sizeof *c);
	c->osd_ms = 1200;
	c->skill_commit_ms = 2500;
	c->alt_hold_ms = 900;
	c->long_press_ms = 600;
	strcpy(c->skill_suffix, " ");
	strcpy(c->skill_apps[0], "claude");
	strcpy(c->skill_apps[1], "code");
	c->n_skill_apps = 2;
	c->clip_items = 30;
	c->clip_mb = 32;
	c->clip_item_mb = 16;
	c->clip_commit_ms = 1000;
	c->led_mode = 2;
	c->led_base = 0x303030;
	c->led_dial_lamp = 0;
	const uint32_t col[5] = {0x44d62c, 0xffb000, 0x2f9bff, 0xffffff, 0xb46bff};
	memcpy(c->color, col, sizeof col);

	char p[512];
	path(p, sizeof p, "XDG_CONFIG_HOME", ".config", "config");
	FILE *f = fopen(p, "r");
	if (!f) { fprintf(stderr, "bwv4d: no config at %s, using defaults (no skills)\n", p); return; }
	char line[256];
	bool apps_set = false;
	while (fgets(line, sizeof line, f)) {
		char *s = trim(line);
		if (!*s || *s == '#') continue;
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		char *v0 = eq + 1;
		while (isspace((unsigned char)*v0)) v0++;
		if (*v0 != '"') { char *h = strchr(v0, '#'); if (h) *h = 0; }   /* trailing comment */
		char *k = trim(s), *v = trim(v0);
		if (!strcmp(k, "skill") && c->n_skills < MAX_SKILLS) snprintf(c->skills[c->n_skills++], 64, "%s", v);
		else if (!strcmp(k, "osd_ms")) c->osd_ms = atoi(v);
		else if (!strcmp(k, "skill_commit_ms")) c->skill_commit_ms = atoi(v);
		else if (!strcmp(k, "alt_hold_ms")) c->alt_hold_ms = atoi(v);
		else if (!strcmp(k, "long_press_ms")) c->long_press_ms = atoi(v);
		else if (!strcmp(k, "dial_invert")) c->dial_invert = !strcmp(v, "true");
		else if (!strcmp(k, "roller_invert")) c->roller_invert = !strcmp(v, "true");
		else if (!strcmp(k, "clip_items")) c->clip_items = atoi(v);
		else if (!strcmp(k, "clip_mb")) c->clip_mb = atoi(v);
		else if (!strcmp(k, "clip_item_mb")) c->clip_item_mb = atoi(v);
		else if (!strcmp(k, "clip_commit_ms")) c->clip_commit_ms = atoi(v);
		else if (!strcmp(k, "led_mode")) c->led_mode = !strcmp(v, "off") ? 0 : !strcmp(v, "flash") ? 1 : 2;
		else if (!strcmp(k, "led_base")) c->led_base = !strcmp(v, "keep") ? -1 : hex(v, c->led_base);
		else if (!strcmp(k, "led_dial_lamp")) c->led_dial_lamp = atoi(v);
		else if (!strncmp(k, "color_", 6)) {
			static const char *names[5] = {"skills", "clipboard", "vscode", "apps", "tabs"};
			for (int i = 0; i < 5; i++) if (!strcmp(k + 6, names[i])) c->color[i] = (uint32_t)hex(v, c->color[i]);
		}
		else if (!strncmp(k, "run_", 4) || !strncmp(k, "focus_", 6)) {
			static const char *keys[8] = {"m1", "m2", "m3", "m4", "m5", "side1", "side2", "side3"};
			const char *name = k + (k[0] == 'r' ? 4 : 6);
			int idx = -1;
			for (int i = 0; i < 8; i++) if (!strcmp(name, keys[i])) idx = i;
			if (idx < 0) fprintf(stderr, "bwv4d: unknown key in '%s'\n", k);
			else if (k[0] == 'r') snprintf(c->run[idx], sizeof c->run[idx], "%s", v);
			else snprintf(c->focus[idx], sizeof c->focus[idx], "%s", v);
		}
		else if (!strcmp(k, "skill_suffix")) snprintf(c->skill_suffix, sizeof c->skill_suffix, "%s", v);
		else if (!strcmp(k, "skill_apps")) {
			if (!apps_set) { c->n_skill_apps = 0; apps_set = true; }
			for (char *t = strtok(v, ","); t && c->n_skill_apps < MAX_APPS; t = strtok(NULL, ","))
				snprintf(c->skill_apps[c->n_skill_apps++], 32, "%s", trim(t));
		} else fprintf(stderr, "bwv4d: unknown config key '%s'\n", k);
	}
	fclose(f);
	fprintf(stderr, "bwv4d: loaded %s (%d skills)\n", p, c->n_skills);
}

void state_load(int *profile, int *skill)
{
	char p[512];
	path(p, sizeof p, "XDG_STATE_HOME", ".local/state", "state");
	FILE *f = fopen(p, "r");
	if (!f) return;
	if (fscanf(f, "profile %d skill %d", profile, skill) != 2) { *profile = 0; *skill = 0; }
	fclose(f);
}

void state_save(int profile, int skill)
{
	char p[512], dir[512];
	path(p, sizeof p, "XDG_STATE_HOME", ".local/state", "state");
	snprintf(dir, sizeof dir, "%s", p);
	char *slash = strrchr(dir, '/');
	if (slash) { *slash = 0; char *up = strrchr(dir, '/'); if (up) { *up = 0; mkdir(dir, 0755); *up = '/'; } mkdir(dir, 0755); }
	FILE *f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "profile %d skill %d\n", profile, skill);
	fclose(f);
}

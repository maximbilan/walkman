/*
 * walkman.c, the readable version: a one-button platformer for the terminal.
 *
 *   cc walkman.readable.c -o walkman -lm && ./walkman
 *
 * You run on Walkman batteries across a skyline of cassettes. SPACE jumps,
 * Q quits. The batteries drain, so grab new ones (the AA cells floating over
 * the tapes). When they die the tape drags to a stop: the music sags and the
 * whole world slows down with it.
 *
 * Everything is plain characters in 24-bit color; make the terminal at least
 * 64x24. The music is synthesized at start-up and played with afplay (macOS)
 * or aplay (Linux).
 *
 * walkman.c is this file without the comments and the DEV parts, with short
 * names, packed into the shape of a Walkman (python3 minify.py && python3 shape.py).
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

enum {
	RATE = 44100,      // audio samples per second
	DH = 96,           // the world is this many pixels tall: 24 rows of 4
	HORIZON = 60,      // where the sky meets the floor grid
	TW = 36, TH = 24,  // a cassette is 36x24 pixels: 18x6 characters
	NT = 256,          // how many cassettes and batteries we keep (the newest)
	MAXW = 240, MAXH = 80,
	WHITE = 0xffffff
};

/* ------------------------------------------------------------------ colors */

typedef unsigned Color;  // 0xRRGGBB

static Color mix(Color a, Color b, float t) {
	Color c = 0;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	for (int s = 0; s < 24; s += 8) c |= (Color)((a >> s & 255) * (1 - t) + (b >> s & 255) * t) << s;
	return c;
}

static Color shade(Color c, float k) { return mix(0, c, k); }

static unsigned hash(unsigned x) {
	x = (x ^ x >> 16) * 0x7feb352d;
	x = (x ^ x >> 15) * 0x846ca68b;
	return x ^ x >> 16;
}

/* ------------------------------------------------------------------ screen */

/* A grid of characters, each with a color. The world is measured in pixels,
 * and a character is 2 of them wide and 4 tall (about the shape of a cell). */
static int COLS, ROWS, W, H, OY, camc;  // in characters; in pixels; world y = 0 at pixel OY; camera column
static int scr[MAXW * MAXH];
static Color ink[MAXW * MAXH];

static void put(int c, int r, int ch, Color col) {
	if (c >= 0 && c < COLS && r >= 0 && r < ROWS) scr[r * COLS + c] = ch, ink[r * COLS + c] = col;
}

static void text(int c, int r, const char *s, Color col) {
	while (*s) put(c++, r, *s++, col);
}

/* n rows of w characters, with a key for each: '.' leaves the cell alone,
 * other keys pick a color from pal. */
static void art(int c, int r, const char *s, const char *key, Color *pal, int w, int n) {
	for (int i = 0; i < w * n; i++)
		if (key[i] - '.') put(c + i % w, r + i / w, s[i], pal[(int)key[i]]);
}

static int colx(float x) { return floorf(x / 2) - camc; }
static int rowy(float y) { return floorf((y + OY) / 4); }

/* ----------------------------------------------------------------- the world */

typedef struct {
	float x, y, vy;            // feet; y grows downward
	int ground, coyote, dead;  // dead: 1 fell, 2 ran into a cassette, 3 hit by a disc
	int got, landed;           // the battery touched in this step (or -1); just landed
} Body;

static float tx[NT], ty[NT], bx[NT], by[NT];  // cassettes (top-left corners) and batteries (centers)
static int look[NT], taken[NT], nt, nb;
static float dm[6], dk[6], dg[6];  // the boss's discs: where each meets you, its speed (yours is 1), the floor it rolls on
static float dt = 1;  // how fast the world runs: 1, or less while the tape drags to a stop

/* Where feet at x would stand on cassette i, or far away. */
static float top(int i, float x) { return x + 3 > tx[i] && x - 3 < tx[i] + TW ? ty[i] : 1e9; }

static void step(Body *b, int jump) {
	float px = b->x, py = b->y, best = 1e9, t;
	int i;
	b->got = -1, b->landed = 0;
	if (b->dead) {
		b->vy += .14 * dt, b->y += b->vy * dt, b->x += .5 * dt;
		return;
	}
	if (jump && (b->ground || b->coyote > 0)) b->vy = -2.8, b->ground = b->coyote = 0;
	b->x += 1.25 * dt;
	if (b->ground) {  // follow the tops, or run off the edge
		for (i = 0; i < NT; i++)
			if (t = top(i, b->x), fabsf(t - b->y) < 3.5 && t < best) best = t;
		if (best < 1e9) b->y = best;
		else b->ground = 0, b->coyote = 6, b->vy = 0;
	}
	if (!b->ground) {
		b->vy += .14 * dt, b->y += b->vy * dt;
		if (b->coyote > 0) b->coyote--;
		if (b->vy > 0) {  // falling: did we pass a top?
			for (i = 0; i < NT; i++)
				if (t = top(i, b->x), t >= py - .01 && t <= b->y && t < best) best = t;
			if (best < 1e9) b->y = best, b->vy = 0, b->ground = b->landed = 1, b->coyote = 0;
		}
	}
	for (i = 0; i < NT; i++) {
		// The side of a cassette stops you (you can jump up through one).
		if (px + 4 <= tx[i] && b->x + 4 > tx[i] && b->y > ty[i] + 2 && b->y - 14 < ty[i] + TH) b->dead = 2;
		if (!taken[i] && fabsf(b->x - bx[i]) < 7 && b->y - 16 < by[i] + 8 && b->y > by[i] - 8) b->got = i;
	}
	// A disc rolling at you meets you at dm, coming 1 + dk times as fast as you
	// run: it's next to you while |x - dm| (1 + dk) < 6. Jump it.
	for (i = 0; i < 6; i++)
		if (fabsf(b->x - dm[i]) * (1 + dk[i]) < 6 && b->y > dg[i] - 12) b->dead = 3;
	if (b->dead > 1) b->vy = -1.2;
	if (b->y > DH + 24) b->dead = 1;
}

/* Endless levels, made as you go: rows of cassettes, a step up or down from
 * the last one, with gaps you can always jump, and a battery every 4 or 5
 * seconds of running (they last 7). Every 35 seconds or so, a boss: a long
 * flat row, and a compact disc that throws six smaller ones at you. */
static unsigned seed, gseed;
static float gx, gy, nextcell, nextboss, arena;  // where the last row ends, its top; the next battery, boss; the boss's row

static float grand(void) {
	gseed = gseed * 1664525 + 1013904223;
	return (gseed >> 8) / 16777216.;
}

static void cassette(float x, float y) {
	int i = nt++ % NT;
	tx[i] = x, ty[i] = y, look[i] = nt;
}

static void cell(float x, float y) {
	int i = nb++ % NT;
	bx[i] = x, by[i] = y, taken[i] = 0, nextcell = x + 230 + grand() * 70;
}

static void generate(float until) {
	while (gx < until) {
		int boss = gx > nextboss, k, i, n;
		float ny = boss ? gy : 4 * roundf(fminf(fmaxf(gy + (grand() - .55) * 36, 44), 76) / 4), rise = gy - ny,
		      gap = boss ? 20 : 12 + grand() * (rise > 0 ? 28 - rise : 32);
		n = boss ? 16 : gx + gap + 3 * TW > nextcell ? 3 + grand() * 2 : 2 + grand() * 3;  // room for a battery when one's due
		for (k = 0; k < n; k++) {
			cassette(gx + gap + k * TW, ny);
			cassette(gx + gap + k * TW, ny + TH);  // and one underneath
		}
		i = boss ? 4 : n;  // batteries, as near nextcell as can be, a cassette from the ends (on a boss's row, only before the fight)
		while (n > 2 && nextcell < gx + gap + i * TW) cell(fminf(fmaxf(nextcell, gx + gap + TW), gx + gap + i * TW - TW), ny - 26 - grand() * 4);
		if (boss)  // its discs, and a battery over every other one: jump the disc, grab the battery
			for (arena = gx + gap, nextboss += 2600, k = 0; k < 6; k++) {
				dm[k] = arena + 150 + k * 70 + grand() * 25, dk[k] = .2 + grand() * .5, dg[k] = ny;
				if (k % 2 == 0) cell(dm[k], ny - 29);
			}
		gx += gap + n * TW, gy = ny;
	}
}

/* ------------------------------------------------------------------- game */

static Body P;  // you
static float charge, tape, cam;  // batteries 0..1; tape speed; camera
static double wt, song;  // world time in frames; song position in seconds of tape
static int frame, dying, stopped, why, jumpq, best;

static struct {
	float x, y, vx, vy, life, max;
	Color c;
	int ch;
} q[64];  // sparks and dust
static int nq;
static unsigned prng = 1;

static float prand(void) {
	prng = prng * 1664525 + 1013904223;
	return (prng >> 8) / 16777216.;
}

static void burst(float x, float y, int n, Color c, float sp, const char *chars) {
	while (n-- && nq < 64) {
		float a = prand() * 6.283, s = sp * (.4 + prand() * .6);
		q[nq].x = x, q[nq].y = y, q[nq].vx = cosf(a) * s, q[nq].vy = sinf(a) * s - .25;
		q[nq].life = 0, q[nq].max = 14 + prand() * 14, q[nq].c = c, q[nq++].ch = chars[(int)(prand() * 3)];
	}
}

static void sound(const char *name, int stop);

/* The boss takes the sun's place: it rises from behind the tapes as you near
 * its row, and when its last disc has gone by it falls. */
static float bossx(void) { return 2 * floorf(cam / 2) + 2 * roundf(W * .39); }  // a fixed column on screen, like the sun
static float bossy(void) { return HORIZON - 19 + fmaxf(0, arena - 50 - P.x) * .5 + .07 * powf(fmaxf(0, P.x - dm[5] - 8) / 1.25, 2); }

static void restart(void) {
	int k;
	for (k = 0; k < NT; k++) tx[k] = bx[k] = dm[k % 6] = -1e9;
	nt = nb = nq = 0, gseed = seed = seed * 69069 + 1;
	P = (Body){41, 68, 0, 1, 0, 0, -1, 0};
	for (k = 0; k < 6; k++) cassette(k * TW, 68), cassette(k * TW, 92);
	gx = 6 * TW, gy = 68, nextcell = 230, nextboss = 700, arena = -1e9;
	charge = tape = dt = 1, wt = song = 0, frame = 1, dying = stopped = why = jumpq = 0;
}

static void update(int press) {
	if (press) jumpq = 8;  // a jump pressed a little early still counts
	int jump = jumpq > 0 && (P.ground || P.coyote > 0) && !dying, k;
	float was = P.x;
	jumpq = jump ? 0 : jumpq - 1;
	step(&P, jump);
	if (was < dm[5] + 8 && P.x >= dm[5] + 8 && !P.dead) burst(bossx(), bossy(), 30, 0xff78c8, 2.2, "*+."), sound("g", 0);
	if (P.landed) burst(P.x, P.y - 2, 5, 0xffd2eb, .9, ".'`");
	if (P.got >= 0 && !dying) {
		k = P.got, taken[k] = 1, charge = 1;
		burst(bx[k], by[k], 16, 0xbeff96, 1.6, "*+.");
		sound("g", 0);
	}
	if (P.dead && !dying) dying = frame, why = P.dead + 1;
	if (!dying && (charge -= 1 / 420.) <= 0) charge = 0, dying = frame, why = 1;
	if (dying) {  // the tape drags to a stop, slower and slower, wobbling
		float len = why == 1 ? 3 : 1.2, t = (frame - dying) / 60., d = fminf(t / len, 1);
		tape = dt = powf(1 - d, 1.4) * (1 + .12 * d * sinf(11 * t));
		if (d >= 1 && !stopped) stopped = frame, best = fmaxf(best, P.x / 6);
	}
	wt += dt, song += tape / 60;
	for (k = 0; k < nq; k++) {
		q[k].x += q[k].vx * dt, q[k].y += q[k].vy * dt, q[k].vy += .03 * dt, q[k].life += dt;
		if (q[k].life >= q[k].max) q[k--] = q[--nq];
	}
	cam = P.x - 2 * roundf(W * .13);
	generate(cam + W + 100);
}

/* ------------------------------------------------------------------ drawing */

static float tri(float u) { return 1 - fabsf(2 * (u - floorf(u)) - 1); }

static void background(int nosun) {
	float beat = expf(-fmod(song, 60 / 70.) * 7), sunx = W * .78, suny = HORIZON - 19 + OY, ph = cam * .035;
	int hz = (HORIZON + OY) / 4, c, r, k, prev, ridge[MAXW + 2];
	for (k = 0; k < COLS * ROWS; k++) scr[k] = ' ', ink[k] = 0;

	// Stars.
	for (r = 1; r < hz - 2; r++)
		for (c = 0; c < COLS; c++) {
			unsigned h = hash((c + (int)(cam * .0125)) * 7919 + (r - OY / 4 + 99) * 104729);
			if (h % 29 == 0)
				put(c, r, "..'.`+*."[h >> 12 & 7],
				    shade(h >> 16 & 1 ? 0xfff0ff : 0xa0dcff,
				          (.45 + .55 * (.5 + .5 * sinf(wt * .05 + (h >> 8 & 63)))) * (1.3 - (r * 4 - OY) / 60.)));
		}

	// The sun: lines of '=', cut by stripes toward the bottom.
	for (r = (suny - 24) / 4; r < hz && !nosun; r++) {
		float y = r * 4 + 2, dy = y - suny, hw, v;
		if (fabsf(dy) >= 24) continue;
		hw = sqrtf(576 - dy * dy), v = (y - suny + 24) / 48, k = r + (int)(wt / 24);
		if (v > .5 && (k % 3 == 0 || (v > .8 && k % 3 == 1))) continue;
		for (c = ceilf((sunx - hw) / 2 - .5); c <= floorf((sunx + hw) / 2 - .5); c++)
			put(c, r, v < .25 ? '-' : '=', v < .5 ? mix(0xffec78, 0xff9646, v * 2) : mix(0xff9646, 0xff3c96, v * 2 - 1));
	}

	// Two ranges of mountains sliding by slower than you: a ridge of / _ \
	// that rises or falls at most a row per column, with a gap for the sun.
	for (k = 0; k < 2; k++) {
		float par = k ? .22 : .1;
		for (c = 0, prev = hz; c <= COLS; c++) {
			float x = 2 * c, valley = fminf(fmaxf((fabsf(x - sunx) - 10) / 30, 0), 1), wx = x + cam * par,
			      h = k ? 10 * tri(wx / 47) + 6 * tri(wx / 29 + .37) - 7
			            : 9 + 5 * sinf(wx * .043) + 3.5 * sinf(wx * .11 + 1.3) + 2 * sinf(wx * .27);
			int want = roundf((HORIZON + OY - fmaxf(h, 0) * valley * 1.3) / 4);
			if (want > hz) want = hz;
			if (c) want = want < prev - 1 ? prev - 1 : want > prev + 1 ? prev + 1 : want;
			ridge[c] = prev = want;
		}
		for (c = 0; c < COLS; c++) {
			int a = ridge[c], b = ridge[c + 1], t;
			Color line = k ? 0xc85ad2 : 0x8246be;
			if (a >= hz && b >= hz) continue;
			if (b < a) put(c, b, '/', line), t = b + 1;
			else if (b > a) put(c, a, '\\', line), t = a + 1;
			else put(c, a - 1, '_', line), t = a;
			for (r = t; r < hz; r++)
				put(c, r, hash((c + (int)(cam * par / 2)) * 31 + r * 17) % 9 ? ' ' : '.', k ? 0x501e6e : 0x341a5c);
		}
	}

	// The floor: a neon grid in perspective that flashes on the kick.
	Color neon = shade(0xff3cc8, .55 + .45 * beat);
	for (c = 0; c < COLS; c++) put(c, hz, '-', shade(mix(neon, 0xffbef0, .4), .8));
	for (r = hz + 1; r < ROWS; r++) {
		int d = r - hz;
		float dd = d + 1, fade = fminf(1, .45 + d * .08);
		if (d == 2 || d == 5 || d == 9)
			for (c = 0; c < COLS; c++) put(c, r, '-', shade(neon, .7 * fade));
		for (k = floorf(ph - W / (4.8 * dd)) - 1; k <= ceilf(ph + W / (4.8 * dd)) + 1; k++) {
			float x = W * .5 + (k - ph) * 2.4 * dd, slope = (k - ph) * 1.2;
			put(floorf(x / 2), r, fabsf(slope) < .4 ? '|' : slope > 0 ? '\\' : '/', shade(neon, fade));
		}
	}
}

/* A cassette: 18x6 characters, with a lid of '_' on the row above. */
static const char *TAPE = " ________________ "
                          "|.--------------.|"
                          "||              ||"
                          "||  ( )====( )  ||"
                          "|'--------------'|"
                          "|    ________    |"
                          "|___/________\\___|";
static const char *TAPE_KEY = ".ssssssssssssssss."
                              "sLLLLLLLLLLLLLLLLs"
                              "sL              Ls"
                              "sL  wwwbbbbwww  Ls"
                              "sLLLLLLLLLLLLLLLLs"
                              "s    dddddddd    s"
                              "sssssddddddddsssss";
static const Color SHELL[] = {0xff5e62, 0xffce40, 0x40cee6, 0xb076ff, 0x60e296, 0xff9640, 0xececf6};
static const Color ACCENT[] = {0xff5a82, 0x5aa0ff, 0xff9628, 0x46c878};
static const char *NAMES[] = {"MIX TAPE",   "FOR YOU",   "SUMMER '86", "ROAD TRIP", "DO NOT ERASE",
                              "LOVE SONGS", "PARTY MIX", "C-90",       "SLOW JAMS"};

static void cassette_art(int c, int r, int k, int lid) {
	Color pal[128] = {0};
	const char *name = NAMES[(k * 7 + k / 9) % 9];
	int s = (int)(wt / 5) & 3, n = 0;
	pal['s'] = SHELL[k % 7], pal['d'] = shade(SHELL[k % 7], .55), pal['L'] = 0xf6ecd4, pal['w'] = WHITE;
	pal['b'] = 0xbe7850;
	if (lid) {
		art(c, r - 1, TAPE, TAPE_KEY, pal, 18, 1);
		return;
	}
	art(c, r, TAPE + 18, TAPE_KEY + 18, pal, 18, 6);
	while (name[n]) n++;
	text(c + 3 + (12 - n) / 2, r + 1, name, ACCENT[k / 7 % 4]);
	put(c + 5, r + 2, "|/-\\"[s], WHITE), put(c + 12, r + 2, "|/-\\"[(s + 1) & 3], WHITE);  // the reels turn
}

/* A battery: an AA cell with two sparks going around it. */
static void battery(int c, int r) {
	Color pal[128] = {0};
	int x, y, k;
	pal['g'] = shade(0x78ff96, .75 + .25 * sinf(wt * .15)), pal['w'] = WHITE;
	for (y = r - 2; y <= r + 1; y++)
		for (x = c - 1; x <= c + 1; x++) put(x, y, ' ', 0);
	put(c, r - 2, '_', 0xd2d6e2);
	art(c - 1, r - 1, "[+][#][-]", "gwgggggwg", pal, 3, 3);
	for (k = 0; k < 2; k++) {
		float an = wt * .09 + k * 3.1416;
		put(c + roundf(cosf(an) * 3), r + roundf(sinf(an) * 1.6), k ? '*' : '+', 0xdcffc8);
	}
}

/* You: a cap, headphones, shades, a Walkman on your belt. Six by four. */
static void player(void) {
	static const char *LEGS = " /  \\ .J..J.  /|  ..JJ..  |\\  ..JJ..  /\\  ..JJ..";
	Color pal[128] = {0};
	int c = colx(P.x) - 3, r = roundf((P.y + OY) / 4) - 4, f = (int)(wt / 6) & 3, arms, legs, k;
	pal['R'] = 0xff4650, pal['Y'] = 0xffd63c, pal['S'] = 0xffcca4, pal['g'] = 0x6edcff, pal['B'] = 0x50aaff;
	pal['O'] = 0xff9628, pal['J'] = 0x8c96ff;
	if (!P.ground) arms = 1, legs = 0;
	else if (dt < .05) arms = 0, legs = 3;
	else arms = f & 1, legs = f == 1 ? 1 : f == 3 ? 2 : 0;
	for (k = 1; k < 18; k++) put(c + k % 6, r + k / 6, ' ', 0);
	art(c, r, " .--._[(oo)]", ".RRRRRYSggSY", pal, 6, 2);
	art(c, r + 2, arms ? " \\#O/ " : " /#O\\ ", ".SBOS.", pal, 6, 1);
	art(c, r + 3, LEGS + 12 * legs, LEGS + 12 * legs + 6, pal, 6, 1);
}

/* A rainbow, like light on the back of a CD. */
static Color rainbow(float a) {
	Color c = 0;
	for (int s = 0; s < 3; s++) c = c << 8 | (int)(150 + 105 * sinf(a + s * 2.1));
	return c;
}

/* The boss: a compact disc with a temper, 23x11 characters. */
static void boss(int c0, int r0, int hp) {
	int c, r;
	for (r = -5; r <= 5; r++)
		for (c = -11; c <= 11; c++) {
			float u = c / 11.5, v = r / 5.5, d = sqrtf(u * u + v * v), a = atan2f(v, u);
			if (d > 1) continue;
			if (d > .86) put(c0 + c, r0 + r, "|/-\\"[(int)lroundf(a / .785) & 3], 0xd8dce8);  // the rim follows the curve
			else if (d < .32) put(c0 + c, r0 + r, d < .2 ? ' ' : 'o', 0xc8ccd8);  // the hole and the hub
			else put(c0 + c, r0 + r, (c + r) & 1 ? '~' : '=', rainbow(a * 2 + (hp ? wt * .15 : 0) + d * 2));
		}
	for (c = -1; c < 2; c += 2) {  // angry eyes; x x when it's beaten
		if (hp) put(c0 + 6 * c, r0 - 3, c < 0 ? '\\' : '/', 0xff4040);
		put(c0 + 5 * c, r0 - 2, hp ? 'O' : 'x', WHITE);
	}
}

static void draw(void) {
	char line[64];
	int i, c, r, s = frame - stopped, hp = 6, up = P.x > arena - W && P.x < dm[5] + 200;
	for (i = 0; i < 6; i++) hp -= dm[i] + 8 < P.x;
	camc = floorf(cam / 2);
	background(up && hp && P.x > arena - 50);  // it rises in front of the sun, and then it is the sun
	if (up) boss(colx(bossx()), rowy(bossy()), hp);
	for (i = 0; i < 2 * NT; i++)  // all the lids first, then the cassettes over them
		if (tx[i % NT] > cam - 40 && tx[i % NT] < cam + W + 4)
			cassette_art(colx(tx[i % NT]), rowy(ty[i % NT]), look[i % NT], i < NT);
	for (i = 0; i < NT; i++)
		if (!taken[i] && bx[i] > cam - 10 && bx[i] < cam + W + 10)
			battery(colx(bx[i]), rowy(by[i] + 2.5 * sinf(wt * .08 + bx[i])));
	for (i = 0; i < nq; i++) {
		c = colx(q[i].x), r = rowy(q[i].y);
		if (c >= 0 && c < COLS && r >= 0 && r < ROWS && scr[r * COLS + c] == ' ')
			put(c, r, q[i].ch, shade(q[i].c, 1 - q[i].life / q[i].max));
	}
	// The boss's discs: each comes out of it, drops to the floor and rolls at you.
	if (up) {
		float x = bossx(), y = bossy();
		for (i = 0; i < 6; i++) {
			float d = dm[i] + (dm[i] - P.x) * dk[i];
			c = colx(d), r = rowy(y + (dg[i] - 6 - y) * fminf((x - d) / 30, 1));
			if (d < x)
				art(c - 2, r - 1, " .-. (=o=) '-' ", " sss srwrs sss ", (Color[128]){['s'] = 0xd8dce8, ['w'] = WHITE, ['r'] = rainbow(wt * .2 + i)}, 5, 3);
		}
		if (hp) text(COLS / 2 - 6, 0, "CD [      ]", 0xff78c8);
		for (i = 0; i < hp; i++) put(COLS / 2 - 2 + i, 0, '#', 0xff78c8);
		text(colx(x) - 11 + 7 * !hp, rowy(y) - 8, hp == 6 ? "BOSS: THE COMPACT DISC" : hp ? "" : "SKIPPED!", 0xff78c8);
	}
	player();

	// The tape counter and the batteries.
	sprintf(line, "[%04d]", (int)(P.x / 6) % 10000);
	text(1, 0, line, 0xe6e6f0);
	i = charge < .25 && !stopped && frame / 10 % 2, c = COLS - 8;
	text(c, 0, "[    ]=", i ? 0xff5050 : 0xe6e6f0);
	for (r = 0; r < ceilf(charge * 4 - .001); r++) put(c + 1 + r, 0, '#', charge > .5 ? 0x64f082 : charge > .25 ? 0xffd23c : 0xff5050);
	if (frame < 200) text(COLS / 2 - 11, OY / 4 + 3, "SPACE JUMPS. GRAB THE AAs", WHITE);

	if (!stopped) return;
	for (i = 0; i < COLS * ROWS; i++) ink[i] = shade(ink[i], 1 - fminf(s / 20., 1) * .55);
	if (s < 8) return;
	c = COLS / 2 - 12, r = OY / 4 + 7;
	text(c, r, ".----------------------.", 0xffb46e);
	text(c, r + 1, why == 1 ? "|   INSERT BATTERIES   |" : why == 4 ? "|      SCRATCHED       |" : "|      TAPE EATEN      |", 0xfff082);
	sprintf(line, "| TAPE %04d  BEST %04d |", (int)(P.x / 6) % 10000, best % 10000);
	text(c, r + 2, line, WHITE);
	text(c, r + 3, "'----------------------'", 0xffb46e);
	if (frame % 50 < 34) text(c + 4, r + 5, "SPACE TO REWIND", WHITE);
}

static char *out;  // a frame of escapes

static int frame_ansi(void) {
	char *o = out;
	Color cur = -1;
	o += sprintf(o, "\33[H");
	for (int i = 0; i < COLS * ROWS; i++) {
		if (scr[i] - ' ' && ink[i] - cur) cur = ink[i], o += sprintf(o, "\33[38;2;%d;%d;%dm", cur >> 16, cur >> 8 & 255, cur & 255);
		*o++ = scr[i];
		if (i % COLS == COLS - 1 && i < COLS * ROWS - 1) *o++ = '\n';
	}
	return o - out;
}

/* ------------------------------------------------------------------- sound */

/* An original tune: eight bars at 140 BPM, Am F C G | Am F G E. The melody is
 * one character per sixteenth note, its MIDI number ('E' is 69, an A); '.'
 * holds, ' ' rests. The bass roots are MIDI numbers too ('-' is 45, an A). */
static const double STEP = 3 / 28., BAR = 12 / 7., SONG = 96 / 7.;  // a sixteenth, a bar and the song at 140 BPM
static const char *MELODY = "E..HL.Q.O..LH.J.H..EH.M.L...H.E.C..HL.O.Q..OL.H.J..GJ.O.S.....  "
                            "E..HL.Q.T..SQ.L.M..LH.E.H...M.Q.S..QO.J.G.J.O.S.P...S.X.....V.S.";
static const char *ROOTS = "-)0+-)+(";
static short *pcm;  // a minute of sound

static float freq(int m) { return 440 * powf(2, (m - 69) / 12.); }
static float noise(double p) { return (hash((int)(p * RATE)) & 65535) / 32768. - 1; }

static float lead(double p) {
	double l = fmod(p, SONG), t;
	int a = l / STEP, e = a + 1;
	if (p < 0) return 0;
	while (MELODY[a] == '.') a--;
	while (MELODY[e] == '.') e++;
	if (MELODY[a] == ' ') return 0;
	t = l - a * STEP;
	return fminf(t * 300, 1) * fmaxf(fminf(((e - a) * STEP - t) * 60, 1), 0) * (.7 + .3 * expf(-6 * t)) *
	       ((fmod(freq(MELODY[a]) * t, 1) < .25) * 2 - .5);
}

static float music(double p) {
	double l = fmod(p, SONG), in = fmod(l, BAR), t = fmod(in, STEP), tb = fmod(in, 2 * STEP), tk, ts = -1;
	int bar = l / BAR, st = in / STEP, j = st & 3;
	float f = freq(ROOTS[bar] + 12 * (st / 2 & 1)), ph = fmod(f * tb, 1), n = noise(p), v;
	// The bass: octaves in eighth notes, a triangle with a bit of square.
	v = .28 * fminf(tb * 400, 1) * fmaxf(fminf((2 * STEP * .8 - tb) * 80, 1), 0) * (4 * fabs(ph - .5) - 1 + .3 * (ph < .5 ? 1 : -1));
	// An arpeggio two octaves up, minor on the A minor bars.
	v += .045 * ((fmod(freq(ROOTS[bar] + 24 + "\0\4\7\14"[j] - (j == 1 && !(bar & 3))) * t, 1) < .125) * 2 - .25) * expf(-14 * t);
	// The melody, and its echo.
	v += .17 * lead(p) + .06 * lead(p - 3 * STEP);
	// Kick, snare (with a roll into the top), hats.
	tk = in - ((bar & 1) && st >= 10 ? 10 : st >= 8 ? 8 : 0) * STEP;
	v += .6 * sin(6.283 * (48 * tk + 2.6 * (1 - exp(-26 * tk)))) * exp(-7.5 * tk);
	if (bar == 7 && st >= 12) ts = t;
	else if (st >= 12) ts = in - 12 * STEP;
	else if (st >= 4) ts = in - 4 * STEP;
	if (ts >= 0) v += .24 * n * exp(-14 * ts) + .22 * sin(6.283 * 190 * ts) * exp(-26 * ts);
	return v + (n - noise(p - 1. / RATE)) * (j == 2 ? .05 : .025) * exp(-(j == 2 ? 30 : 80) * t);
}

/* Plays the tape from song position p into pcm: at full speed for two times
 * round the song, or (len > 0) dragging to a stop over len seconds, the same
 * way the world does. A slow tape sounds dull, so it's filtered more. */
static int record(double p, float len) {
	float lp = 0, t, d, s;
	int i, n = len ? (len + .4) * RATE : 2 * SONG * RATE;
	for (i = 0; i < n; i++) {
		t = (float)i / RATE, d = len ? fminf(t / len, 1) : 0, s = powf(1 - d, 1.4) * (1 + .12 * d * sinf(11 * t));
		p += s / RATE;
		lp += ((s > 1e-4 ? music(p) * sqrtf(s) : 0) - lp) * (.1 + .85 * fminf(s, 1));
		pcm[i] = tanhf(lp) * 29000;
	}
	return n;
}

/* The battery chime: five quick notes up. */
static int chime(void) {
	int i, n = .3 * RATE;
	for (i = 0; i < n; i++) {
		float t = (float)i / RATE;
		pcm[i] = ((fmod(freq(84 + "\0\4\7\14\20"[(int)fminf(t / .045, 4)]) * t, 1) < .5) * 2 - 1) * expf(-9 * t) * 12000;
	}
	return n;
}

static void save(const char *name, int n) {
	char path[64];
	int h[] = {0x46464952, 36 + 2 * n, 0x45564157, 0x20746d66, 16, 0x10001, RATE, RATE * 2, 0x100002, 0x61746164, 2 * n};
	FILE *f;
	sprintf(path, "/tmp/walkman-%s.wav", name);
	if ((f = fopen(path, "wb"))) fwrite(h, 4, 11, f), fwrite(pcm, 2, n, f), fclose(f);
}

static int tune;  // the process playing the music
#ifdef DEV
static int mute;  // the bot's test plays without sound
#endif

/* Plays /tmp/walkman-NAME.wav in the background with afplay (macOS) or aplay
 * (Linux). With stop it becomes the music, and the old music stops. */
static void sound(const char *name, int stop) {
	char path[64];
	int pid;
#ifdef DEV
	if (mute) return;
#endif
	sprintf(path, "/tmp/walkman-%s.wav", name);
	if (!(pid = fork())) {
		execlp("afplay", "afplay", path, (char *)0);
		execlp("aplay", "aplay", "-q", path, (char *)0);
		_exit(0);
	}
	if (stop) {
		if (tune > 0) kill(tune, 9);
		tune = pid;
	}
}

/* ----------------------------------------------------------------- terminal */

static double now(void) {
	struct timeval t;
	gettimeofday(&t, 0);
	return t.tv_sec + t.tv_usec / 1e6;
}

static void quit(int sig) {
	if (tune > 0) kill(tune, 9);
	system("stty icanon echo");
	printf("\33[m\33[2J\33[H\33[?25h\33[?1049l");
	exit(sig);
}

/* Whatever was typed since the last frame: 1 if it was a jump. */
static int keys(void) {
	char b[64];
	int n = read(0, b, 64), j = 0;
	while (n-- > 0) {
		if (b[n] == 'q' || b[n] == 'Q') quit(0);
		j |= b[n] == ' ' || b[n] == 'w' || b[n] == 'k' || b[n] == 'A' || b[n] == '\r' || b[n] == '\n';
	}
	return j;
}

#ifdef DEV
#include "dev.h"
#endif

int main(int argc, char **argv) {
	struct winsize ws = {0};
	double t0, ms;
	long shown = 0;
	int press;
	ioctl(1, TIOCGWINSZ, &ws);
	pcm = malloc(RATE * 120), out = malloc(MAXW * MAXH * 20 + 64);
	COLS = ws.ws_col > MAXW ? MAXW : ws.ws_col, ROWS = ws.ws_row > MAXH ? MAXH : ws.ws_row;
#ifdef DEV
	if (dev(argc, argv)) return 0;
#endif
	if (COLS < 64 || ROWS < 24) return puts("walkman: make the terminal at least 64x24"), 1;
	W = COLS * 2, H = ROWS * 4, OY = H - DH, seed = getpid();
	save("g", chime()), save("m", record(0, 0));
	system("stty -icanon -echo min 0 time 0");
	signal(SIGINT, quit), signal(SIGTERM, quit), signal(SIGCHLD, SIG_IGN);
	printf("\33[?1049h\33[?25l\33[2J");
	restart();
	sound("m", 1);
	t0 = ms = now();
	for (;;) {
		press = keys();
#ifdef DEV
		if (autoplay) press = bot();
#endif
		if (stopped && press && frame > stopped + 30) {  // rewind: a new tape
			restart();
			sound("m", 1);
			ms = now();
			continue;
		}
		if (!dying) {
			if (now() - ms > 2 * SONG) sound("m", 1), ms += 2 * SONG;  // round again
			song = now() - ms;
		}
		update(press);
		if (dying == frame) save("t", record(song, why == 1 ? 3 : 1.2)), sound("t", 1);
		draw();
		fwrite(out, 1, frame_ansi(), stdout);
		fflush(stdout);
		frame++, shown++;
		if (now() < t0 + shown / 60.) usleep((t0 + shown / 60. - now()) * 1e6);
		else if (now() > t0 + shown / 60. + .1) t0 = now() - shown / 60.;
	}
}

/* Development helpers, compiled only with -DDEV: a bot, a fairness test over
 * many levels, and a recorder for the video. None of this is in walkman.c.
 *
 *   ./walkman-dev --test FRAMES SEEDS     the bot plays each seed headlessly
 *   ./walkman-dev --bot                   watch the bot play
 *   ./walkman-dev --record DIR SEED COLS ROWS DROP
 *        the bot plays SEED until the tape stops (it ignores batteries from
 *        frame DROP on), writing DIR/frames.ans and DIR/sound.wav
 */
#include <string.h>
#define LOOK 110

static int autoplay, drop_at = 1 << 30, nchimes;
static long chimes[512];

static void dev_sound(const char *name) {  // the recorder puts the chimes in the soundtrack
	if (*name == 'g' && nchimes < 512) chimes[nchimes++] = frame;
}

typedef struct {
	Body b;
	int n, seen[8];  // batteries touched so far
} Sim;

static void sim(Sim *s, int jump) {
	int k;
	step(&s->b, jump);
	if (s->b.got < 0) return;
	for (k = 0; k < s->n && s->seen[k] != s->b.got; k++);
	if (k == s->n && s->n < 8) s->seen[s->n++] = s->b.got;
}

/* How good is state s with n frames left to look ahead? Staying alive to the
 * end is worth 1000, each battery 100 (or -60 after drop_at); dying early
 * scores how long you lasted. With depth left it also tries jumping, and
 * returns in *best_k when to jump (-1: keep running). */
static int evaluate(Sim s, int depth, int n, int done, int *best_k) {
	Sim w = s;
	int i, per = frame >= drop_at ? -60 : 100;
	for (i = 0; i < n && !w.b.dead; i++) sim(&w, 0);
	int walk = w.b.dead ? (done + i) / 8 : 1000 + per * w.n, best = walk;
	*best_k = -1;
	if (!depth || n <= 0) return walk;
	short score[LOOK + 1];
	int stride = depth >= 3 ? 1 : depth == 2 ? 2 : 3;
	for (int k = 0; k < n; k++) score[k] = -32000;
	Sim c = s;
	for (int k = 0; k < n && !c.b.dead; k++) {
		if ((c.b.ground || c.b.coyote > 0) && k % stride == 0) {
			Sim j = c;
			int f = 1, sc, ignore;
			sim(&j, 1);
			while (!j.b.ground && !j.b.dead && f < 150) sim(&j, 0), f++;
			if (j.b.dead) sc = (done + k + f) / 8;
			else {
				if (k + f >= n) {  // past the horizon: still make sure we don't land in trouble
					Sim t = j;
					for (int g = 0; g < 24 && !t.b.dead; g++) sim(&t, 0);
					sc = t.b.dead ? (done + k + f) / 8 : 1000 + per * j.n;
				} else
					sc = evaluate(j, depth - 1, n - k - f, done + k + f, &ignore);
				Sim t = j;  // landings that leave little room to stand on cost a little
				int g = 0;
				while (g < 12 && sc >= 900) {
					sim(&t, 0);
					if (!t.b.ground) break;
					g++;
				}
				if (sc >= 900) sc -= 12 - g;
			}
			score[k] = sc;
			if (sc > best) best = sc;
		}
		sim(&c, 0);
	}
	if (best == walk) return walk;
	int a = -1, z = -1;  // jump in the middle of the last stretch where jumping is best
	for (int k = n - 1; k >= 0; k--) {
		if (score[k] == best) {
			if (z < 0) z = k;
			a = k;
		} else if (z >= 0 && score[k] > -32000)
			break;
	}
	*best_k = (a + z) / 2;
	return best;
}

static long plan_at = -1, plan_next;

static int bot(void) {
	if (dying) return 0;
	if (plan_at >= 0) {
		if (frame < plan_at) return 0;
		plan_at = -1;
		return 1;
	}
	if (!(P.ground || P.coyote > 0) || frame < plan_next) return 0;
	int k, sc;
	sc = evaluate((Sim){P}, 2, LOOK, 0, &k);
	if (getenv("BOTDEBUG") && P.x > 780 && P.x < 1040) fprintf(stderr, "eval at x=%.1f: k=%d score=%d\n", P.x, k, sc);
	if (k < 0) {
		plan_next = frame + 3;
		return 0;
	}
	if (k == 0) return 1;
	plan_at = frame + k;
	return 0;
}

static void dev_screen(int cols, int rows) {
	COLS = cols, ROWS = rows, W = COLS * 2, H = ROWS * 4, OY = H - DH;
}

static int dev_test(long frames, int seeds) {
	int bad = 0;
	setvbuf(stdout, 0, _IOLBF, 0);
	mute = 1;
	dev_screen(80, 24);
	for (int s = 1; s <= seeds; s++) {
		seed = s, plan_at = -1, plan_next = 0;
		restart();
		int cells = 0;
		while (!dying && frame < frames) {
			update(bot());
			cells += P.got >= 0;
			frame++;
		}
		if (dying) {
			bad++;
			printf("seed %3d: died at %5.1f s (%s) x %.0f, %d batteries\n", s, frame / 60., why == 1 ? "batteries" : "crash",
			       P.x, cells);
		}
	}
	printf("%d of %d seeds died within %.0f s\n", bad, seeds, frames / 60.);
	return 1;
}

static int dev_record(const char *dir, unsigned s, int cols, int rows, int drop) {
	char path[512];
	static float mixbuf[RATE * 120];
	int n, i, death = 0, deathsong_n = 0, limit = getenv("WALKMAN_FRAMES") ? atoi(getenv("WALKMAN_FRAMES")) : 60 * 110;
	double deathsong = 0;
	FILE *f;
	mute = 1, drop_at = drop;
	dev_screen(cols, rows);
	seed = s, plan_at = -1, plan_next = 0;
	restart();
	snprintf(path, sizeof path, "%s/frames.ans", dir);
	if (!(f = fopen(path, "wb"))) return perror(path), 1;
	while (!stopped || frame < stopped + 100) {
		int press = bot();
		update(press);
		if (dying == frame) death = frame, deathsong = song;
		draw();
		fwrite(out, 1, frame_ansi(), f);
		frame++;
		if (frame > limit) break;
	}
	fclose(f);
	long total = (long)(frame - 1) * RATE / 60;
	// The music from the top until the tape starts dying, then the tail.
	n = record(0, 0);
	long upto = death ? (long)((death - 1) * (double)RATE / 60) : total;
	for (long k = 0; k < upto && k < total; k++) mixbuf[k] = pcm[k % n] / 32768.;
	if (death) {
		deathsong_n = record(deathsong, why == 1 ? 3 : 1.2);
		for (i = 0; i < deathsong_n && upto + i < total; i++) mixbuf[upto + i] = pcm[i] / 32768.;
	}
	n = chime();
	for (int g = 0; g < nchimes; g++) {
		long at = (long)((chimes[g] - 1) * (double)RATE / 60);
		for (i = 0; i < n && at + i < total; i++) mixbuf[at + i] += pcm[i] / 32768.;
	}
	for (long k = 0; k < total; k++) pcm[k % (RATE * 60)] = 0;
	snprintf(path, sizeof path, "%s/sound.wav", dir);
	if (!(f = fopen(path, "wb"))) return perror(path), 1;
	int h[] = {0x46464952, (int)(36 + 2 * total), 0x45564157, 0x20746d66, 16, 0x10001, RATE, RATE * 2, 0x100002, 0x61746164, (int)(2 * total)};
	fwrite(h, 4, 11, f);
	for (long k = 0; k < total; k++) {
		float v = fmaxf(-1, fminf(1, mixbuf[k]));
		short x = v * 32767;
		fwrite(&x, 2, 1, f);
	}
	fclose(f);
	fprintf(stderr, "boss row at x %.0f (frame %.0f); rises from frame %.0f; discs meet you at frames", arena, (arena - 41) / 1.25 + 1, (arena - W - 41) / 1.25 + 1);
	for (i = 0; i < 6; i++) fprintf(stderr, " %.0f", (dm[i] - 41) / 1.25 + 1);
	fprintf(stderr, "; beaten at frame %.0f\n", (dm[5] + 8 - 41) / 1.25 + 1);
	fprintf(stderr, "recorded %d frames (%.2f s); died at frame %d (%s), stopped at %d, %d batteries, tape %04d\n", frame - 1,
	        (frame - 1) / 60., death, why == 1 ? "batteries" : "crash", stopped, nchimes, (int)(P.x / 6));
	return 1;
}

static int dev(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--test") && i + 2 < argc) return dev_test(atol(argv[i + 1]), atoi(argv[i + 2]));
		if (!strcmp(argv[i], "--record") && i + 5 < argc)
			return dev_record(argv[i + 1], atoi(argv[i + 2]), atoi(argv[i + 3]), atoi(argv[i + 4]), atoi(argv[i + 5]));
		if (!strcmp(argv[i], "--bot")) autoplay = 1;
		if (!strcmp(argv[i], "--mute")) mute = 1;
	}
	return 0;
}

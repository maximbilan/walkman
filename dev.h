/* Development helpers, compiled only with -DDEV: a bot, and a test that has
 * it play many levels to check that every jump the generator makes can be
 * made. None of this is in walkman.c.
 *
 *   ./walkman-dev --bot                   watch the bot play
 *   ./walkman-dev --test FRAMES SEEDS     the bot plays each seed, no screen
 */
#include <string.h>
#define LOOK 110  // frames the bot looks ahead

static int autoplay;

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
 * end is worth 1000, each battery 100; dying early scores how long you
 * lasted. With depth left it also tries jumping, and returns in *best_k when
 * to jump (-1: keep running). */
static int evaluate(Sim s, int depth, int n, int done, int *best_k) {
	Sim w = s;
	int i;
	for (i = 0; i < n && !w.b.dead; i++) sim(&w, 0);
	int walk = w.b.dead ? (done + i) / 8 : 1000 + 100 * w.n, best = walk;
	*best_k = -1;
	if (!depth || n <= 0) return walk;
	short score[LOOK + 1];
	int stride = depth > 1 ? 2 : 3;  // try a jump every few frames, more often near the top
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
					sc = t.b.dead ? (done + k + f) / 8 : 1000 + 100 * j.n;
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
	int k;
	evaluate((Sim){P, 0, {0}}, 2, LOOK, 0, &k);
	if (k < 0) {
		plan_next = frame + 3;
		return 0;
	}
	if (k == 0) return 1;
	plan_at = frame + k;
	return 0;
}

static int dev_test(long frames, int seeds) {
	int bad = 0;
	setvbuf(stdout, 0, _IOLBF, 0);
	mute = 1;
	COLS = 80, ROWS = 24, W = COLS * 2, H = ROWS * 4, OY = H - DH;
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

static int dev(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--test") && i + 2 < argc) return dev_test(atol(argv[i + 1]), atoi(argv[i + 2]));
		if (!strcmp(argv[i], "--bot")) autoplay = 1;
	}
	return 0;
}

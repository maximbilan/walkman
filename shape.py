"""Packs walkman.min.c into the shape of a Walkman wearing its headphones: walkman.c.

The #include lines are the top of the headband (each padded with a comment,
which says how to build it). Every other token is laid row by row into the
headband, the ear cups, the cable and the player: around the cassette window
(with its two reels), and up into the buttons. Each run of a row is justified
so the outline stays crisp, and spare cells are shared over every row so the
code ends exactly at the bottom. String literals that don't fit are split into
adjacent literals, and a unary minus stays next to its operand.

    python3 shape.py [scale]   # writes walkman.c and prints it
"""

import math
import re
import sys

SRC, OUT = "walkman.min.c", "walkman.c"
ASPECT = 2.0  # a terminal cell is about twice as tall as it is wide
MARGIN = 2

TOKEN = re.compile(
    r'"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
    r"|[A-Za-z_]\w*"
    r"|0[xX][0-9a-fA-F]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?"
    r"|->|\+\+|--|&&|\|\||<=|>=|==|!=|\+=|-=|\*=|/=|%=|&=|\|=|<<|>>"
    r"|[^\s]"
)


def tokenize(code):
    return TOKEN.findall(code)


def needs_space(a, b):
    return tokenize(a + b) != tokenize(a) + tokenize(b) or "/*" in a + b or "//" in a + b


UNARY_AFTER = {"=", "(", ",", "return", "*", "/", "+", "-", "<", ">", "?", ":", "&&", "||", "+=", "-=", "*=", "/=", "["}


def glue(tokens):
    units, i = [], 0
    while i < len(tokens):
        t = tokens[i]
        if t == "-" and i + 1 < len(tokens) and (not units or tokenize(units[-1])[-1] in UNARY_AFTER):
            units.append(t + tokens[i + 1])
            i += 2
        else:
            units.append(t)
            i += 1
    return units


def string_units(lit):
    return re.findall(r"\\[0-7]{1,3}|\\x[0-9a-fA-F]+|\\.|[^\\]", lit[1:-1])


# MARK: the outline (units; y grows up)


def rbox(x, y, a, b, r):
    qx, qy = abs(x) - a + r, abs(y) - b + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def ellipse(x, y, a, b):
    return math.hypot(x / a, y / b)


BODY = (0.0, -0.40, 0.80, 0.48, 0.07)  # center, half sizes, corner
WINDOW = (-0.07, -0.38, 0.55, 0.28, 0.04)
REELS = ((-0.32, -0.40), (0.18, -0.40))
BAND = (0.0, 0.40, 0.84, 0.50, 0.075)  # center, radii, thickness
CUP = (0.84, 0.36, 0.095, 0.15, 0.05)


def walkman(x, y):
    """Is (x, y) code?"""
    cx, cy, a, b, r = BODY
    if rbox(x - cx, y - cy, a, b, r) <= 0:
        wx, wy, wa, wb, wr = WINDOW
        if rbox(x - wx, y - wy, wa, wb, wr) < 0:  # the cassette window: only the reels show
            for rx, ry in REELS:
                d = math.hypot(x - rx, (y - ry))
                if 0.05 < d < 0.135:
                    return True
            # the tape running between the reels, under them
            return abs(y - (REELS[0][1] - 0.15)) < 0.012 and REELS[0][0] - 0.02 < x < REELS[1][0] + 0.02
        # the speaker grille on the right: four slits
        if 0.56 < x < 0.72 and -0.70 < y < -0.42 and (y + 0.70) % 0.075 < 0.014:
            return False
        return True
    # the buttons along the top, PLAY pressed down
    top = cy + b
    for i in range(5):
        x0 = 0.06 + i * 0.14
        h = 0.035 if i == 1 else 0.07
        if x0 < x < x0 + 0.11 and top <= y < top + h:
            return True
    # the headband
    bx, by, ra, rb, th = BAND
    if y >= by:
        e = ellipse(x - bx, y - by, ra, rb)
        if e <= 1 and ellipse(x - bx, y - by, ra - th, rb - th) >= 1:
            return True
    # the ear cups
    ux, uy, ua, ub, ur = CUP
    if rbox(abs(x) - ux, y - uy, ua, ub, ur) <= 0:
        return True
    # the cable: from the left cup down and around into the side of the player
    t = (0.21 - y) / (0.21 + 0.30)  # 0 at the cup, 1 at the jack
    if 0 <= t <= 1:
        xc = -0.84 - 0.10 * math.sin(t * math.pi) + 0.04 * t
        if abs(x - xc) < 0.03:
            return True
    return False


TOP, BOTTOM, LEFT, RIGHT = 0.92, -0.90, -0.98, 0.95


def mask(scale):
    cols = math.ceil((RIGHT - LEFT) * scale)
    rows, r = [], 0
    while True:
        y = TOP - (r + 0.5) * ASPECT / scale
        if y < BOTTOM:
            break
        r += 1
        runs, start = [], None
        for c in range(cols + 1):
            x = LEFT + (c + 0.5) / scale
            inside = c < cols and walkman(x, y)
            if inside and start is None:
                start = c
            elif not inside and start is not None:
                runs.append((start, c))
                start = None
        if runs:
            rows.append(runs)
    return rows, cols


# MARK: packing


def pack(tokens, rows, reserve=0.0):
    tokens = list(tokens)
    lines = []
    for runs in rows:
        line = []
        for start, end in runs:
            width = end - start
            cap = width - int(width * reserve + 0.5)
            seg, used = [], 0
            while tokens:
                tok = tokens[0]
                gap = 1 if seg and needs_space(seg[-1], tok) else 0
                if used + gap + len(tok) <= cap:
                    seg.append(tok)
                    used += gap + len(tok)
                    tokens.pop(0)
                    continue
                room = cap - used - gap
                if tok.startswith('"') and room >= 3:
                    units = string_units(tok)
                    n = 0
                    while n < len(units) and len("".join(units[: n + 1])) + 2 <= room:
                        n += 1
                    if 0 < n < len(units):
                        head = '"' + "".join(units[:n]) + '"'
                        seg.append(head)
                        used += gap + len(head)
                        tokens[0] = '"' + "".join(units[n:]) + '"'
                break
            line.append((start, width, seg))
        lines.append(line)
    return None if tokens else lines


def justify(seg, width):
    if not seg:
        return " " * width
    gaps = [1 if needs_space(a, b) else 0 for a, b in zip(seg, seg[1:])]
    extra = width - sum(map(len, seg)) - sum(gaps)
    if len(seg) == 1:
        return seg[0].ljust(width)
    n = len(gaps)
    for i in range(n):
        gaps[i] += extra * (i + 1) // n - extra * i // n
    out = seg[0]
    for g, tok in zip(gaps, seg[1:]):
        out += " " * g + tok
    return out


NOTES = [
    " walkman.c ",
    " a platformer you compile ",
    " cc walkman.c -o walkman -lm ",
    " ./walkman ",
    " SPACE jumps ",
    " grab the batteries ",
    " Q quits ",
]


def include_line(inc, runs, note):
    """The include, then a comment that fills the rest of the row's runs."""
    first, last = runs[0][0], runs[-1][1]
    row = [" "] * last
    body = inc + "/*"
    fill_end = last - 2
    text = list(body)
    # comment filler: '=' on the runs, spaces in the gaps, the note in the middle of the first run
    cells = []
    for c in range(first + len(body), fill_end):
        cells.append(c)
    for i, ch in enumerate(text):
        row[first + i] = ch
    inside = lambda c: any(s <= c < e for s, e in runs)
    for c in cells:
        row[c] = "=" if inside(c) else " "
    # center the note on the widest run
    s, e = max(runs, key=lambda r: r[1] - r[0])
    lo = max(s, first + len(body) + 1)
    hi = min(e, fill_end) - 1
    if hi - lo >= len(note):
        at = lo + (hi - lo - len(note)) // 2
        for i, ch in enumerate(note):
            row[at + i] = ch
    row[last - 2], row[last - 1] = "*", "/"
    return "".join(row)


def main():
    src = open(SRC).read()
    includes = [l.strip() for l in src.splitlines() if l.startswith("#")]
    code = "\n".join(l for l in src.splitlines() if not l.startswith("#"))
    tokens = glue(tokenize(code))

    def fit(scale):
        rows, total = mask(scale)
        head = rows[: len(includes)]
        if any(r[-1][1] - r[0][0] < len(inc) + 6 for r, inc in zip(head, includes)):
            return None
        body = rows[len(includes):]
        if not pack(tokens, body):
            return None
        lo, hi = 0.0, 0.3
        for _ in range(30):
            mid = (lo + hi) / 2
            if pack(tokens, body, mid):
                lo = mid
            else:
                hi = mid
        lines = pack(tokens, body, lo)
        flat = [seg for line in lines for _, _, seg in line]
        trailing = len(flat) - 1 - max(i for i, seg in enumerate(flat) if seg)
        return head, lines, trailing, total

    scale = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
    best = None
    while scale < 200:
        got = fit(scale)
        if got:
            if got[2] == 0:
                best = (scale, got)
                break
            best = best or (scale, got)
        scale += 0.1
    scale, (head, lines, _, total) = best
    text = []
    for runs, inc, note in zip(head, includes, NOTES):
        text.append((" " * MARGIN + include_line(inc, runs, note)).rstrip())
    for line in lines:
        row = [" "] * total
        for start, width, seg in line:
            row[start : start + width] = list(justify(seg, width))
        text.append((" " * MARGIN + "".join(row)).rstrip())
    out = "\n".join(text) + "\n"
    open(OUT, "w").write(out)
    print(out)
    cap = sum(w for line in lines for _, w, _ in line)
    print(f"scale {scale:.2f}: {len(text)} rows x {max(map(len, text))} cols, {cap} cells for "
          f"{sum(map(len, tokens))} chars of tokens", file=sys.stderr)


main()

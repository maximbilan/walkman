"""Turns walkman.readable.c into walkman.min.c: no DEV code, no comments, no
static or const, short names, the fewest spaces that still tokenize.

    python3 minify.py     # writes walkman.min.c and prints its size
"""

import collections
import re
import subprocess

SRC, OUT = "walkman.readable.c", "walkman.min.c"

TOKEN = re.compile(
    r"//[^\n]*|/\*.*?\*/"
    r'|"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
    r"|[A-Za-z_]\w*"
    r"|0[xX][0-9a-fA-F]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[fF]?"
    r"|->|\+\+|--|&&|\|\||<=|>=|==|!=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<=|>>=|<<|>>"
    r"|[^\s]",
    re.S,
)

KEYWORDS = set(
    "auto break case char const continue default do double else enum extern float for goto if int long register "
    "return short signed sizeof static struct switch typedef union unsigned void volatile while".split()
)
LIBRARY = set(
    "main malloc fork execlp _exit kill SIGCHLD SIG_IGN printf sprintf fwrite fopen fclose FILE stdout fflush puts exit system usleep read getpid ioctl TIOCGWINSZ "
    "winsize ws_col ws_row gettimeofday timeval tv_sec tv_usec signal SIGINT SIGTERM floorf ceilf roundf fabsf fabs "
    "fminf fmaxf sqrtf sinf cosf expf powf tanhf fmod sin exp atan2f lroundf".split()
)
AVOID = KEYWORDS | LIBRARY | {"y0", "y1", "yn", "j0", "j1", "jn"}


def tokens_of(code):
    return [t for t in TOKEN.findall(code) if not t.startswith(("//", "/*"))]


def needs_space(a, b):
    return TOKEN.findall(a + b) != [a, b]


def short_number(t):
    if t.lower().startswith("0x") or not re.fullmatch(r"[\d.]+", t) or "." not in t:
        return t
    t = t.rstrip("0")
    if t.startswith("0.") and len(t) > 2:
        t = t[1:]
    return t


def names():
    letters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    for a in letters:
        yield a
    for a in letters:
        for b in letters + "0123456789":
            yield a + b


def main():
    code = subprocess.run(["unifdef", "-UDEV", SRC], capture_output=True, text=True).stdout
    lines = code.splitlines()
    includes = [l.replace(" ", "") for l in lines if l.startswith("#include")]
    assert all(l.startswith("#include") for l in lines if l.startswith("#")), "only #include lines allowed"
    toks = tokens_of("\n".join(l for l in lines if not l.startswith("#")))

    # Join adjacent string literals, drop static and const, shorten numbers.
    out = []
    for t in toks:
        if t in ("static", "const"):
            continue
        if t.startswith('"') and out and out[-1].startswith('"'):
            out[-1] = out[-1][:-1] + t[1:]
            continue
        if t[0].isdigit() or t[0] == ".":
            t = short_number(t)
        out.append(t)
    toks = out

    # Rename: the most used names get the shortest replacements.
    count = collections.Counter(t for t in toks if re.fullmatch(r"[A-Za-z_]\w*", t) and t not in AVOID)
    gen = (n for n in names() if n not in AVOID)
    rename = {}
    for name, _ in count.most_common():
        rename[name] = next(gen)
    toks = [rename.get(t, t) for t in toks]

    text = toks[0]
    for a, b in zip(toks, toks[1:]):
        text += (" " if needs_space(a, b) else "") + b
    open(OUT, "w").write("\n".join(includes) + "\n" + text + "\n")
    print(f"{OUT}: {len(text)} chars of code, {len(includes)} includes, {len(rename)} names")


main()

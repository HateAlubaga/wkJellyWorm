"""Bake ProjectX sprite-builder art into plain WA sprite strips.

ProjectX's p_sprite_builder drew two things at runtime that WA cannot:
  * WormSprite: the weapon image (plus a hand) rotated to the aim angle and
    laid over the worm's aim pose.
  * WeapSprite: a projectile image rotated to its flight direction (and
    mirrored when flying left).
WA instead picks one pre-drawn frame by angle. This script reads every
package's ProjectX scripts, re-computes what the sprite builder would have
drawn for each aim angle / flight angle, and writes vertical strips that WA
can pick from, plus assets/_baked/bake.txt, which wkJellyWorm reads to use
them in place of the plain art.

usage: pxbake.py [--only pkg1,pkg2] [--dry]
"""
import glob, math, os, re, struct, sys, time

sys.path.insert(0, os.path.dirname(__file__))
import pngtool

P = r"C:\Program Files (x86)\by Decepticon\Worms Armageddon\jelly\packages"
MAX_STRIP_H = 4096     # wkJellyWorm refuses taller images
ROT_FRAMES = 32
AIM_FRAMES = 32

# ---------------------------------------------------------------- assets ---

def norm_name(s):
    out = ""
    for c in os.path.basename(s).lower():
        n = c if c.isalnum() else "_"
        if n != "_" or not out or out[-1] != "_":
            out += n
    return out


def version_dirs(pkg):
    return sorted(d for d in glob.glob(os.path.join(P, pkg, "*")) if os.path.isdir(d))


ASSETS = {}   # pkg -> {norm: path}
def assets_of(pkg):
    if pkg not in ASSETS:
        m = {}
        for v in version_dirs(pkg):
            a = os.path.join(v, "assets")
            if os.path.isdir(a):
                for f in os.listdir(a):
                    m.setdefault(norm_name(f), os.path.join(a, f))
        ASSETS[pkg] = m
    return ASSETS[pkg]


def resolve(pkg, file, extra=()):
    if not file:
        return None
    key = norm_name(file)
    for p in (pkg, pkg + "_gfx") + tuple(extra) + ("p_sprite_builder_graphics",):
        hit = assets_of(p).get(key)
        if hit:
            return hit
    return None

# --------------------------------------------------------------- trailer ---

def rs(d, p):
    n = struct.unpack_from("<I", d, p)[0]
    if n > 260 or p + 4 + n > len(d):
        raise ValueError
    return d[p + 4:p + 4 + n].decode("latin1"), p + 4 + n


def read_anims(d, p, strings_variant):
    try:
        p += 9
        if strings_variant:
            _, p = rs(d, p)
            _, p = rs(d, p)
        else:
            p += 8
        names = []
        for _ in range(9):
            s, p = rs(d, p)
            names.append(s)
        return names if p + 4 == len(d) else None
    except (ValueError, struct.error):
        return None


def parse_bin(path):
    d = open(path, "rb").read()
    if len(d) < 0x1E8:
        return None
    try:
        p = 0x1E0 + 4
        vals = []
        for _ in range(7):
            s, p = rs(d, p)
            vals.append(s)
    except (ValueError, struct.error):
        return None
    anims = read_anims(d, p, False) or read_anims(d, p, True)
    field = lambda off: struct.unpack_from("<i", d, 4 + off)[0]
    return dict(name1=vals[0], icon=vals[2], sprites=vals[4:7], aim=anims[0] if anims else "",
                mode60=field(0x64), sprite60=field(0x60))

# ------------------------------------------------------------ px parsing ---

WORM_DEFAULTS = dict(hand_radius=1.0, hand_rotation=1.0, hand_radial_rotation=1.0, hand_scale=1.0,
                     hand_type=2, zplane=10.0, scale=1.0, weap_rotation=1.0, weap_radial_rotation=1.0,
                     weap_radius=1.0, draw_hand=True, dont_rotate_with_angle=False, ang_if_static=0.5,
                     hand_ang_if_static=1.0, draw_worm=True, animate=False, anim_speed=1.0,
                     fix_flip_rotation=False)
WEAP_DEFAULTS = dict(animate=False, anim_speed=1.0, ZPlane=5.0, scale=1.0, hideWorm=False, rotation=0.0,
                     dont_rotate_by_speed=False, animate_by_speed=False, play_backwards=False,
                     ang_if_dont_rotate=0.0)


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def statements(src):
    # String literals never contain ; { } in these scripts in practice, but
    # keep them intact anyway.
    out, cur, q = [], [], False
    for c in src:
        if c == '"':
            q = not q
        if not q and c in ";{}":
            s = "".join(cur).strip()
            if s:
                out.append(s)
            cur = []
        else:
            cur.append(c)
    return out


class Px:
    def __init__(self):
        self.files = {}      # CFile var -> filename (last assignment wins, in order)
        self.sprites = {}    # CSprite var -> (filename, frames)
        self.env = {}
        self.objs = {}
        self.worm = []       # (weapName, sprite, params)
        self.weap = []       # (weapName, sprite, params)
        self.ctx_weapon = None

    def val(self, e):
        e = e.strip()
        while e.startswith("(") and e.endswith(")"):
            e = e[1:-1].strip()
        m = re.fullmatch(r'"([^"]*)"', e)
        if m:
            return m.group(1)
        if re.fullmatch(r"[-+]?\d+(\.\d*)?([eE][-+]?\d+)?f?|[-+]?\.\d+f?", e):
            return float(e.rstrip("f"))
        if e in ("true", "false"):
            return e == "true"
        m = re.fullmatch(r"(\w+)\s*->\s*Index", e)
        if m:
            return ("sprite", m.group(1))
        m = re.fullmatch(r"-\s*(\w+)", e)
        if m:
            v = self.env.get(m.group(1))
            return -v if isinstance(v, float) else None
        if re.fullmatch(r"\w+", e):
            return self.env.get(e)
        return None

    def load_sprite(self, var, fexpr, frames):
        fexpr = fexpr.strip()
        m = re.fullmatch(r'GetAttachment\(\s*"([^"]+)"\s*\)', fexpr)
        fname = m.group(1) if m else self.files.get(fexpr)
        if fname:
            try:
                n = int(float(frames))
            except ValueError:
                n = 1
            self.sprites[var] = (fname, max(1, n))

    def run(self, src):
        for st in statements(strip_comments(src)):
            m = re.search(r'CheckName\(\s*(?:"([^"]*)"|(\w+))\s*\)', st)
            if m:
                self.ctx_weapon = m.group(1) if m.group(1) is not None else self.env.get(m.group(2))
            for m in re.finditer(r'(\w+)\s*=\s*GetAttachment\(\s*"([^"]+)"\s*\)', st):
                self.files[m.group(1)] = m.group(2)
            m = re.search(r"(\w+)\s*=\s*LoadSprite\(\s*(.+?)\s*,\s*([\w.]+)\s*(?:,\s*[^)]*)?\)\s*$", st)
            if m:
                self.load_sprite(m.group(1), m.group(2), m.group(3))
                continue
            m = re.search(r"(\w+)\s*=\s*new\s+(CWormAnimParams|WeapSpriteParams)\b", st)
            if m:
                self.objs[m.group(1)] = dict(WORM_DEFAULTS if m.group(2) == "CWormAnimParams" else WEAP_DEFAULTS)
                continue
            m = re.search(r"new\s+WormSprite\s*\((.*)\)", st)
            if m:
                args = [a.strip() for a in m.group(1).split(",")]
                if len(args) == 3 and args[2] in self.objs:
                    self.worm.append((self.val(args[0]), self.val(args[1]), dict(self.objs[args[2]])))
                continue
            m = re.search(r"new\s+WeapSprite\s*\((.*)\)", st)
            if m:
                args = [a.strip() for a in m.group(1).split(",")]
                if len(args) == 3 and args[2] in self.objs:
                    self.weap.append((self.ctx_weapon, self.val(args[1]), dict(self.objs[args[2]])))
                continue
            m = re.fullmatch(r"(\w+)\s*->\s*(\w+)\s*=\s*(.+)", st, flags=re.S)
            if m and m.group(1) in self.objs:
                v = self.val(m.group(3))
                if v is not None:
                    self.objs[m.group(1)][m.group(2)] = v
                continue
            m = re.fullmatch(r"(?:(?:local|string|int|float|bool)\s+)?(\w+)\s*=\s*(.+)", st, flags=re.S)
            if m:
                v = self.val(m.group(2))
                if v is not None:
                    self.env[m.group(1)] = v


def requires(src):
    m = re.search(r"^\s*require\s+([^;]+);", src, flags=re.M)
    return [r.strip() for r in m.group(1).split(",")] if m else []


PXSRC = {}
def px_sources(pkg):
    if pkg not in PXSRC:
        srcs = []
        for v in version_dirs(pkg):
            for f in sorted(glob.glob(os.path.join(v, "scripts_px", "*.pxc"))):
                srcs.append(open(f, encoding="latin1").read())
        PXSRC[pkg] = srcs
    return PXSRC[pkg]


def interpret(pkg):
    """Runs the package's scripts after those of the libraries it requires, so
    sprites a library loads (cweps_gfx) are known by the time they are used."""
    order, seen = [], set()
    def visit(p, depth=0):
        if p in seen or depth > 6 or not os.path.isdir(os.path.join(P, p)):
            return
        seen.add(p)
        for s in px_sources(p):
            for r in requires(s):
                visit(r, depth + 1)
        order.append(p)
    visit(pkg)
    px = Px()
    sprite_owner = {}
    for p in order:
        before = set(px.sprites)
        mine = p == pkg
        if not mine:
            # Only harvest sprite definitions from libraries, not their
            # WormSprite/WeapSprite calls (those belong to the library).
            lib = Px()
            lib.files, lib.sprites = px.files, px.sprites
            for s in px_sources(p):
                lib.run(s)
        else:
            for s in px_sources(p):
                px.run(s)
        for k in set(px.sprites) - before:
            sprite_owner[k] = p
    return px, sprite_owner, order

# -------------------------------------------------------------- imaging ----

IMG = {}
def load(path):
    if path not in IMG:
        IMG[path] = pngtool.read(path)
    return IMG[path]


def frame_of(img, frames, idx):
    w, h, rows = img
    frames = max(1, min(frames, h))
    fh = h // frames
    idx = max(0, min(idx, frames - 1))
    return w, fh, rows[idx * fh:(idx + 1) * fh]


def blit(canvas, S, frame, cx, cy, ang, scale, mirror):
    """Draws `frame` centred on (cx, cy), mirrored first (ProjectX's +262144
    flag), then rotated clockwise by `ang` radians and scaled."""
    w, h, rows = frame
    if scale <= 0:
        return
    ca, sa = math.cos(ang), math.sin(ang)
    r = math.hypot(w, h) * scale / 2 + 1
    x0, x1 = max(0, int(cx - r)), min(S, int(cx + r) + 1)
    y0, y1 = max(0, int(cy - r)), min(S, int(cy + r) + 1)
    hw, hh = w / 2, h / 2
    inv = 1.0 / scale
    for Y in range(y0, y1):
        dy = Y + 0.5 - cy
        row = canvas[Y]
        for X in range(x0, x1):
            dx = X + 0.5 - cx
            sx = (dx * ca + dy * sa) * inv + hw
            sy = (-dx * sa + dy * ca) * inv + hh
            if mirror:
                sx = w - sx
            ix, iy = int(math.floor(sx)), int(math.floor(sy))
            if 0 <= ix < w and 0 <= iy < h:
                p = rows[iy][ix]
                if p[3] >= 128:
                    row[X] = p


def extent(frame, scale, dist):
    w, h, _ = frame
    return dist + math.hypot(w, h) * scale / 2


def write_strip(path, S, frames):
    rows = []
    for f in frames:
        rows.extend(f)
    pngtool.write(path, S, S * len(frames), rows)


CLEAR = (0, 0, 0, 0)

# ------------------------------------------------------------- bakers -----

def bake_projectile(src_frame, mode, params, out_path):
    """WA picks frame = angle * N >> 16 with angle 0 = up, clockwise
    (WA.exe 0x50950B via 0x575730). WeapSprite drew the image rotated by
    the flight angle plus `rotation` half-turns, mirrored while flying left."""
    mirror_left = mode in (1, 2)
    offset = math.pi * float(params.get("rotation") or 0.0)
    static = bool(params.get("dont_rotate_by_speed"))
    static_ang = float(params.get("ang_if_dont_rotate") or 0.0)
    scale = float(params.get("scale") or 1.0)
    S = int(math.ceil(extent(src_frame, scale, 0) * 2)) + 2
    S += S % 2
    n = min(ROT_FRAMES, MAX_STRIP_H // S)
    if n < 4:
        return None
    frames = []
    for k in range(n):
        a = (k + 0.5) * 2 * math.pi / n
        canvas = [[CLEAR] * S for _ in range(S)]
        left = mirror_left and a > math.pi
        blit(canvas, S, src_frame, S / 2, S / 2, static_ang if static else a + offset, scale, left)
        frames.append(canvas)
    write_strip(out_path, S, frames)
    return n


def hand_image(ht):
    name = {3: "hand_rad_upper.png", 1: "hand_rad_lower.png"}.get(int(ht), "hand_rad.png")
    return resolve("p_sprite_builder_graphics", name)


def bake_aim(body, body_frames, weap_frame, prm, out_path):
    """Recomputes WormSprite::CalculateAngle for the worm facing right
    (TurnSide > 0), draws body, hand and weapon, then mirrors the frame:
    WA's worm sheets face left and WA mirrors them for a right-facing worm."""
    pi = math.pi
    no_rot = bool(prm["dont_rotate_with_angle"])
    wr = float(prm["weap_radius"]); wrr = float(prm["weap_radial_rotation"]); wrot = float(prm["weap_rotation"])
    hr = float(prm["hand_radius"]); hrr = float(prm["hand_radial_rotation"]); hrot = float(prm["hand_rotation"])
    scale = float(prm["scale"]); hscale = float(prm["hand_scale"])
    draw_hand = bool(prm["draw_hand"]); draw_worm = bool(prm["draw_worm"]) and body is not None
    hand = None
    if draw_hand:
        hp = hand_image(prm["hand_type"])
        hand = frame_of(load(hp), 1, 0) if hp else None
    S = 60
    S = max(S, int(math.ceil(extent(weap_frame, scale, abs(wr)) * 2)) + 2)
    if hand:
        S = max(S, int(math.ceil(extent(hand, hscale, abs(hr)) * 2)) + 2)
    if body is not None:
        S = max(S, body[0], body[1] // max(1, body_frames))
    S += S % 2
    n = AIM_FRAMES if body_frames >= AIM_FRAMES or body is None else 1
    n = min(n, MAX_STRIP_H // S)
    if n < 1:
        return None
    frames = []
    for i in range(n):
        F = (i + 0.5) / n if n > 1 else 0.5
        rot = pi / 2 if no_rot else (1 - F) * pi
        c = S / 2
        canvas = [[CLEAR] * S for _ in range(S)]
        if draw_worm:
            bf = frame_of(body, body_frames, i if body_frames >= n else 0)
            blit(canvas, S, bf, c, c, 0.0, 1.0, True)   # face right
        if hand:
            rotH = rot - pi * hrr
            hx, hy = c + math.sin(rotH) * hr, c - math.cos(rotH) * hr
            hang = float(prm["hand_ang_if_static"]) * pi if no_rot else rot + pi * hrot
            blit(canvas, S, hand, hx, hy, hang, hscale, False)
        rotW = rot + pi * wrr
        wx, wy = c + math.sin(rotW) * wr, c - math.cos(rotW) * wr
        wang = float(prm["ang_if_static"]) * pi if no_rot else pi - pi * F - pi * (1 - wrot)
        blit(canvas, S, weap_frame, wx, wy, wang, scale, False)
        frames.append([list(reversed(r)) for r in canvas])   # back to facing left
    write_strip(out_path, S, frames)
    return n

# --------------------------------------------------------------- driver ----

def safe(s):
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")[:40] or "x"


def main():
    only = None
    dry = "--dry" in sys.argv
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    t0 = time.time()

    # Every weapon ProjectX ever shipped as a .bin, by name: which package owns
    # it (that is where wkJellyWorm looks for bake.txt), its launch mode and
    # the worm aim pose from its trailer.
    bins = {}
    for pkg in sorted(os.listdir(P)):
        for v in version_dirs(pkg):
            for b in glob.glob(os.path.join(v, "weapons_raw", "*.bin")):
                t = parse_bin(b)
                if t and t["name1"]:
                    bins.setdefault(t["name1"].lower(), []).append((pkg, v, t))

    report = {"aim": 0, "proj": 0, "skip": []}
    manifests = {}   # version dir -> lines
    pkgs = sorted(p for p in os.listdir(P) if os.path.isdir(os.path.join(P, p)))
    for pkg in pkgs:
        if only and pkg not in only:
            continue
        if not any(px_sources(pkg)):
            continue
        srcs = "\n".join(px_sources(pkg))
        if "WormSprite" not in srcs and "WeapSprite" not in srcs:
            continue
        px, owner, order = interpret(pkg)
        extra = tuple(order)

        def sprite_file(ref):
            if not (isinstance(ref, tuple) and ref[0] == "sprite"):
                return None, 1
            s = px.sprites.get(ref[1])
            if not s:
                return None, 1
            f = resolve(owner.get(ref[1], pkg), s[0], extra)
            return f, s[1]

        done = set()
        for name, ref, prm in px.worm:
            if not isinstance(name, str):
                report["skip"].append((pkg, "worm name?", str(name)))
                continue
            owners = [o for o in bins.get(name.lower(), []) if o[0] == pkg] or bins.get(name.lower(), [])
            if not owners:
                report["skip"].append((pkg, "no .bin for WormSprite", name))
                continue
            wf, wn = sprite_file(ref)
            if not wf:
                report["skip"].append((pkg, "weapon image?", name, str(ref)))
                continue
            for opkg, over, t in owners:
                key = ("aim", over, name.lower())
                if key in done:
                    continue
                done.add(key)
                body_path = resolve(opkg, t["aim"], extra) if t["aim"] else None
                if not body_path:
                    # No usable pose in the trailer: ProjectX's stock aim pose.
                    body_path = resolve("p_sprite_builder_graphics", "def_shot_aim_p.png")
                body, bframes = None, 1
                if body_path:
                    body = load(body_path)
                    bframes = max(1, body[1] // body[0]) if body[1] % body[0] == 0 else 1
                elif prm["draw_worm"]:
                    report["skip"].append((pkg, "no aim pose", name, t["aim"]))
                    continue
                out_rel = "_baked/aim_%s.png" % safe(name)
                out = os.path.join(over, "assets", out_rel.replace("/", os.sep))
                if not dry:
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    n = bake_aim(body, bframes, frame_of(load(wf), wn, 0), prm, out)
                else:
                    n = 1
                if n:
                    manifests.setdefault(over, []).append("aim\t%s\t%s" % (t["name1"], out_rel))
                    report["aim"] += 1

        for name, ref, prm in px.weap:
            if not isinstance(name, str):
                report["skip"].append((pkg, "WeapSprite without CheckName", str(ref)))
                continue
            owners = [o for o in bins.get(name.lower(), []) if o[0] == pkg] or bins.get(name.lower(), [])
            if not owners:
                report["skip"].append((pkg, "no .bin for WeapSprite", name))
                continue
            wf, wn = sprite_file(ref)
            if not wf:
                report["skip"].append((pkg, "projectile image?", name, str(ref)))
                continue
            for opkg, over, t in owners:
                key = ("proj", over, name.lower())
                if key in done:
                    continue
                done.add(key)
                mode = t["mode60"]
                animated = wn > 1 and prm.get("animate")
                if mode not in (1, 2, 4, 5, 6) or (animated and mode not in (1, 2)):
                    # Time-driven (0), sprite-stepping (3) or natively animated:
                    # WA already does the right thing with the plain image.
                    manifests.setdefault(over, []).append("proj\t%s\t%s\t%d" % (t["name1"], "=" + os.path.basename(wf), wn))
                    report["proj"] += 1
                    continue
                out_rel = "_baked/proj_%s.png" % safe(name)
                out = os.path.join(over, "assets", out_rel.replace("/", os.sep))
                if not dry:
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    n = bake_projectile(frame_of(load(wf), wn, 0), mode, prm, out)
                else:
                    n = 1
                if n:
                    manifests.setdefault(over, []).append("proj\t%s\t%s\t%d" % (t["name1"], out_rel, n))
                    report["proj"] += 1

    for over, lines in manifests.items():
        path = os.path.join(over, "assets", "_baked", "bake.txt")
        if not dry:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("# generated by pxbake.py; kind<TAB>ProjectX weapon name<TAB>file under assets/<TAB>frames\n")
                f.write("\n".join(sorted(set(lines))) + "\n")
    print("aim composites: %d, projectiles: %d, packages with bake.txt: %d, %.1fs"
          % (report["aim"], report["proj"], len(manifests), time.time() - t0))
    for s in report["skip"]:
        print("skip:", *s)


if __name__ == "__main__":
    main()

"""Converts a model of the iPod mini into what the simulator's 3D device
(firmware/target/hosted/sdl/window-3d.c) draws: model.bin, the meshes in
millimetres with the device's centre at the origin and its face towards
+z, and the textures as JPEGs, smaller than the model's.

The model is 945950, "iPod Mini Blue": its OBJ,
iPod_Mini_Blue_obj.zip, is the author's control mesh, which is subdivided
here as their renders were, and its glTF, 945950-gltf-extended.gltf.zip,
has the same parts with PBR materials and textures, which are used. The
parts are named body003, button007 (the wheel), button008 (the centre
button), switch003 (hold), display003, display_glass003 and so on. Needs
numpy and Pillow.

The dock cable, which slides in when the phone charges, is the 30-pin
plug of GrabCAD's "30 pin connector to USB cable 1 meter" (its zip, with
an OBJ of the whole cable, coiled), with the straight cord behind it.

Usage: ipodmini2g-3d.py model.gltf.zip model_obj.zip outdir [cable.zip]
"""
import io, json, os, struct, sys, zipfile
import numpy as np
from PIL import Image

src, obj_src, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
cable_src = sys.argv[4] if len(sys.argv) > 4 else None
os.makedirs(outdir, exist_ok=True)
z = zipfile.ZipFile(src)
gltf_name = next(n for n in z.namelist() if n.endswith(".gltf"))
g = json.loads(z.read(gltf_name))
buf = z.read(g["buffers"][0]["uri"])

def accessor(i):
    a = g["accessors"][i]
    bv = g["bufferViews"][a["bufferView"]]
    n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
    dt = np.dtype({5126: "<f4", 5123: "<u2", 5125: "<u4"}[a["componentType"]])
    off = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride", n * dt.itemsize)
    rows = np.frombuffer(buf, np.uint8, (a["count"] - 1) * stride + n * dt.itemsize, off)
    rows = np.lib.stride_tricks.as_strided(rows, (a["count"], n * dt.itemsize), (stride, 1))
    return np.ascontiguousarray(rows).view(dt).reshape(a["count"], n)

def node_matrix(n):
    if "matrix" in n:
        return np.array(n["matrix"], float).reshape(4, 4).T
    return np.eye(4)

# Parts by role, see window-3d.c
ROLE = {"body003": 1, "display003": 2, "display_glass003": 3, "switch003": 4,
        "button007": 5}

# Each part's material, by name, from the glTF
material_of = {}
def walk(i):
    n = g["nodes"][i]
    if "mesh" in n:
        material_of[n["name"]] = g["meshes"][n["mesh"]]["primitives"][0].get("material", 0)
    for c in n.get("children", []):
        walk(c)
for i in g["scenes"][g.get("scene", 0)]["nodes"]:
    walk(i)

def catmull_clark(faces, points):
    """One step of Catmull-Clark subdivision of a quad mesh, as 3ds Max's
    TurboSmooth does: the model is the author's control mesh, smooth only
    once subdivided. Edges used by one face, the mesh's edge or a seam in
    the texture coordinates, follow a cubic B-spline. Works on positions
    and on texture coordinates alike."""
    nf, nv = len(faces), len(points)
    fp = points[faces].mean(1)
    edge = np.stack([faces, np.roll(faces, -1, 1)], -1).reshape(-1, 2)
    uniq, eid = np.unique(np.sort(edge, 1), axis=0, return_inverse=True)
    eid = eid.reshape(nf, 4)
    ne = len(uniq)
    uses = np.bincount(eid.ravel(), minlength=ne)
    fsum = np.zeros((ne, points.shape[1]))
    np.add.at(fsum, eid.ravel(), np.repeat(fp, 4, 0))
    mid = (points[uniq[:, 0]] + points[uniq[:, 1]]) / 2
    inner = uses == 2
    ep = np.where(inner[:, None], (2 * mid + fsum) / 4, mid)

    # vertices inside the surface: (Q + 2R + (n - 3)P) / n
    nfv = np.bincount(faces.ravel(), minlength=nv)
    q = np.zeros_like(points)
    np.add.at(q, faces.ravel(), np.repeat(fp, 4, 0))
    nev = np.bincount(uniq.ravel(), minlength=nv)
    r = np.zeros_like(points)
    np.add.at(r, uniq[:, 0], mid)
    np.add.at(r, uniq[:, 1], mid)
    n = np.maximum(nev, 1)[:, None]
    vp = (q / np.maximum(nfv, 1)[:, None] + 2 * r / n + (n - 3) * points) / n
    # on an edge of it: along the edge, a corner stays put
    border = uniq[~inner]
    nb = np.bincount(border.ravel(), minlength=nv)
    nsum = np.zeros_like(points)
    np.add.at(nsum, border[:, 0], points[border[:, 1]])
    np.add.at(nsum, border[:, 1], points[border[:, 0]])
    vp = np.where((nb == 2)[:, None], 0.75 * points + nsum / 8, vp)
    vp = np.where(((nb != 0) & (nb != 2))[:, None], points, vp)
    vp = np.where((nfv == 0)[:, None], points, vp)

    new_points = np.vstack([vp, ep, fp])
    e = eid + nv
    f = (np.arange(nf) + nv + ne)[:, None]
    new_faces = np.concatenate([np.stack([faces[:, k], e[:, k], f[:, 0], e[:, k - 1]], 1)
                                for k in range(4)])
    return new_faces, new_points

def read_obj(path):
    """The OBJ's parts: their quads' position and texture coordinate
    indices, and the positions and texture coordinates"""
    with zipfile.ZipFile(path) as zo:
        text = zo.read(next(n for n in zo.namelist() if n.endswith(".obj"))).decode()
    v, vt, part_faces, name = [], [], {}, None
    for line in text.splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "v":
            v.append([float(x) for x in w[1:4]])
        elif w[0] == "vt":
            vt.append([float(x) for x in w[1:3]])
        elif w[0] == "g":
            name = w[1].replace("_pivot", "")
        elif w[0] == "f":
            corners = [c.split("/") for c in w[1:]]
            part_faces.setdefault(name, []).append(
                [(int(c[0]) - 1, int(c[1]) - 1) for c in corners])
    # cm, z up, into the glTF's mm, y up, face towards +z
    v = np.array(v) * 10
    v = np.c_[v[:, 0], v[:, 2], -v[:, 1]]
    vt = np.array(vt)
    vt[:, 1] = 1 - vt[:, 1]
    return v, vt, part_faces

# The dock connector's pins are boxes, not smoothed by their author
SHARP = ("bottom_connector008",)
LEVELS = 2

obj_v, obj_vt, obj_parts = read_obj(obj_src)
parts = []
for name, quads in obj_parts.items():
    quads = np.array(quads)
    # positions shared at the same place, whatever their index
    _, at = np.unique(np.round(obj_v[quads[:, :, 0].ravel()], 5), axis=0, return_inverse=True)
    fpos = at.reshape(-1, 4)
    ppos = np.zeros((at.max() + 1, 3))
    ppos[at] = obj_v[quads[:, :, 0].ravel()]
    _, uat = np.unique(quads[:, :, 1].ravel(), return_inverse=True)
    fuv = uat.reshape(-1, 4)
    puv = np.zeros((uat.max() + 1, 2))
    puv[uat] = obj_vt[quads[:, :, 1].ravel()]
    for _ in range(0 if name in SHARP else LEVELS):
        fpos, ppos = catmull_clark(fpos, ppos)
        fuv, puv = catmull_clark(fuv, puv)
    # normals of the smooth surface, from its faces round each position
    tri_p = np.r_[fpos[:, [0, 1, 2]], fpos[:, [0, 2, 3]]]
    tri_t = np.r_[fuv[:, [0, 1, 2]], fuv[:, [0, 2, 3]]]
    fn = np.cross(ppos[tri_p[:, 1]] - ppos[tri_p[:, 0]], ppos[tri_p[:, 2]] - ppos[tri_p[:, 0]])
    pn = np.zeros_like(ppos)
    for k in range(3):
        np.add.at(pn, tri_p[:, k], fn)
    pn /= np.linalg.norm(pn, axis=1, keepdims=True) + 1e-12
    if name in SHARP:
        # flat faces, flat normals
        corner_n = np.repeat(fn / (np.linalg.norm(fn, axis=1, keepdims=True) + 1e-12), 3, 0)
        keys = np.c_[tri_p.ravel(), tri_t.ravel(), np.round(corner_n, 4)]
    else:
        keys = np.c_[tri_p.ravel(), tri_t.ravel()]
    uniq, inv = np.unique(keys, axis=0, return_inverse=True)
    pos = ppos[uniq[:, 0].astype(int)]
    uv = puv[uniq[:, 1].astype(int)]
    nrm = uniq[:, 2:5] if name in SHARP else pn[uniq[:, 0].astype(int)]
    idx = inv.reshape(-1).astype(np.uint32)
    parts.append((name, material_of[name], pos, nrm, uv, idx))

def part(name):
    return next(p for p in parts if p[0] == name)

# The device's centre at the origin
allpos = np.vstack([p[2] for p in parts])
centre = (allpos.max(0) + allpos.min(0)) / 2
parts = [(n, mat, pos - centre, nrm, uv, idx) for n, mat, pos, nrm, uv, idx in parts]
half = (allpos.max(0) - allpos.min(0)) / 2

# The wheel: centre and radius from its face, the centre button's radius
wpos = part("button007")[2]
wface = wpos[wpos[:, 2] > wpos[:, 2].max() - 0.05]
wheel_c = (wface[:, :2].max(0) + wface[:, :2].min(0)) / 2
wheel_r = np.linalg.norm(wpos[:, :2] - wheel_c, axis=1).max()
bpos = part("button008")[2]
button_r = (bpos[:, 0].max() - bpos[:, 0].min()) / 2
front_z = wpos[:, 2].max()

def hole(name, plane, axis_a, axis_b, keep, res=0.05):
    """Extents of the opening round the origin of a part's faces that keep()
    selects, drawn into a grid on the axis_a/axis_b plane"""
    from PIL import ImageDraw
    pos, idx = part(name)[2], part(name)[5].reshape(-1, 3)
    a0, a1 = pos[:, axis_a].min() - 1, pos[:, axis_a].max() + 1
    b0, b1 = pos[:, axis_b].min() - 1, pos[:, axis_b].max() + 1
    im = Image.new("L", (int((a1 - a0) / res), int((b1 - b0) / res)))
    d = ImageDraw.Draw(im)
    for t in idx:
        if keep(pos[t]):
            d.polygon([((pos[k, axis_a] - a0) / res, (pos[k, axis_b] - b0) / res)
                       for k in t], fill=255)
    grid = np.array(im) > 0
    ca, cb = int((plane[0] - a0) / res), int((plane[1] - b0) / res)
    lo, hi = ca, ca
    while lo > 0 and not grid[cb, lo]: lo -= 1
    while hi < grid.shape[1] - 1 and not grid[cb, hi]: hi += 1
    blo, bhi = cb, cb
    while blo > 0 and not grid[blo, ca]: blo -= 1
    while bhi < grid.shape[0] - 1 and not grid[bhi, ca]: bhi += 1
    return (a0 + lo * res, a0 + hi * res, b0 + blo * res, b0 + bhi * res)

# The screen: the display frame's opening, and the LCD in it, centred,
# with the margins of the 2D skin (138x110 in a 148x118 window)
dpos = part("display_frame003")[2]
dc = (dpos.max(0) + dpos.min(0)) / 2
fx0, fx1, fy0, fy1 = hole("display_frame003", (dc[0], dc[1]), 0, 1, lambda p: True)
lcd_h = (fy1 - fy0) * 110 / 118
lcd_w = lcd_h * 138 / 110
lcd = ((fx0 + fx1 - lcd_w) / 2, (fy0 + fy1 - lcd_h) / 2,
       (fx0 + fx1 + lcd_w) / 2, (fy0 + fy1 + lcd_h) / 2)

# Hold: the switch's cap slides along x in a slot in the top. On hold it
# moves towards the slot's far end, uncovering orange at the near one: the
# plate the cap sits on is orange there, up to under the cap's round end.
spos = part("switch003")[2]
top = spos[:, 1].max()
cap = spos[spos[:, 1] > top - 0.3]
sc = (cap.max(0) + cap.min(0)) / 2
sx0, sx1, sz0, sz1 = hole("top003", (sc[0], sc[2]), 0, 2,
                      lambda p: p[:, 1].min() > top - 0.6)
cap_x0, cap_x1 = cap[:, 0].min(), cap[:, 0].max()
travel = (sx1 - sx0) - (cap_x1 - cap_x0) - 0.1
switch_min = spos.min(0)
switch_max = spos.max(0)
top_surface = part("top003")[2][:, 1].max()
plate = spos[spos[:, 1] < top_surface][:, 1].max()
cap_round = (cap[:, 2].max() - cap[:, 2].min()) / 2
orange = (cap_x0 + cap_round, plate + 0.05, 0, 0, 0)  # x below, y below

# Textures, smaller: the body's colour at 2048, the rest at 1024
textures = {}
def texture(info, size):
    if info is None:
        return ""
    image = g["images"][g["textures"][info["index"]]["source"]]["uri"]
    name = os.path.splitext(image)[0] + ".jpg"
    if name not in textures:
        im = Image.open(io.BytesIO(z.read(image))).convert("RGB")
        if im.width > size:
            im = im.resize((size, size), Image.LANCZOS)
        im.save(os.path.join(outdir, name), quality=90)
        textures[name] = im
    return name

materials = []
for m in g["materials"]:
    pbr = m.get("pbrMetallicRoughness", {})
    big = 2048 if m.get("name") == "body_blue" else 1024
    materials.append((m.get("name", ""),
                      pbr.get("baseColorFactor", [1, 1, 1, 1]),
                      pbr.get("metallicFactor", 1.0),
                      pbr.get("roughnessFactor", 1.0),
                      texture(pbr.get("baseColorTexture"), big),
                      texture(pbr.get("metallicRoughnessTexture"), 1024),
                      texture(m.get("normalTexture"), 1024)))

# The dock cable's plug and cord, plugged in (window-3d.c slides it), in
# materials of its own
ROLE_CABLE = 6
def add_cable(path):
    with zipfile.ZipFile(path) as zc:
        text = zc.read(next(n for n in zc.namelist() if n.endswith(".obj"))).decode()
    v, vn, faces = [], [], []
    mat = None
    for line in text.splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "v":
            v.append([float(x) for x in w[1:4]])
        elif w[0] == "vn":
            vn.append([float(x) for x in w[1:4]])
        elif w[0] == "usemtl":
            mat = w[1]
        elif w[0] == "f":
            faces.append(([int(c.split("/")[0]) - 1 for c in w[1:]],
                          [int(c.split("/")[2]) - 1 for c in w[1:]], mat))
    v, vn = np.array(v), np.array(vn)
    # in the OBJ, mm: the 30-pin plug points along +z, 26 wide in x and 6
    # thick in y, its housing ending at z 80.4, its metal tip 6 longer; its
    # cord runs straight back along -z to z -10, then coils
    head = lambda p: (p[:, 0] > -66).all() and (p[:, 0] < -38).all() and (p[:, 2] > 60).all()
    cord = lambda p: (np.abs(p[:, 0] + 52) < 3).all() and (p[:, 2] > -8).all() and (p[:, 2] <= 61).all()
    plug_x, plug_y, face_z = -52.0, 2.9, 80.4
    kept = [f for f in faces if head(v[f[0]]) or cord(v[f[0]])]
    # the cord's far end, much further, out of sight
    vv = v.copy()
    vv[vv[:, 2] < 10, 2] -= 240

    dock = part("bottom_connector007")[2]
    dock_c = (dock.max(0) + dock.min(0)) / 2
    bottom = min(p[2][:, 1].min() for p in parts if p[0] in ("body003", "bottom003"))
    def to_ipod(p):
        """the plug up into the dock, its housing against the bottom"""
        return np.c_[p[:, 0] - plug_x + dock_c[0], p[:, 2] - face_z + bottom,
                     -(p[:, 1] - plug_y) + dock_c[2]]

    base = len(materials)
    materials.extend([("cable_plastic", [0.9, 0.9, 0.9, 1], 0.0, 0.35, "", "", ""),
                      ("cable_insert", [0.03, 0.03, 0.03, 1], 0.0, 0.45, "", "", ""),
                      ("cable_metal", [0.8, 0.8, 0.8, 1], 1.0, 0.3, "", "", "")])
    def material(f):
        p = v[f[0]]
        if f[2] == "diffuse_Black":
            return base + 1
        if head(p) and (p[:, 2] >= 80.8).all():  # the pins; not the plate
            return base + 2                       # at the housing's end
        return base
    for m in range(3):
        sel = [f for f in kept if material(f) == base + m]
        pos, nrm, idx, at = [], [], [], {}
        for vi, ni, _ in sel:
            corners = []
            for a, b in zip(vi, ni):
                if (a, b) not in at:
                    at[a, b] = len(pos)
                    pos.append(vv[a])
                    nrm.append(vn[b])
                corners.append(at[a, b])
            for k in range(1, len(corners) - 1):
                idx += [corners[0], corners[k], corners[k + 1]]
        pos = to_ipod(np.array(pos))
        nrm = np.array(nrm)
        nrm = np.c_[nrm[:, 0], nrm[:, 2], -nrm[:, 1]]
        parts.append((f"cable{m}", base + m, pos, nrm, np.zeros((len(pos), 2)),
                      np.array(idx, np.uint32)))
        ROLE[f"cable{m}"] = ROLE_CABLE
if cable_src:
    add_cable(cable_src)

# The body's average colour, which a tint replaces (window-3d.c)
body_tex = next(mt[4] for mt in materials if mt[0] == "body_blue")
avg = np.asarray(textures[body_tex].resize((64, 64)), float).reshape(-1, 3).mean(0) / 255

def name32(s):
    return s.encode()[:31].ljust(32, b"\0")

with open(os.path.join(outdir, "model.bin"), "wb") as f:
    f.write(b"RB3D" + struct.pack("<I", 1))
    meta = [*half, *wheel_c, wheel_r, button_r, front_z, *lcd,
            *switch_min, *switch_max, travel, *orange, *avg]
    f.write(struct.pack("<I", len(meta)) + struct.pack(f"<{len(meta)}f", *meta))
    f.write(struct.pack("<I", len(materials)))
    for name, base, metal, rough, tb, torm, tn in materials:
        f.write(name32(name) + struct.pack("<6f", *base, metal, rough)
                + name32(tb) + name32(torm) + name32(tn))
    f.write(struct.pack("<I", len(parts)))
    for name, mat, pos, nrm, uv, idx in parts:
        verts = np.c_[pos, nrm, uv].astype("<f4")
        f.write(name32(name) + struct.pack("<4I", mat, ROLE.get(name, 0),
                                           len(verts), len(idx)))
        f.write(verts.tobytes() + idx.astype("<u4").tobytes())

print(f"{len(parts)} parts, {os.path.getsize(os.path.join(outdir, 'model.bin')) >> 10} KB, "
      f"{len(textures)} textures, wheel r {wheel_r:.2f} "
      f"at {wheel_c.round(2)}, LCD {np.round(lcd, 2)}, hold travel {travel:.2f}")

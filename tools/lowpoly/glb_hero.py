# Reads a hero model made of boxes from a .glb (Blender: File > Export > glTF 2.0)
# and turns it into block lines for emit.py. Each mesh object must be a box:
# Blender's cube, moved / scaled / turned about the forward-up plane (Z in the
# game's units), optionally with its top face scaled (taper) or its back face
# scaled (tback). Units: 1 = one SNES pixel; +X forward, +Y up (glTF), feet at 0.
#
# Object names pick the limb: "Nearleg", "Farleg", "Neararm", "Fararm", "Wing",
# "Tail" (anything else is the body). Material names pick the look: "glow" in
# the name = unlit glow, "tex" = panel texture, otherwise plain lit. A box
# without a material keeps the colour and look of the Kestrel block with the
# same number (kKestrel_NN_... names), or light grey.
import json, struct, math

LIMBS = (('nearleg', 1), ('farleg', 2), ('neararm', 3), ('fararm', 4), ('wing', 5), ('tail', 6))

def _srgb(c):
    c = min(max(c, 0.0), 1.0)
    return c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055

def load(path, fallback=None):
    d = open(path, 'rb').read()
    if d[:4] != b'glTF': raise SystemExit('%s is not a .glb file' % path)
    off, chunks = 12, []
    while off < len(d):
        n, kind = struct.unpack('<I4s', d[off:off + 8]); chunks.append(d[off + 8:off + 8 + n]); off += 8 + n
    j, buf = json.loads(chunks[0]), chunks[1]
    def acc(i):
        a = j['accessors'][i]; bv = j['bufferViews'][a['bufferView']]
        o = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        assert a['type'] == 'VEC3' and a['componentType'] == 5126, 'positions must be float VEC3'
        stride = bv.get('byteStride', 12)
        return [struct.unpack('<3f', buf[o + k * stride:o + k * stride + 12]) for k in range(a['count'])]
    blocks = []
    for node in j['nodes']:
        if 'mesh' not in node: continue
        name = node.get('name', '?')
        if 'matrix' in node: raise SystemExit('%s: apply transforms differently (matrix not supported)' % name)
        prim = j['meshes'][node['mesh']]['primitives'][0]
        verts = sorted(set((round(x, 5), round(y, 5), round(z, 5)) for x, y, z in acc(prim['attributes']['POSITION'])))
        if len(verts) != 8: raise SystemExit('%s: not a box (%d corners); only boxes are supported' % (name, len(verts)))
        s = node.get('scale', [1, 1, 1]); t = node.get('translation', [0, 0, 0]); q = node.get('rotation', [0, 0, 0, 1])
        if abs(q[0]) > 1e-4 or abs(q[1]) > 1e-4: raise SystemExit('%s: may only be turned about the forward-up plane' % name)
        rz = 2 * math.atan2(q[2], q[3])
        v = [(x * s[0], y * s[1], z * s[2]) for x, y, z in verts]
        cx = sum(p[0] for p in v) / 8; cy = sum(p[1] for p in v) / 8; cz = sum(p[2] for p in v) / 8
        fb = [p for p in v if p[0] > cx and p[1] < cy]          # front bottom
        ft = [p for p in v if p[0] > cx and p[1] > cy]          # front top
        bb = [p for p in v if p[0] < cx and p[1] < cy]          # back bottom
        if len(fb) != 2 or len(ft) != 2 or len(bb) != 2: raise SystemExit('%s: not a box' % name)
        my = (min(p[1] for p in fb + ft) + max(p[1] for p in fb + ft)) / 2   # centre from the front face
        hx = fb[0][0] - cx; hy = my - fb[0][1]; hz = abs(fb[0][2] - cz)
        taper = (ft[0][0] - cx) / hx if hx else 1.0
        tback = (my - bb[0][1]) / hy if hy else 1.0
        # check: rebuild the 8 corners like lowpoly.c's LpPrepare
        mx, mz = cx, cz
        want = []
        for i in range(8):
            x = mx + (hx if i & 1 else -hx); y = my + (hy if i & 2 else -hy); z = mz + (hz if i & 4 else -hz)
            if i & 2: x = mx + (x - mx) * taper; z = mz + (z - mz) * taper
            if not i & 1: y = my + (y - my) * tback; z = mz + (z - mz) * tback
            want.append((round(x, 3), round(y, 3), round(z, 3)))
        have = sorted((round(p[0], 3), round(p[1], 3), round(p[2], 3)) for p in v)
        if any(abs(a - b) > 0.02 for p, w in zip(have, sorted(want)) for a, b in zip(p, w)):
            raise SystemExit('%s: box shape not supported (only top / back faces may be scaled)' % name)
        lname = name.lower()
        limb = next((l for key, l in LIMBS if key in lname), 0)
        mat, col = 1, 0xC8C8C8
        if prim.get('material') is not None:
            m = j['materials'][prim['material']]
            c = m.get('pbrMetallicRoughness', {}).get('baseColorFactor', [0.8, 0.8, 0.8, 1])
            col = sum(int(round(_srgb(c[k]) * 255)) << (16 - 8 * k) for k in range(3))
            mn = m.get('name', '').lower()
            mat = 2 if 'glow' in mn else 0 if 'tex' in mn else 1
        elif fallback is not None:
            parts = name.split('_')
            if len(parts) > 1 and parts[1].isdigit() and int(parts[1]) < len(fallback):
                f = fallback[int(parts[1])]; col, mat = f['col'], f['mat']
        X, Y, Z = cx + t[0], my + t[1], cz + t[2]
        # (rotation is about the box centre in lowpoly.c; glTF turns about the object origin)
        if rz:
            c_, s_ = math.cos(rz), math.sin(rz)
            X, Y = t[0] + c_ * cx - s_ * my, t[1] + s_ * cx + c_ * my
        blocks.append(dict(b=(X - hx, Y - hy, Z - hz, X + hx, Y + hy, Z + hz), col=col, mat=mat, limb=limb,
                           taper=taper, tback=tback, rz=rz))
    if not blocks: raise SystemExit('%s: no boxes found' % path)
    return blocks

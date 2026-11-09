# Emits src/platform/n64/lowpoly_models.h (Kestrel and Burrowdrake block tables).
# Usage: python3 tools/lowpoly/emit.py src/platform/n64/lowpoly_models.h [--hero FILE.glb|FILE.json | --no-hero]
# The hero comes from --hero, else tools/lowpoly/hero.json if it exists, else the built-in Kestrel.
import math, json, os, sys
GREEN, DKGREEN, PURPLE, HELMET, HDARK, CHAR, GLOW, AMBER, RED, VISOR, SILVER = \
    0x3FA34D, 0x2A6E35, 0x7A4FB0, 0x5B3A92, 0x35214F, 0x2E2F38, 0xD8FF3A, 0xFFB23A, 0xE0302A, 0x14161E, 0xC8CCD4
LIT, TEX, GLOWM = 1, 0, 2           # mat: 0 lit + texture, 1 lit, 2 unlit
def B(x0, y0, z0, x1, y1, z1, col, mat=LIT, limb=0, taper=1.0, tback=1.0, rz=0.0):
    if z0 > z1: z0, z1 = z1, z0
    return dict(b=(x0, y0, z0, x1, y1, z1), col=col, mat=mat, limb=limb, taper=taper, tback=tback, rz=rz)
def cone(base, r, apex, col, mat=LIT, limb=0):
    dx, dy = apex[0] - base[0], apex[1] - base[1]; L = math.hypot(dx, dy)
    cx, cy, cz = (base[0] + apex[0]) / 2, (base[1] + apex[1]) / 2, (base[2] + apex[2]) / 2
    return B(cx - r, cy - L / 2, cz - r, cx + r, cy + L / 2, cz + r, col, mat, limb, taper=0.08, rz=math.atan2(-dx, dy))

def kestrel():
    k = []
    for s, limb in ((1, 1), (-1, 2)):
        k.append(B(-3, 0, s * 1.6, 6, 3, s * 5.6, DKGREEN, limb=limb, taper=0.85))
        k.append(B(6, 0, s * 1.9, 8.6, 1.4, s * 5.3, SILVER, limb=limb, taper=0.35))   # toe claws (one wedge)
        k.append(B(-2, 3, s * 1.9, 2.6, 14, s * 5.3, GREEN, limb=limb, taper=1.12))
        k.append(B(2.6, 6, s * 3.1, 3.1, 12, s * 4.1, GLOW, GLOWM, limb))
        k.append(B(-2.4, 16, s * 1.6, 2.8, 24, s * 5.6, GREEN, limb=limb, taper=1.12))
    k += [B(-3.4, 23, -5.6, 3.6, 26, 5.6, CHAR), B(-2.8, 26, -4.4, 3, 30.5, 4.4, PURPLE, taper=1.3),
          B(-3.8, 30, -5.8, 4.2, 38, 5.8, GREEN, taper=1.1), B(4.2, 32, -0.9, 4.9, 34, 0.9, GLOW, GLOWM)]
    for s in (1, -1):
        k += [B(4.0, 34.5, s * 1.6, 4.5, 35.3, s * 4.4, GLOW, GLOWM),
              B(-2.6, 35, s * 5.8, 2.6, 38.2, s * 10, GREEN, taper=0.8),
              B(-6.4, 34, s * 1.2, -3.8, 42, s * 3.4, DKGREEN, taper=0.25)]
    for s, limb in ((1, 3), (-1, 4)):   # arms hang from the shoulder (pivot y 33.5)
        k += [B(-1.2, 27, s * 7, 1.2, 34, s * 9.4, PURPLE, limb=limb),
              B(-1.5, 18.6, s * 6.8, 1.5, 27.2, s * 9.8, GREEN, limb=limb, taper=1.1)]
    k += [B(-1.6, 21, 9.6, 2.0, 26.2, 12.2, CHAR, limb=3),              # wrist blaster along the forearm
          B(-1.0, 19.6, 10.2, 1.4, 21.0, 11.6, AMBER, GLOWM, 3),       # muzzle at the hand end
          B(1.95, 21.8, 10.4, 2.3, 25.4, 11.4, GLOW, GLOWM, 3)]
    k += [          B(-4.4, 39.4, -4.8, 5.4, 48.8, 4.8, HELMET, taper=0.72),      # helmet shell
          B(-3.6, 38.4, -4.2, 5.6, 41.8, 4.2, HELMET, taper=1.0),       # chin bar
          B(5.0, 42.0, -4.0, 6.0, 45.6, 4.0, VISOR, GLOWM),             # visor (the face)
          B(-2.6, 48.7, -0.6, 3.8, 49.3, 0.6, RED, LIT),                # top stripe
          B(1.0, 43.0, -5.3, 2.6, 45.0, -4.6, HDARK), B(1.0, 43.0, 4.6, 2.6, 45.0, 5.3, HDARK)]       # visor hinges
    return k

def drake(P):
    k = []
    for s, limb in ((1, 1), (-1, 2)):
        k.append(B(-2, 0, s * 1.8, 4.6, 2.4, s * 5.2, P['back'], TEX, limb, taper=0.8))
        for c in (-1.2, 0, 1.2): k.append(cone((4.6, 0.8, s * 3.5 + c), 0.5, (7.2, 0.2, s * 3.5 + c), P['horn'], LIT, limb))
        k.append(B(-2.6, 2.2, s * 2.2, 0.6, 9.6, s * 4.8, P['back'], TEX, limb, rz=0.35))
        k.append(B(-1.6, 8.6, s * 1.6, 3.6, 16.6, s * 5.6, P['patch'], TEX, limb, taper=0.9, rz=-0.4))
        k.append(B(2.4, 17, s * 4.2, 6.6, 19.4, s * 5.8, P['back'], TEX, rz=-0.5))
        k.append(cone((6.4, 16.6, s * 5), 0.6, (8, 15, s * 5), P['horn']))
        k.append(B(-7.5, 22, s * 4.0, -1.0, 31, s * 4.6, P['wing'], LIT, 5, taper=0.25))
        k.append(cone((2.5, 33, s * 1.6), 0.8, (-1.5, 37.5, s * 2.4), P['horn']))
        k.append(B(6.2, 31, s * 2.4, 7.2, 32, s * 3.05, P['eye'], GLOWM))
    k += [B(-4.4, 14, -5, 4.2, 26, 5, P['back'], TEX, taper=0.8, rz=-0.25),
          B(3.4, 15, -3.4, 4.8, 25, 3.4, P['belly'], LIT, taper=0.8, rz=-0.25),
          B(-0.6, 24.5, -2.8, 3.8, 30, 2.8, P['back'], TEX, taper=0.85, rz=-0.45),
          B(0.6, 29.2, -3.0, 10.6, 33.6, 3.0, P['back'], TEX, taper=0.8),
          B(3.0, 27.6, -2.4, 10.0, 29.4, 2.4, P['belly'])]
    for x, y in [(-1.6, 28.6), (-3.8, 23.6), (-5.0, 17.6)]:
        k.append(cone((x, y, 0), 0.65, (x - 1.8, y + 1.4, 0), P['horn']))
    k += [B(-11, 10, -3.6, -3.6, 17, 3.6, P['back'], TEX, 6, tback=0.7),
          B(-18.5, 5.2, -2.5, -10.6, 11.6, 2.5, P['patch'], TEX, 6, tback=0.6, rz=0.35),
          B(-26, 1.0, -1.6, -18.0, 5.6, 1.6, P['back'], TEX, 6, tback=0.35, rz=0.2)]
    return k
DRAKE = {'Green': dict(back=0x5E7A3A, patch=0x3E5228, belly=0xD8CBA8, wing=0x8A9A4A, horn=0x3A2E24, eye=0xF0A030),
         'Red':   dict(back=0xA8322A, patch=0xD8702E, belly=0xE3CFA8, wing=0xE08A3A, horn=0x2E2A28, eye=0xFFD84A)}

def c_table(name, blocks):
    out = ['static const LpBlock %s[] = {' % name]
    for b in blocks:
        x0, y0, z0, x1, y1, z1 = b['b']
        out.append('  { %.2ff, %.2ff, %.2ff, %.2ff, %.2ff, %.2ff, %.2ff, %.2ff, %.4ff, 0x%06x, %d, %d },' %
                   (x0, y0, z0, x1, y1, z1, b['taper'], b['tback'], b['rz'], b['col'], b['limb'], b['mat']))
    out.append('};')
    return '\n'.join(out)
MAX_BLOCKS = 48
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HERO = os.path.join(HERE, 'hero.json')

def load_hero(path):
    """Hero blocks from a .glb (Blender glTF export, boxes only) or a hero.json (Blender add-on export)."""
    if path.lower().endswith('.glb'):
        if HERE not in sys.path: sys.path.insert(0, HERE)
        import glb_hero
        blocks = glb_hero.load(path, fallback=kestrel())
    else:
        with open(path) as f: blocks = json.load(f)['blocks']
        blocks = [dict(b=tuple(k['b']), col=int(k['col']), mat=int(k['mat']), limb=int(k['limb']),
                       taper=float(k.get('taper', 1.0)), tback=float(k.get('tback', 1.0)), rz=float(k.get('rz', 0.0)))
                  for k in blocks]
    if not blocks: raise SystemExit('%s: no blocks' % path)
    if len(blocks) > MAX_BLOCKS: raise SystemExit('%s: %d blocks; the game draws at most %d' % (path, len(blocks), MAX_BLOCKS))
    for k in blocks:
        if not 0 <= k['limb'] <= 6: raise SystemExit('%s: limb %d out of range 0-6' % (path, k['limb']))
    return blocks

def generate(out, hero=None):
    """Writes the header; hero = path to a .glb / .json to use instead of the built-in Kestrel."""
    hero_blocks = load_hero(hero) if hero else kestrel()
    parts = ['// Generated by emit.py: low-poly block models (model units, +X forward, +Y up, +Z toward the viewer).',
             '// limb: 0 body, 1/2 near/far leg, 3/4 near/far arm, 5 wings, 6 tail. mat: 0 lit + texture, 1 lit, 2 unlit.',
             '// Hero: %s\n' % (os.path.basename(hero) if hero else 'built-in Kestrel') + c_table('kKestrel', hero_blocks)]
    for v, P in DRAKE.items(): parts.append(c_table('kDrake' + v, drake(P)))
    with open(out, 'w', newline='\n') as f: f.write('\n\n'.join(parts) + '\n')
    return 'hero %s: %d blocks, drake: %d blocks' % (os.path.basename(hero) if hero else 'Kestrel',
                                                    len(hero_blocks), len(drake(DRAKE['Green'])))

if __name__ == '__main__':
    args = sys.argv[1:]
    usage = 'usage: emit.py OUT.h [--hero FILE.glb|FILE.json | --no-hero]'
    if not args or args[0].startswith('--'): raise SystemExit(usage)
    out, hero, rest = args[0], (DEFAULT_HERO if os.path.exists(DEFAULT_HERO) else None), args[1:]
    while rest:
        a = rest.pop(0)
        if a == '--no-hero': hero = None
        elif a == '--hero':
            if not rest: raise SystemExit(usage)
            hero = rest.pop(0)
            if not os.path.exists(hero): raise SystemExit('hero model not found: %s' % hero)
        else: raise SystemExit('unknown option %s\n%s' % (a, usage))
    print(generate(out, hero))

# Super Metroid 64: low-poly block model tools for Blender.
#
# Builds the hero (the character that replaces the player's sprite) out of
# boxes in Blender and writes it into the N64 port:
#   tools/lowpoly/hero.json  ->  src/platform/n64/lowpoly_models.h (kKestrel)
# Then run `make` as usual.
#
# Install: Blender > Edit > Preferences > Add-ons > Install from Disk, pick this
# file, tick "Super Metroid 64 Low-Poly Blocks". The panel is in the 3D view's
# sidebar (N key), tab "SM64 Blocks".
#
# Conventions (match lowpoly.c):
#   * 1 Blender unit = 1 model unit. Kestrel is 49 units tall, feet at Z = 0.
#   * The character faces +X. Front view (numpad 1) is the in-game side view.
#     The near side (toward the player) is -Y, the far side +Y.
#   * Every block is a box: move it, scale it, and rotate it about Y only.
#     Taper (top face size) and Back taper (the -X end face size) are block
#     properties in the panel. Don't edit a block's vertices.
#   * Legs swing about a hinge at Z = 24 (x = 0), arms at Z = 33.5, the tail at
#     Z = 14.4; a spin jump rolls the whole body about Z = 26. Build the model
#     around these (the add-on can show guides).
#
# The math (box <-> Blender transform, header parsing, checks) is plain Python
# at the top of this file, so it can be tested without Blender.

bl_info = {
    "name": "Super Metroid 64 Low-Poly Blocks",
    "author": "Claude, for the Super Metroid 64 N64 port",
    "version": (1, 0, 2),
    "blender": (3, 6, 0),
    "location": "3D View > Sidebar (N) > SM64 Blocks",
    "description": "Import, edit and export the N64 port's low-poly block models (lowpoly_models.h)",
    "category": "Import-Export",
}

import json
import math
import os
import re

# ---------------------------------------------------------------------------
# Plain-Python core
# ---------------------------------------------------------------------------

LIMBS = [("0", "Body", "Moves with the whole body"),
         ("1", "Near leg", "Swings about the hip (Z 24); near side = -Y"),
         ("2", "Far leg", "Swings about the hip (Z 24); far side = +Y"),
         ("3", "Near arm", "Swings about the shoulder (Z 33.5); aims shots. Near side = -Y"),
         ("4", "Far arm", "Swings about the shoulder (Z 33.5); far side = +Y"),
         ("5", "Wings", "Moves with the body"),
         ("6", "Tail", "Swings about Z 14.4")]
MATS = [("1", "Lit", "Flat colour, shaded by the light"),
        ("0", "Lit + texture", "Shaded, with the small scale texture"),
        ("2", "Glow", "Unlit: always full colour")]
PIVOTS = {"Hip (legs)": 24.0, "Shoulder (arms)": 33.5, "Tail": 14.4, "Spin (roll)": 26.0}
MAX_BLOCKS = 48                         # LP_MAXBLOCKS in lowpoly.c
HERO_SCALE = 0.86                       # LP_HERO_SCALE: screen pixels per model unit
TOP_LIMIT = 62 / HERO_SCALE             # rows the game redraws above the feet (LpHeroRows)
BOTTOM_LIMIT = -8 / HERO_SCALE
SPIN_RADIUS = 36 / HERO_SCALE           # spin jump: rows +-36 around 22 px above the feet
SPIN_CENTRE = 22 / HERO_SCALE


def model_to_blender(p):
    """Model (x fwd, y up, z toward viewer) -> Blender (x fwd, -y toward viewer, z up)."""
    return (p[0], -p[2], p[1])


def blender_to_model(p):
    return (p[0], p[2], -p[1])


def block_to_xform(b):
    """A block dict (as in emit.py / hero.json) -> Blender location, Y rotation, scale."""
    x0, y0, z0, x1, y1, z1 = b["b"]
    c = ((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2)
    h = ((x1 - x0) / 2, (y1 - y0) / 2, (z1 - z0) / 2)
    return model_to_blender(c), -b.get("rz", 0.0), (h[0], h[2], h[1])


def xform_to_block(loc, rot_y, scale, taper=1.0, tback=1.0, col=0x808080, limb=0, mat=1):
    """Inverse of block_to_xform (scale = half extents)."""
    cx, cy, cz = blender_to_model(loc)
    hx, hz, hy = (abs(s) for s in scale)
    return {"b": [cx - hx, cy - hy, cz - hz, cx + hx, cy + hy, cz + hz],
            "taper": taper, "tback": tback, "rz": -rot_y,
            "col": int(col) & 0xFFFFFF, "limb": int(limb), "mat": int(mat)}


def unit_box_verts(taper=1.0, tback=1.0):
    """The 8 corners of a block's mesh in Blender object space (half size 1),
    with taper / tback applied exactly like LpPrepare(). Corner i: bit 0 front
    (+X), bit 1 top (+model Y = Blender Z), bit 2 near (+model Z = Blender -Y)."""
    out = []
    for i in range(8):
        x = 1.0 if i & 1 else -1.0
        y = 1.0 if i & 2 else -1.0
        z = 1.0 if i & 4 else -1.0
        if i & 2:
            x *= taper; z *= taper
        if not i & 1:
            y *= tback; z *= tback
        out.append(model_to_blender((x, y, z)))
    return out


def _sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def _cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def _dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _faces():
    quads = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
    v = unit_box_verts()
    out = []
    for q in quads:                      # wind every face outward (counter-clockwise from outside)
        a, b, c, d = (v[k] for k in q)
        n = _cross(_sub(b, a), _sub(c, a))
        ctr = tuple((a[i] + c[i]) / 2 for i in range(3))
        out.append(q if _dot(n, ctr) > 0 else tuple(reversed(q)))
    return out


BOX_FACES = _faces()

_ROW = re.compile(r"\{\s*([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f,"
                  r"\s*([-\d.]+)f,\s*([-\d.]+)f,\s*([-\d.]+)f,\s*0x([0-9a-fA-F]+),\s*(\d+),\s*(\d+)\s*\}")


def parse_header(text):
    """lowpoly_models.h -> {table name: [block dict, ...]} in file order."""
    tables, cur = {}, None
    for line in text.splitlines():
        m = re.match(r"\s*static const LpBlock (\w+)\[\]", line)
        if m:
            cur = tables.setdefault(m.group(1), [])
            continue
        if cur is None:
            continue
        r = _ROW.search(line)
        if r:
            g = r.groups()
            f = [float(x) for x in g[:9]]
            cur.append({"b": f[:6], "taper": f[6], "tback": f[7], "rz": f[8],
                        "col": int(g[9], 16), "limb": int(g[10]), "mat": int(g[11])})
        elif line.strip().startswith("};"):
            cur = None
    return tables


def block_corners(b):
    """Model-space corners of a block, as lowpoly.c builds them (for the checks)."""
    x0, y0, z0, x1, y1, z1 = b["b"]
    mx, my, mz = (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2
    rc, rs = math.cos(b.get("rz", 0.0)), math.sin(b.get("rz", 0.0))
    out = []
    for i in range(8):
        x = x1 if i & 1 else x0
        y = y1 if i & 2 else y0
        z = z1 if i & 4 else z0
        if i & 2:
            x = mx + (x - mx) * b["taper"]; z = mz + (z - mz) * b["taper"]
        if not i & 1:
            y = my + (y - my) * b["tback"]; z = mz + (z - mz) * b["tback"]
        xx, yy = x - mx, y - my
        out.append((mx + xx * rc - yy * rs, my + xx * rs + yy * rc, z))
    return out


def check_hero(blocks):
    """Problems (errors) and things to look at (warnings) for a hero model."""
    errors, warnings = [], []
    n = len(blocks)
    if n == 0:
        errors.append("No blocks to export.")
    if n > MAX_BLOCKS:
        errors.append("%d blocks: the hero can have at most %d." % (n, MAX_BLOCKS))
    if not blocks:
        return errors, warnings
    ys = [c[1] for b in blocks for c in block_corners(b)]
    if max(ys) > TOP_LIMIT:
        warnings.append("Top is at %.1f units: the game only redraws up to %.0f above the feet, "
                        "so the top will be cut off." % (max(ys), TOP_LIMIT))
    if min(ys) < BOTTOM_LIMIT:
        warnings.append("Bottom is at %.1f: below %.1f gets cut off." % (min(ys), BOTTOM_LIMIT))
    if min(ys) > 2.0:
        warnings.append("Lowest point is at %.1f: the feet should touch Z = 0 or the hero floats." % min(ys))
    far = 0.0
    for b in blocks:
        for c in block_corners(b):
            far = max(far, math.hypot(c[0], c[1] - PIVOTS["Spin (roll)"]))
    if far > SPIN_RADIUS + 2.0:
        warnings.append("Spin jump: parts reach %.1f units from the roll centre (limit about %.0f); "
                        "they'll be clipped while spinning." % (far, SPIN_RADIUS))
    for limb, pivot, name in ((3, 33.5, "near arm"), (4, 33.5, "far arm")):
        reach = max((math.hypot(c[0], c[1] - pivot) for b in blocks if b["limb"] == limb
                     for c in block_corners(b)), default=0.0)
        if pivot + reach > TOP_LIMIT:
            warnings.append("The %s is %.1f long: aimed straight up it passes the top limit." % (name, reach))
    if not any(b["limb"] == 3 for b in blocks):
        warnings.append("No near-arm blocks (limb 3): nothing will point where shots go.")
    if not any(b["limb"] in (1, 2) for b in blocks):
        warnings.append("No leg blocks (limbs 1 / 2): nothing will walk.")
    tri = 0
    for b in blocks:
        tri += 12
    if tri > 400:
        warnings.append("About %d triangles before hidden faces are removed: may slow the game." % tri)
    return errors, warnings


def hex_to_rgb(col):
    return ((col >> 16 & 255) / 255.0, (col >> 8 & 255) / 255.0, (col & 255) / 255.0)


def rgb_to_hex(rgb):
    return (int(round(max(0.0, min(1.0, rgb[0])) * 255)) << 16 |
            int(round(max(0.0, min(1.0, rgb[1])) * 255)) << 8 |
            int(round(max(0.0, min(1.0, rgb[2])) * 255)))


def project_paths(root):
    root = os.path.abspath(bpy_path(root)) if root else ""
    return {"root": root,
            "header": os.path.join(root, "src", "platform", "n64", "lowpoly_models.h"),
            "emit": os.path.join(root, "tools", "lowpoly", "emit.py"),
            "hero": os.path.join(root, "tools", "lowpoly", "hero.json")}


def bpy_path(p):
    try:
        import bpy
        return bpy.path.abspath(p)
    except ImportError:
        return p


def load_emit(emit_path):
    """emit.py from the project, as a module (Blender's own Python runs it)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("sm64_lowpoly_emit", emit_path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    if not hasattr(mod, "generate"):
        raise RuntimeError("This emit.py is the old version without hero support. "
                           "Copy the new tools/lowpoly/emit.py into the project.")
    return mod


def write_hero(blocks, paths, name="hero"):
    """Writes hero.json and regenerates lowpoly_models.h with the project's emit.py."""
    with open(paths["hero"], "w", newline="\n") as f:
        json.dump({"name": name, "units": "model", "blocks": blocks}, f, indent=1)
    emit = load_emit(paths["emit"])
    return emit.generate(paths["header"], paths["hero"])


# ---------------------------------------------------------------------------
# Blender part
# ---------------------------------------------------------------------------

try:
    import bpy
    from bpy.props import (BoolProperty, EnumProperty, FloatProperty, FloatVectorProperty,
                           PointerProperty, StringProperty)
    from mathutils import Vector
except ImportError:                     # outside Blender (tests)
    bpy = None

if bpy is not None:

    def _set_mesh(obj):
        """Rebuilds the block's 8 vertices from its taper / tback."""
        me = obj.data
        if me.users > 1:                 # linked duplicate: give this block its own mesh
            obj.data = me = me.copy()
        verts = unit_box_verts(obj.sm64_block.taper, obj.sm64_block.tback)
        if len(me.vertices) != 8 or len(me.polygons) != 6:
            me.clear_geometry()
            me.from_pydata(verts, [], BOX_FACES)
        else:
            for v, co in zip(me.vertices, verts):
                v.co = co
        me.update()

    def _on_shape(self, context):
        obj = self.id_data
        if obj and obj.type == 'MESH' and self.is_block:
            _set_mesh(obj)

    def _on_colour(self, context):
        obj = self.id_data
        if obj:
            obj.color = (self.color[0], self.color[1], self.color[2], 1.0)

    class SM64BlockProps(bpy.types.PropertyGroup):
        is_block: BoolProperty(name="Is block", default=False)
        limb: EnumProperty(name="Limb", items=LIMBS, default="0")
        mat: EnumProperty(name="Material", items=MATS, default="1")
        taper: FloatProperty(name="Taper", description="Size of the top face (1 = straight box)",
                             default=1.0, min=0.01, max=4.0, step=5, update=_on_shape)
        tback: FloatProperty(name="Back taper", description="Size of the back (-X) end face",
                             default=1.0, min=0.01, max=4.0, step=5, update=_on_shape)
        color: FloatVectorProperty(name="Colour", subtype='COLOR_GAMMA', size=3, min=0.0, max=1.0,
                                   default=(0.5, 0.5, 0.5), update=_on_colour)

    _TABLE_ITEMS = []                    # Blender needs the enum strings kept alive

    def _scene_tables(self, context):
        paths = project_paths(self.project)
        items = []
        if os.path.exists(paths["header"]):
            try:
                with open(paths["header"]) as f:
                    items = [(n, n, "Model table %s in lowpoly_models.h" % n) for n in parse_header(f.read())]
            except OSError:
                items = []
        _TABLE_ITEMS[:] = items or [("kKestrel", "kKestrel", "")]
        return _TABLE_ITEMS

    class SM64SceneProps(bpy.types.PropertyGroup):
        project: StringProperty(name="Project folder", subtype='DIR_PATH',
                                description="The sm_n64_latest_source folder")
        table: EnumProperty(name="Model", description="Which model to import", items=_scene_tables)

    def _blocks_in(coll):
        return [o for o in coll.all_objects if o.type == 'MESH' and o.sm64_block.is_block]

    def _model_collection(context):
        obj = context.active_object
        if obj and obj.sm64_block.is_block and obj.users_collection:
            return obj.users_collection[0]
        return context.collection

    def _shade_by_object_colour(context):
        for area in context.screen.areas if context.screen else []:
            if area.type == 'VIEW_3D':
                for space in area.spaces:
                    if space.type == 'VIEW_3D':
                        space.shading.color_type = 'OBJECT'
                        space.clip_end = max(space.clip_end, 1000.0)

    def make_block_object(context, coll, block, name):
        me = bpy.data.meshes.new(name)
        me.from_pydata(unit_box_verts(block["taper"], block["tback"]), [], BOX_FACES)
        me.update()
        obj = bpy.data.objects.new(name, me)
        coll.objects.link(obj)
        loc, rot_y, scale = block_to_xform(block)
        obj.location = loc
        obj.rotation_mode = 'XYZ'
        obj.rotation_euler = (0.0, rot_y, 0.0)
        obj.scale = scale
        p = obj.sm64_block
        p.is_block = True
        p.limb = str(block["limb"])
        p.mat = str(block["mat"])
        p.taper = block["taper"]         # (rebuilds the mesh: same corners)
        p.tback = block["tback"]
        p.color = hex_to_rgb(block["col"])
        return obj

    def read_block(obj):
        """The block an object stands for, in model units, or raises ValueError."""
        mw = obj.matrix_world
        if mw.determinant() <= 0:
            raise ValueError("%s is mirrored (negative scale). Use Mirror to far side instead." % obj.name)
        loc, rot, scale = mw.decompose()
        ax, ay = rot @ Vector((1, 0, 0)), rot @ Vector((0, 1, 0))
        if abs(ay.x) > 1e-3 or abs(ay.z) > 1e-3 or ay.y < 0.999:
            raise ValueError("%s is rotated about X or Z. Blocks may only turn about Y." % obj.name)
        rot_y = math.atan2(-ax.z, ax.x)
        p = obj.sm64_block
        # the mesh must still be the generated box (edited vertices aren't exported)
        want = unit_box_verts(p.taper, p.tback)
        me = obj.data
        if len(me.vertices) != 8 or any((Vector(w) - v.co).length > 1e-3 for w, v in zip(want, me.vertices)):
            raise ValueError("%s's vertices were edited. Use Make Block on it to turn it back into a box." % obj.name)
        return xform_to_block(tuple(loc), rot_y, tuple(scale), p.taper, p.tback,
                              rgb_to_hex(p.color), int(p.limb), int(p.mat))

    # ---- operators ----------------------------------------------------------
    class SM64_OT_import(bpy.types.Operator):
        bl_idname = "sm64blocks.import_model"
        bl_label = "Import Model"
        bl_description = "Load the model chosen above from the project's lowpoly_models.h as editable blocks"
        bl_options = {'REGISTER', 'UNDO'}

        def execute(self, context):
            paths = project_paths(context.scene.sm64_blocks.project)
            if not os.path.exists(paths["header"]):
                self.report({'ERROR'}, "Set the project folder first (lowpoly_models.h not found).")
                return {'CANCELLED'}
            with open(paths["header"]) as f:
                tables = parse_header(f.read())
            table = context.scene.sm64_blocks.table
            blocks = tables.get(table)
            if not blocks:
                self.report({'ERROR'}, "No table %s in lowpoly_models.h." % table)
                return {'CANCELLED'}
            coll = bpy.data.collections.new(table)
            context.scene.collection.children.link(coll)
            made = [make_block_object(context, coll, b, "%s_%02d_%s" % (table, i, LIMBS[b["limb"]][1].replace(" ", "")))
                    for i, b in enumerate(blocks)]
            for o in context.selected_objects:
                o.select_set(False)
            made[0].select_set(True)
            context.view_layer.objects.active = made[0]
            _add_guides(context)
            _shade_by_object_colour(context)
            self.report({'INFO'}, "Imported %d blocks into collection %s." % (len(blocks), coll.name))
            return {'FINISHED'}

    def _add_guides(context):
        name = "SM64 pivot guides"
        coll = bpy.data.collections.get(name)
        if coll is None:
            coll = bpy.data.collections.new(name)
            context.scene.collection.children.link(coll)
        for label, z in PIVOTS.items():
            ob_name = "Pivot: " + label
            if bpy.data.objects.get(ob_name):
                continue
            e = bpy.data.objects.new(ob_name, None)
            e.empty_display_type = 'SPHERE' if label.startswith("Spin") else 'PLAIN_AXES'
            e.empty_display_size = 3.0
            e.location = (0.0, 0.0, z)
            e.hide_select = True
            coll.objects.link(e)
        if not bpy.data.objects.get("Ground (feet)"):
            e = bpy.data.objects.new("Ground (feet)", None)
            e.empty_display_type = 'SINGLE_ARROW'
            e.empty_display_size = 10.0
            e.hide_select = True
            coll.objects.link(e)

    class SM64_OT_guides(bpy.types.Operator):
        bl_idname = "sm64blocks.guides"
        bl_label = "Show Pivot Guides"
        bl_description = "Markers at the hip, shoulder, tail and spin pivots"
        bl_options = {'REGISTER', 'UNDO'}

        def execute(self, context):
            _add_guides(context)
            return {'FINISHED'}

    class SM64_OT_add(bpy.types.Operator):
        bl_idname = "sm64blocks.add_block"
        bl_label = "Add Block"
        bl_description = "Add a new box at the 3D cursor, in the model's collection"
        bl_options = {'REGISTER', 'UNDO'}

        def execute(self, context):
            coll = _model_collection(context)
            c = context.scene.cursor.location
            blk = xform_to_block(tuple(c), 0.0, (2.0, 2.0, 2.0), col=0x3FA34D)
            act = context.active_object
            if act and act.sm64_block.is_block:   # copy the active block's settings
                blk["limb"], blk["mat"] = int(act.sm64_block.limb), int(act.sm64_block.mat)
                blk["col"] = rgb_to_hex(act.sm64_block.color)
            obj = make_block_object(context, coll, blk, "Block")
            for o in context.selected_objects:
                o.select_set(False)
            obj.select_set(True)
            context.view_layer.objects.active = obj
            _shade_by_object_colour(context)
            return {'FINISHED'}

    class SM64_OT_make(bpy.types.Operator):
        bl_idname = "sm64blocks.make_block"
        bl_label = "Make Block"
        bl_description = ("Turn the selected mesh objects into blocks: each becomes the box around "
                          "its vertices (rotation about Y is kept)")
        bl_options = {'REGISTER', 'UNDO'}

        def execute(self, context):
            done = 0
            for obj in context.selected_objects:
                if obj.type != 'MESH' or not obj.data.vertices:
                    continue
                mw = obj.matrix_world.copy()
                loc, rot, scale = mw.decompose()
                ax = rot @ Vector((1, 0, 0))
                rot_y = math.atan2(-ax.z, ax.x)
                # box around the vertices, measured in the block's own axes
                inv = (mathutils_rot_y(rot_y)).inverted()
                pts = [inv @ (mw @ v.co) for v in obj.data.vertices]
                lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
                hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
                centre = mathutils_rot_y(rot_y) @ ((lo + hi) / 2)
                half = (hi - lo) / 2
                if min(half) < 1e-4:
                    self.report({'WARNING'}, "%s is flat; skipped." % obj.name)
                    continue
                me = obj.data
                if me.users > 1:
                    obj.data = me = me.copy()
                p = obj.sm64_block
                p.taper, p.tback = 1.0, 1.0
                obj.parent = None
                obj.rotation_mode = 'XYZ'
                obj.location, obj.rotation_euler, obj.scale = centre, (0.0, rot_y, 0.0), half
                for mod in list(obj.modifiers):
                    obj.modifiers.remove(mod)
                me.clear_geometry()
                me.from_pydata(unit_box_verts(), [], BOX_FACES)
                me.update()
                if not p.is_block:
                    p.is_block = True
                    p.color = tuple(obj.color[:3])
                done += 1
            self.report({'INFO'}, "%d block(s) made." % done)
            return {'FINISHED'}

    def mathutils_rot_y(angle):
        from mathutils import Matrix
        return Matrix.Rotation(angle, 4, 'Y')

    class SM64_OT_mirror(bpy.types.Operator):
        bl_idname = "sm64blocks.mirror"
        bl_label = "Mirror to Far Side"
        bl_description = ("Copy the selected blocks to the other side (Y flipped); near legs / arms "
                          "become far legs / arms and the other way round")
        bl_options = {'REGISTER', 'UNDO'}

        def execute(self, context):
            swap = {"1": "2", "2": "1", "3": "4", "4": "3"}
            made = []
            for obj in [o for o in context.selected_objects if o.type == 'MESH' and o.sm64_block.is_block]:
                try:
                    b = read_block(obj)
                except ValueError as e:
                    self.report({'ERROR'}, str(e))
                    return {'CANCELLED'}
                x0, y0, z0, x1, y1, z1 = b["b"]
                b["b"] = [x0, y0, -z1, x1, y1, -z0]          # model z (Blender -Y) flipped; rz unchanged
                b["limb"] = int(swap.get(str(b["limb"]), str(b["limb"])))
                coll = obj.users_collection[0] if obj.users_collection else context.collection
                made.append(make_block_object(context, coll, b, obj.name + ".mirror"))
            for o in context.selected_objects:
                o.select_set(False)
            for o in made:
                o.select_set(True)
            self.report({'INFO'}, "%d block(s) mirrored." % len(made))
            return {'FINISHED'}

    def _collect(context):
        coll = _model_collection(context)
        objs = sorted(_blocks_in(coll), key=lambda o: o.name)
        blocks = [read_block(o) for o in objs]
        return coll, blocks

    class SM64_OT_check(bpy.types.Operator):
        bl_idname = "sm64blocks.check"
        bl_label = "Check Model"
        bl_description = "Count the blocks and check sizes against what the game can draw"

        def execute(self, context):
            try:
                coll, blocks = _collect(context)
            except ValueError as e:
                self.report({'ERROR'}, str(e))
                return {'CANCELLED'}
            errors, warnings = check_hero(blocks)
            for w in warnings:
                self.report({'WARNING'}, w)
            for e in errors:
                self.report({'ERROR'}, e)
            if not errors:
                self.report({'INFO'}, "%s: %d blocks, %s." % (coll.name, len(blocks),
                                                              "looks good" if not warnings else "see warnings"))
            return {'FINISHED'}

    class SM64_OT_export(bpy.types.Operator):
        bl_idname = "sm64blocks.export_hero"
        bl_label = "Export as Hero"
        bl_description = ("Write this collection as the hero (replaces Kestrel): tools/lowpoly/hero.json and "
                          "src/platform/n64/lowpoly_models.h. Then run make")

        def execute(self, context):
            paths = project_paths(context.scene.sm64_blocks.project)
            if not os.path.exists(paths["emit"]):
                self.report({'ERROR'}, "Set the project folder first (tools/lowpoly/emit.py not found).")
                return {'CANCELLED'}
            try:
                coll, blocks = _collect(context)
            except ValueError as e:
                self.report({'ERROR'}, str(e))
                return {'CANCELLED'}
            errors, warnings = check_hero(blocks)
            if errors:
                for e in errors:
                    self.report({'ERROR'}, e)
                return {'CANCELLED'}
            try:
                n, _ = write_hero(blocks, paths, coll.name)
            except Exception as e:
                self.report({'ERROR'}, "Export failed: %s" % e)
                return {'CANCELLED'}
            for w in warnings:
                self.report({'WARNING'}, w)
            self.report({'INFO'}, "Exported %d blocks as the hero. Now run make." % n)
            return {'FINISHED'}

    class SM64_OT_restore(bpy.types.Operator):
        bl_idname = "sm64blocks.restore_kestrel"
        bl_label = "Restore Kestrel"
        bl_description = "Delete tools/lowpoly/hero.json and regenerate lowpoly_models.h with the original Kestrel"

        def invoke(self, context, event):
            return context.window_manager.invoke_confirm(self, event)

        def execute(self, context):
            paths = project_paths(context.scene.sm64_blocks.project)
            if not os.path.exists(paths["emit"]):
                self.report({'ERROR'}, "Set the project folder first.")
                return {'CANCELLED'}
            if os.path.exists(paths["hero"]):
                os.remove(paths["hero"])
            load_emit(paths["emit"]).generate(paths["header"], None)
            self.report({'INFO'}, "Kestrel restored. Run make.")
            return {'FINISHED'}

    # ---- panel --------------------------------------------------------------
    class SM64_PT_panel(bpy.types.Panel):
        bl_label = "Super Metroid 64 Blocks"
        bl_space_type = 'VIEW_3D'
        bl_region_type = 'UI'
        bl_category = "SM64 Blocks"

        def draw(self, context):
            lay = self.layout
            sp = context.scene.sm64_blocks
            lay.prop(sp, "project")
            row = lay.row(align=True)
            row.prop(sp, "table", text="")
            row.operator("sm64blocks.import_model", icon='IMPORT')
            row.operator("sm64blocks.guides", text="", icon='EMPTY_AXIS')

            box = lay.box()
            box.label(text="Blocks", icon='MESH_CUBE')
            row = box.row(align=True)
            row.operator("sm64blocks.add_block", icon='ADD')
            row.operator("sm64blocks.make_block")
            box.operator("sm64blocks.mirror", icon='MOD_MIRROR')
            obj = context.active_object
            if obj and obj.type == 'MESH' and obj.sm64_block.is_block:
                p = obj.sm64_block
                col = box.column()
                col.label(text=obj.name)
                col.prop(p, "limb")
                col.prop(p, "mat")
                col.prop(p, "color")
                col.prop(p, "taper")
                col.prop(p, "tback")
                col.label(text="Size: scale = half the box (S), turn: R Y", icon='INFO')
            elif obj and obj.type == 'MESH':
                box.label(text="Not a block: use Make Block", icon='INFO')

            box = lay.box()
            coll = _model_collection(context)
            n = len(_blocks_in(coll)) if coll else 0
            box.label(text="%s: %d / %d blocks" % (coll.name if coll else "-", n, MAX_BLOCKS), icon='OUTLINER_COLLECTION')
            box.operator("sm64blocks.check", icon='CHECKMARK')
            box.operator("sm64blocks.export_hero", icon='EXPORT')
            box.operator("sm64blocks.restore_kestrel", icon='LOOP_BACK')

    classes = (SM64BlockProps, SM64SceneProps, SM64_OT_import, SM64_OT_guides, SM64_OT_add, SM64_OT_make,
               SM64_OT_mirror, SM64_OT_check, SM64_OT_export, SM64_OT_restore, SM64_PT_panel)

    def register():
        for c in classes:
            bpy.utils.register_class(c)
        bpy.types.Object.sm64_block = PointerProperty(type=SM64BlockProps)
        bpy.types.Scene.sm64_blocks = PointerProperty(type=SM64SceneProps)

    def unregister():
        del bpy.types.Scene.sm64_blocks
        del bpy.types.Object.sm64_block
        for c in reversed(classes):
            bpy.utils.unregister_class(c)

    if __name__ == "__main__":
        register()

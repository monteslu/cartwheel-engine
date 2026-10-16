# Export a terrain from Blender for a Terrain3D node: heights as a raw .r16
# and, when the mesh has a colour attribute, a splat image.
#
#   blender --background scene.blend --factory-startup --python tools/blender/export_terrain.py -- \
#       <out dir> [--object NAME] [--samples N] [--min H] [--max H]
#
# (Flatpak: flatpak run org.blender.Blender --background ...; give absolute paths.)
#
# The object (default: the mesh with the largest footprint) is sampled
# straight down from above on an N x N grid over its bounding box in world
# space (default 1025, a 1024-quad terrain), with modifiers applied (sculpt,
# displace, geometry nodes). Blender's Z is up, the engine's Y; Blender's
# X is the terrain's x and Blender's -Y its z, so the top-down view of the
# file matches the terrain seen from above with z toward the bottom.
#
# Writes into <out dir>:
#   heights.r16   N x N little-endian uint16, row 0 = the far (+Y) edge: the
#                 heights mapped from --min..--max (default: the object's own
#                 range) onto 0..65535. Set the node's min_height and
#                 max_height to the same range (the script prints them) and
#                 size_x / size_z to N - 1 with vertex_spacing = extent / (N - 1).
#   splat.png     when the mesh has a colour attribute: its RGBA at each
#                 sample, the weights of layers 0..3 (Terrain3D-style splat;
#                 cw_terrain_import_splat, or the node's `splat`).
#   terrain.txt   the Terrain3D properties to paste into a .cwscene.
import os
import struct
import sys

import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not argv:
    sys.exit("usage: export_terrain.py -- <out dir> [--object NAME] [--samples N] [--min H] [--max H]")
out = os.path.abspath(argv[0])
opt = lambda k, d=None: argv[argv.index(k) + 1] if k in argv else d
n = int(opt("--samples", "1025"))
os.makedirs(out, exist_ok=True)

deps = bpy.context.evaluated_depsgraph_get()
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
if not meshes:
    sys.exit("export_terrain.py: no mesh in the file")
name = opt("--object")
if name:
    obj = bpy.context.scene.objects.get(name)
    if not obj or obj.type != "MESH":
        sys.exit(f"export_terrain.py: no mesh named {name}")
else:
    def footprint(o):
        c = [o.matrix_world @ Vector(b) for b in o.bound_box]
        return (max(v.x for v in c) - min(v.x for v in c)) * (max(v.y for v in c) - min(v.y for v in c))
    obj = max(meshes, key=footprint)

ev = obj.evaluated_get(deps)
mesh = ev.to_mesh()
mesh.transform(obj.matrix_world)
verts = [v.co.copy() for v in mesh.vertices]
polys = [list(p.vertices) for p in mesh.polygons]
bvh = BVHTree.FromPolygons(verts, polys)
x0, x1 = min(v.x for v in verts), max(v.x for v in verts)
y0, y1 = min(v.y for v in verts), max(v.y for v in verts)
z0, z1 = min(v.z for v in verts), max(v.z for v in verts)
lo = float(opt("--min", z0))
hi = float(opt("--max", z1))
if hi <= lo:
    sys.exit("export_terrain.py: the height range is empty (flat mesh? give --min and --max)")

# a colour attribute, read per polygon corner through the hit polygon (nearest corner)
col = mesh.color_attributes.active_color if mesh.color_attributes else None

heights = bytearray()
splat = [] if col else None
top = z1 + 1.0
for j in range(n):
    # row 0 = the far (+Y) edge: the engine's z grows toward Blender's -Y
    y = y1 - (y1 - y0) * j / (n - 1)
    for i in range(n):
        x = x0 + (x1 - x0) * i / (n - 1)
        hit, _, index, _ = bvh.ray_cast(Vector((x, y, top)), Vector((0, 0, -1)))
        z = hit.z if hit else lo
        q = round((min(max(z, lo), hi) - lo) / (hi - lo) * 65535)
        heights += struct.pack("<H", q)
        if splat is not None:
            rgba = (0.0, 0.0, 0.0, 0.0)
            if hit:
                p = mesh.polygons[index]
                best, bd = None, None
                for li in p.loop_indices:
                    d = (mesh.vertices[mesh.loops[li].vertex_index].co - hit).length
                    if bd is None or d < bd:
                        best, bd = li, d
                c = col.data[best if col.domain == "CORNER" else mesh.loops[best].vertex_index].color
                rgba = tuple(c)
            splat.extend(rgba)

with open(os.path.join(out, "heights.r16"), "wb") as f:
    f.write(heights)
if splat is not None:
    img = bpy.data.images.new("splat", n, n, alpha=True, float_buffer=False)
    img.colorspace_settings.name = "Non-Color"
    # Blender images run bottom-up: flip the rows so the file's row 0 is the far edge
    rows = [splat[r * n * 4:(r + 1) * n * 4] for r in range(n)]
    img.pixels = [v for r in reversed(rows) for v in r]
    img.filepath_raw = os.path.join(out, "splat.png")
    img.file_format = "PNG"
    img.save()
spacing = (x1 - x0) / (n - 1)
if abs((y1 - y0) - (x1 - x0)) > 1e-3 * (x1 - x0):
    print(f"export_terrain.py: the footprint is not square ({x1 - x0:.3f} x {y1 - y0:.3f}): z samples are {(y1 - y0) / (n - 1):.4f} apart")
props = [f"size_x = {n - 1}", f"size_z = {n - 1}", f"vertex_spacing = {spacing:.6g}", f"min_height = {lo:.6g}",
         f"max_height = {hi:.6g}", 'heightmap = "heights.r16"'] + (['splat = "splat.png"'] if splat is not None else [])
with open(os.path.join(out, "terrain.txt"), "w") as f:
    f.write("\n".join(props) + "\n")
print(f"export_terrain.py: {obj.name}: {n} x {n} samples, {spacing:.4g} m apart, heights {lo:.4g}..{hi:.4g} -> {out}")
ev.to_mesh_clear()

# Render a model from several angles into one contact sheet, to check a
# model by eye without opening Blender.
#
#   blender --background --factory-startup --python tools/blender/render_views.py -- \
#       model.glb out.png [--size 512]
#
# (Flatpak: flatpak run org.blender.Blender --background ...)
#
# Imports .glb/.gltf, .fbx, .obj or opens a .blend, frames the meshes, lights
# them with a sun and a soft world, and renders front, right, back, top and two
# three-quarter views with EEVEE, tiled 3x2 into out.png.
import math
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(argv) < 2:
    sys.exit("usage: render_views.py -- <model> <out.png> [--size N]")
src, out = os.path.abspath(argv[0]), os.path.abspath(argv[1])
size = int(argv[argv.index("--size") + 1]) if "--size" in argv else 512

ext = os.path.splitext(src)[1].lower()
if ext == ".blend":
    bpy.ops.wm.open_mainfile(filepath=src)
else:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=src)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=src)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=src)
    else:
        sys.exit(f"render_views.py: can't load {ext} files")

scene = bpy.context.scene
meshes = [o for o in scene.objects if o.type == "MESH" and o.visible_get()]
if not meshes:
    sys.exit("render_views.py: no visible meshes")

# bounds of everything visible, in world space
lo = Vector((math.inf,) * 3)
hi = Vector((-math.inf,) * 3)
for o in meshes:
    for c in o.bound_box:
        p = o.matrix_world @ Vector(c)
        lo = Vector(map(min, lo, p))
        hi = Vector(map(max, hi, p))
center = (lo + hi) / 2
radius = max((hi - lo).length / 2, 1e-3)

engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items} else "BLENDER_EEVEE"
scene.render.engine = engine
scene.render.resolution_x = scene.render.resolution_y = size
scene.render.film_transparent = False
scene.render.image_settings.file_format = "PNG"

world = scene.world or bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes.get("Background")
bg.inputs[0].default_value = (0.32, 0.34, 0.38, 1)
bg.inputs[1].default_value = 0.8

sun = bpy.data.objects.new("check_sun", bpy.data.lights.new("check_sun", "SUN"))
sun.data.energy = 3
sun.rotation_euler = (math.radians(50), 0, math.radians(30))
scene.collection.objects.link(sun)

cam = bpy.data.objects.new("check_cam", bpy.data.cameras.new("check_cam"))
cam.data.lens = 50
scene.collection.objects.link(cam)
scene.camera = cam
dist = radius / math.tan(cam.data.angle / 2) * 1.15

# (azimuth, elevation) in degrees; glTF imports are Y-up turned Z-up, front = -Y
views = [("front", 0, 10), ("right", 90, 10), ("back", 180, 10),
         ("top", 0, 89), ("3/4 front", 35, 25), ("3/4 back", 215, 25)]
tiles = []
for i, (name, az, el) in enumerate(views):
    a, e = math.radians(az), math.radians(el)
    cam.location = center + Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e))) * dist
    cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
    path = f"{out}.view{i}.png"
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    tiles.append(path)

# tile the views 3x2 into the contact sheet
sheet = bpy.data.images.new("sheet", size * 3, size * 2)
px = [0.0] * (size * 3 * size * 2 * 4)
for i, path in enumerate(tiles):
    img = bpy.data.images.load(path)
    src_px = list(img.pixels)
    ox, oy = (i % 3) * size, (1 - i // 3) * size  # Blender images start at the bottom row
    for y in range(size):
        row = (oy + y) * size * 3 + ox
        px[row * 4:(row + size) * 4] = src_px[y * size * 4:(y + 1) * size * 4]
    bpy.data.images.remove(img)
    os.remove(path)
sheet.pixels = px
sheet.filepath_raw = out
sheet.file_format = "PNG"
sheet.save()
print(f"render_views: {out} ({len(meshes)} meshes, radius {radius:.3f})")

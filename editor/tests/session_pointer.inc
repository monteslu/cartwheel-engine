/* Gate editor-pointer (app platformer3d_editor_pointer, pointer input
 * tests/editor-pointer.json): a 2D scene built by commands, then edited by
 * the POINTER only: a polygon point dragged in the 2D viewport (snapped to
 * the 8 px grid), a scene tree row dragged onto another (reparent), a
 * ctrl+click multi-selection in the tree. { frame, command line } */
{ 2, "scene.new type=Node2D name=Sketch" },
{ 3, "node.add type=CanvasSettings parent=. name=Settings" },
{ 4, "node.set path=Settings prop=virtual_width value=480" },
{ 5, "node.set path=Settings prop=virtual_height value=270" },
{ 6, "node.add type=StaticBody2D parent=. name=Ground" },
{ 7, "node.set path=Ground prop=position value=vec2(0, 220)" },
{ 8, "node.add type=CollisionShape2D parent=Ground name=Shape" },
{ 9, "node.set path=Ground/Shape prop=shape value=POLYGON" },
{ 10, "node.set path=Ground/Shape prop=points value=\"0 0, 300 0, 360 -40, 480 -40, 480 50, 0 50\"" },
{ 11, "node.add type=Polygon2D parent=Ground name=Fill" },
{ 12, "node.set path=Ground/Fill prop=polygon value=\"0 0, 300 0, 360 -40, 480 -40, 480 50, 0 50\"" },
{ 13, "node.set path=Ground/Fill prop=color value=color(0.25, 0.45, 0.2, 1)" },
{ 14, "node.add type=Line2D parent=. name=Rope" },
{ 15, "node.set path=Rope prop=points value=\"200 40, 240 90, 300 100\"" },
{ 16, "node.set path=Rope prop=width value=3" },
{ 17, "node.add type=Camera2D parent=. name=Cam" },
{ 18, "node.set path=Cam prop=position value=vec2(240, 135)" },
{ 19, "node.add type=Light2D parent=. name=Lamp" },
{ 22, "view2d.camera center=vec2(240, 140) zoom=1" },
{ 23, "view2d.tool tool=select" },
{ 24, "view2d.snap on=true step=8" },
{ 25, "select paths=Ground/Shape" },
{ 26, "panel.show id=viewport2d" },
{ 160, "node.rename path=NoSuch name=X" },

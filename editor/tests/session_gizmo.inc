/* Gate editor-gizmo (app platformer3d_editor_gizmo, pointer input
 * tests/editor-gizmo.json): level 1 with Coin1 taken out and instanced
 * again at the origin; the pointer drags the move gizmo's Y then Z handle
 * with snapping until it sits where the hand-written level has it, so the
 * document is the level again (editor.doc_hash = the level's). The 3D view
 * is set first so the handles are where the input script presses. */
{ 2, "scene.open path=scenes/level1.cwscene" },
{ 3, "node.delete path=Coin1" },
{ 4, "node.instance scene=scenes/coin.cwscene parent=. name=Coin1 index=5" },
{ 5, "select paths=Coin1" },
{ 6, "view3d.camera target=vec3(0, 0.5, -0.5) yaw=60 pitch=-15 distance=6" },
{ 7, "view3d.tool tool=move" },
{ 8, "view3d.snap on=true move=0.5" },
{ 9, "view3d.local on=false" },
{ 10, "panel.show id=viewport3d" },
{ 120, "node.delete path=NoSuchNode" },

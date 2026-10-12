/* Triggers: data-driven "when X then Y" rules stored in scenes (the tier-3
 * layer of SUBSYSTEMS 8.3 that needs no script VM). A Trigger is a node:
 *
 *   [node name="OnGoal" type="Trigger" parent="Goal"]
 *   signal = "body_entered"
 *   filter_group = "player"
 *   action = SET
 *   target = "../Lamp"
 *   member = "color"
 *   value = "color(0.2, 1, 0.3, 1)"
 *
 * reads "when Goal emits body_entered with a node of group player, set
 * Lamp.color". Being a node, a trigger round-trips through .cwscene text,
 * shows in the editor's scene tree and inspector, undoes like any edit and
 * is readable and writable by an agent as plain text.
 *
 * Semantics (deliberately small):
 *   - source (default "..", the trigger's parent): the node whose `signal`
 *     fires the rule. Paths are relative to the Trigger node ("..",
 *     "../Lamp", "/root/Level/Door", "%Unique").
 *   - The rule connects when the trigger becomes ready (its source is
 *     resolved then; a missing source or a signal the source's type does not
 *     declare is a warning naming the fix) and disconnects when it leaves the
 *     tree. Changing source/signal/enabled at runtime reconnects.
 *   - Firing is DEFERRED: actions run at the end of the phase that emitted
 *     the signal, in emission order, so a rule may free, spawn or change the
 *     scene safely from a physics callback.
 *   - filter_group: fire only when the signal's first argument is a node in
 *     that group (body_entered from the player, not from a crate). Empty =
 *     always.
 *   - once: fire at most once per trigger life (a pickup, a cutscene).
 *   - enabled: false = never fire (another trigger may SET it true).
 *   - Several triggers on one signal fire in tree order; one trigger does one
 *     action (chain more by adding triggers, or EMIT a signal another trigger
 *     listens to).
 *   - Actions (member and value are text; target defaults to ".."):
 *       SET           target.member = value (parsed for the property: enums
 *                     by name, "color(1, 0, 0, 1)", vec2(...), 3.5, true, text)
 *       CALL          the method `member` of target (cw_node_type_add_method or
 *                     the method resolver); with value empty the signal's
 *                     arguments are passed, else value parsed as the method's
 *                     first argument type
 *       EMIT          target emits signal `member` with the signal's arguments
 *       SPAWN         instance the scene file `value` as a child of target
 *                     (member, when set, names the new node)
 *       FREE          free target (and its subtree)
 *       CHANGE_SCENE  the scene app's main scene becomes `value`
 *   - fired (read-only, not saved): how many times the rule ran; the debug
 *     channel reads it (`get <trigger> fired`).
 * Anything per entity or per frame belongs in C (tier 1/2): triggers are for
 * the handful of "when this happens, do that" rules a human tweaks. */
#ifndef CW_TRIGGERS_H
#define CW_TRIGGERS_H

#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CW_TRIGGER_SET, CW_TRIGGER_CALL, CW_TRIGGER_EMIT, CW_TRIGGER_SPAWN, CW_TRIGGER_FREE, CW_TRIGGER_CHANGE_SCENE
} cw_trigger_action;

/* Run a Trigger node's action now as if its signal fired with args (the
 * filter, once and enabled rules apply). false when it did not run (or the
 * action failed; the log says why). The editor's "test fire" uses it. */
bool cw_trigger_fire(cw_node trigger, const cw_value *args, int argc);

/* Registers the Trigger node type (cw_scene_register_builtin_types calls it). */
void cw_triggers_register(void);

#ifdef __cplusplus
}
#endif
#endif

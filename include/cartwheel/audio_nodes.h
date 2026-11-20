/* Audio nodes: sounds placed in a scene, over the mixer of cartwheel/audio.h.
 *
 *   AudioStreamPlayer    (Node)   plays a sound as it is: music, UI, anything without a place
 *   AudioStreamPlayer2D  (Node2D) pans and fades it by where the node is on the 2D view
 *   AudioStreamPlayer3D  (Node3D) a spatial voice at the node: distance, cone, doppler, reverb zones
 *   AudioListener3D      (Node3D) the ears. With none in the tree, the current Camera3D is the listener.
 *
 * ── every player ─────────────────────────────────────────────────────
 *   stream         the sound: an asset path (WAV, FLAC, MP3, Ogg Vorbis, Ogg Opus, modules). Loaded on first
 *                  play and shared by every player that names it.
 *   volume_db      gain in decibels (0 = as recorded, -6 halves the amplitude, -80 and below is silence)
 *   pitch_scale    playback rate (1 = as recorded)
 *   autoplay       start on the first fixed tick after the node is ready
 *   loop           repeat until stopped
 *   bus            the mixer bus by name ("Master", or a name given to cw_bus_create); an unknown name plays
 *                  into the master
 *   max_polyphony  how many copies may sound at once (1..8). play while that many are sounding stops the
 *                  oldest first. Default 1: play restarts the sound.
 *   stream_paused  hold every voice where it is
 *   playing        true while a voice is sounding or held; set it true to play, false to stop (not saved)
 *   signal finished()   a voice played to its end (never for a looping, stopped or stolen one)
 *   methods play (an optional float: seconds into the sound to start from), stop
 *
 * ── AudioStreamPlayer2D ──────────────────────────────────────────────
 *   max_distance      pixels from the middle of the view at which the sound is silent (2000)
 *   attenuation       the fade's exponent: gain = (1 - distance / max_distance) ^ attenuation (1 = linear)
 *   panning_strength  how far a sound at the edge of the view is panned: 0 none, 1 hard left/right (1)
 * The view is the one the last frame drew (the current Camera2D); before a frame is drawn the sound plays
 * centred at full volume.
 *
 * ── AudioStreamPlayer3D ──────────────────────────────────────────────
 *   attenuation_model  NONE, INVERSE (default), LINEAR, EXPONENTIAL (cw_atten_model)
 *   unit_size          the distance at which the sound is at full volume (cw_voice_3d.ref_distance; 1)
 *   max_distance       beyond this it gets no quieter (0 = 1000)
 *   rolloff            how fast it fades with distance (1)
 *   cone_inner_angle, cone_outer_angle, cone_outer_gain   a directional sound along the node's -Z: full
 *                      volume inside the inner angle (degrees, full width), cone_outer_gain outside the
 *                      outer one. 360 / 360 (the default) is every direction.
 *   doppler            0 = none, 1 = physical. The node's velocity is its movement between fixed ticks.
 *   air_absorption     distance low-pass (cw_voice_3d.air_absorption; 0 = none)
 *   reverb_send        level into the reverb zones (1; 0 = none)
 *   occlusion          0 clear .. 1 blocked: a low-pass the game sets (a ray cast to the listener)
 *   panning_only       always pan, never the head-related filter (cw_spatial_mode PAN)
 * Every fixed tick the player gives its voices the node's world position, its velocity and its facing.
 *
 * ── AudioListener3D ──────────────────────────────────────────────────
 *   current        this node is a listener (true). Several current listeners (split screen, up to
 *                  CW_AUDIO_MAX_LISTENERS) become listeners 0.. in the order they entered the tree; each
 *                  spatial voice is heard by the nearest.
 * A tree with a 3D player or a listener node sets the mixer's listeners every fixed tick: from its current
 * AudioListener3D nodes, else from the current Camera3D. cw_audio_nodes_set_auto_listener(tree, false) leaves
 * the listener to the game (cw_audio_set_listener).
 *
 * ── the mixer ────────────────────────────────────────────────────────
 * Players need the mixer running. A game that calls cw_audio_init (or uses the game shell, which does) owns it
 * as before. If nothing has started it when a player first plays, the player starts it with the defaults and
 * the tree then calls cw_audio_update each frame.
 *
 * ── timing ───────────────────────────────────────────────────────────
 * Players are stepped once per fixed tick, after the physics step, in the order they entered the tree. For a
 * sound whose length is known, `finished` is counted in fixed ticks from the sound's length and the pitch, not
 * read back from the mixer: it is emitted on the same tick on every host and in a replay, with or without an
 * audio device. A sound of unknown length (a tracker module) finishes when the mixer says its voice ended.
 * A paused tree holds the voices of players it pauses (their pause mode) and their count. */
#ifndef CW_AUDIO_NODES_H
#define CW_AUDIO_NODES_H

#include "audio.h"
#include "scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Play a player node's stream from from_seconds (0 = the start). Returns the voice (null when the node is not
 * a player, has no stream, the stream does not load, or the mixer has no free voice). The voice starts at the
 * next mixed block; a player in a tree is also given its place (2D pan, 3D position) on the same call. */
cw_voice cw_audio_player_play(cw_node player, double from_seconds);
/* Give a player a sound made in code (cw_sound_from_pcm, cw_res_audio_sound, cw_sound_load with a desc of your
 * own): it plays that instead of its stream until given the null handle. What was playing stops. The caller
 * keeps the sound alive. */
void cw_audio_player_set_sound(cw_node player, cw_sound sound);
/* Stop every voice of the player at once. No `finished`. */
void cw_audio_player_stop(cw_node player);
/* true while a voice of the player is sounding or held paused; false for other nodes */
bool cw_audio_player_is_playing(cw_node player);
/* Seconds into the sound of the player's newest voice, counted in fixed ticks (pitch applied; a looping sound
 * keeps counting past its length); 0 when nothing plays. */
double cw_audio_player_position(cw_node player);
/* The player's newest voice, for anything the node has no property for (cw_voice_fade, ...); null when none. */
cw_voice cw_audio_player_voice(cw_node player);
/* false: the tree's audio nodes stop setting the mixer's listeners (the game calls cw_audio_set_listener). */
void cw_audio_nodes_set_auto_listener(cw_tree *t, bool on);

#ifdef __cplusplus
}
#endif
#endif

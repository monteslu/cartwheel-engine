# The story_scene sample's script (tools/story-compile.mjs; the sample's CMake compiles it into assets/main.cwstory).
# A keeper at a gate and a traveller: a choice that moves a stat and sets a flag, a branch on both, a save point,
# a pool of barks and a pool of one-time lines.
cast keeper "The Keeper" voice=bm_george speed=0.95 color=#ffd27a
cast traveller "The Traveller" voice=af_heart speed=1.0 color=#9fe3ff

stat trust = 0
flag gave_name
flag gate_open

scene gate save
  shot two keeper traveller
  say keeper calm "Halt. Nobody crosses the bridge after dark."
  say traveller "It is barely dusk."
  shot react keeper traveller
  say keeper anger:0.6 "Then you have a little time to tell me who you are."
  choice "Answer the keeper" timeout=12 default=1
    option "I am a courier from the coast."
      add trust 2
      set gave_name
      say traveller joy "A courier from the coast, with letters for the hill."
    option "That is my business."
      add trust -1
      say traveller "That is my business, keeper."
    option "Say nothing."
  end
  if flag gave_name and stat trust >= 2
    say keeper joy "The coast. Go on, then, and mind the third plank."
    set gate_open
    emit open_gate 1
  elif stat trust < 0
    shot push keeper dur=2.5
    say keeper anger "Then your business waits here until morning."
  else
    say keeper sorrow "Silence. Fine. Sit by the fire until you find your tongue."
  end
  save
  call farewell
end

scene farewell
  if flag gate_open or stat trust < 0
    say keeper calm "Safe roads."
  else
    say keeper calm "Mind the cold."
  end
end

pool keeper.idle cooldown=4
  line keeper "The river is loud tonight."
  line keeper "Mind the third plank."
  line keeper "Lanterns are lit at the far side."
end

pool keeper.once once
  line keeper surprise "You came back." if flag gate_open
  line keeper "Still here?" if not flag gate_open
end

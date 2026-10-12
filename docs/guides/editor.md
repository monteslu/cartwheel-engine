# The editor (paused)

Work on the editor is paused while its direction is reconsidered, so these
docs do not cover it.

The code in `editor/` still builds and its gates still run, but treat its
commands, panels and extension API as unstable.

You do not need the editor to make a game:

- Scenes (`.cwscene`), project settings (`project.cwproj`) and input maps are
  text files. Edit them by hand or have an agent edit them. See
  [Scenes and nodes](guide:scenes).
- A new project is a copy of a template directory. See
  [Project templates](guide:templates).
- A running game answers questions and takes edits over the debug channel.
  See [The debug channel](guide:debug-channel) and
  [The agent workflow](guide:agent-workflow).

# Loki Action

`loki_action` is a small native module for Android UI planning.

It takes a user command and a JSON dump of the current screen, turns the screen into a compact internal representation, sends that to a local `llama.cpp` server, and returns one resolved action for Loki to execute.

Right now the library can return:

- `click`
- `set_text`
- `back`
- `done`

The external result stays simple:

- `selected_id`
- `path_json`
- `action_type`
- `text`
- `done`

Inside the library the model answers in a compact numeric format to save tokens, and then the native code converts that back into the normal result structure.

The main files are:

- [loki_action_api.h](/Users/murean/Documents/FINAL%20PROJECT/llama.cpp/tools/loki_action/loki_action_api.h)
- [loki_action_internal.h](/Users/murean/Documents/FINAL%20PROJECT/llama.cpp/tools/loki_action/loki_action_internal.h)
- [loki_action_lib.cpp](/Users/murean/Documents/FINAL%20PROJECT/llama.cpp/tools/loki_action/loki_action_lib.cpp)

Build artifact for Android ARM64:

- [libloki_action.so](/Users/murean/Documents/FINAL%20PROJECT/llama.cpp/dev-so/arm64-v8a/libloki_action.so)

The library talks to the local server at `http://127.0.0.1:8080/v1/chat/completions`.

Typical flow:

1. parse the screen tree
2. collect interactive candidates
3. build the prompt context
4. ask the local model for one next action
5. map the chosen id back to the original path
6. return the final result to Loki

Host-side regression test:

- [test-loki-action.cpp](/Users/murean/Documents/FINAL%20PROJECT/llama.cpp/tests/test-loki-action.cpp)

This folder only contains the planning part. Execution on the Android side is handled in the Loki project.

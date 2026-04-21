# Loki Action Native Library

`loki_action` is a standalone native library that takes:

- `user_prompt`
- `screen_json` in the same shape as Loki `ScreenTreeParser`

and performs the full pipeline:

1. parse the screen tree
2. group interactive candidates with stable numeric `id`
3. build the TOON payload
4. call local `llama.cpp` server at `POST /v1/chat/completions`
5. parse a compact model response like `1:7`, `2:12:0`, `5`, `6`, or `0`
6. resolve that `id` back to the original UI `path`

## Files

- Header: [loki_action_api.h](/Users/murean/Documents/FINAL%20PROJECT/llama.cpp/tools/loki_action/loki_action_api.h)
- Implementation: [loki_action_lib.cpp](/Users/murean/Documents/FINAL%20PROJECT/llama.cpp/tools/loki_action/loki_action_lib.cpp)
- Built ARM64 artifact: [libloki_action.so](/Users/murean/Documents/FINAL%20PROJECT/llama.cpp/dev-so/arm64-v8a/libloki_action.so)

## Exported C API

```c
typedef enum {
    LOKI_ACTION_STATUS_OK = 0,
    LOKI_ACTION_STATUS_INVALID_INPUT = 1,
    LOKI_ACTION_STATUS_NO_CANDIDATES = 2,
    LOKI_ACTION_STATUS_HTTP_ERROR = 3,
    LOKI_ACTION_STATUS_INVALID_RESPONSE = 4,
    LOKI_ACTION_STATUS_ID_NOT_FOUND = 5,
} loki_action_status_t;

typedef struct {
    loki_action_status_t status;
    int32_t selected_id;
    const char * path_json;
    const char * error_message;
    const char * action_type;
    const char * text;
    bool done;
} loki_action_result_t;

loki_action_result_t * loki_action_resolve_path(
    const char * user_prompt,
    const char * screen_json,
    const char * host,
    int32_t port
);

void loki_action_result_destroy(loki_action_result_t * result);
```

Successful results:

- Click:
  - `selected_id = 7`
  - `path_json = "[0,1,3]"`
  - `action_type = "click"`
  - `text = NULL`
  - `done = false`
- Set text:
  - `selected_id = 12`
  - `path_json = "[0,4,0]"`
  - `action_type = "set_text"`
  - `text = "котики"`
  - `done = false`
- Task complete:
  - `selected_id = -1`
  - `path_json = "[]"`
  - `action_type = NULL`
  - `text = NULL`
  - `done = true`

Semantics:

- `click` means Loki should execute exactly one click on the resolved node.
- `set_text` means Loki should execute exactly one text insertion on the resolved node.
- `done = true` means the current task is already complete on the visible screen, so Loki should stop and perform no action.
- `set_text` is valid only for nodes that are already present in the `editable` TOON group.
- `back` is allowed only for explicit back/exit/mismatch intents from the user request.
- For prompts that look like text entry or text editing, the library first prioritizes `editable` candidates and asks the model to pick a `set_text` target there.
- If that editable-priority pass returns no match, the library falls back to the remaining non-editable candidates and asks for a `click` target.
- Loki must not search for editable nodes on its own and must not do a preliminary click before `set_text`.
- If the current screen only shows a search bar or launcher container that opens a text field, the correct result is `click`. After the screen changes, a second independent call may return `set_text` for the newly visible `EditText`.

## Runtime behavior

- Default endpoint: `http://127.0.0.1:8080/v1/chat/completions`
- Request settings:
  - `stream = false`
  - `temperature = 0.0`
  - `max_tokens = 96`
- Primary internal wire-format is compact numeric:
  - `0` = no match
  - `1:<id>` = click
  - `2:<id>:<text_index>` = set_text
  - `5` = back
  - `6` = done
- `text_index` points into native text candidates prepared before inference; the library resolves it back to UTF-8 text and exposes the final string via `result->text`.
- Legacy JSON replies are still accepted as a compatibility fallback:
  - `{"id":7,"action":"click","done":false}`
  - `{"id":12,"action":"set_text","text":"котики","done":false}`
  - `{"action":"back","done":false}`
  - `{"done":true}`
  - `{"id":-1}`
- The native library validates that `set_text` can target only ids that belong to the `editable` group. If the model returns `set_text` for a non-editable id, the result is `LOKI_ACTION_STATUS_INVALID_RESPONSE`.
- Runtime TOON shown to the model includes richer accessibility metadata for interactive nodes:
  - `id | label | resId | hint | meta | attrs`
- On failure, the library returns `path_json = "[]"` and an error status/message.
- On Android, the generated TOON payload is written to `adb logcat` with tag `loki_action`.

## Android build command

```bash
/Users/murean/Library/Android/sdk/cmake/3.22.1/bin/cmake \
  -S /Users/murean/Documents/FINAL\ PROJECT/llama.cpp \
  -B /tmp/llama-loki-action-android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=/Users/murean/Library/Android/sdk/ndk/27.0.12077973/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 \
  -DANDROID_STL=c++_shared \
  -DLLAMA_CURL=OFF \
  -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_SERVER=OFF \
  -DLLAMA_BUILD_COMMON=ON \
  -DLLAMA_BUILD_TOOLS=ON

/Users/murean/Library/Android/sdk/cmake/3.22.1/bin/cmake \
  --build /tmp/llama-loki-action-android-arm64 \
  --target loki_action \
  -j 4
```

Output:

- `/tmp/llama-loki-action-android-arm64/bin/libloki_action.so`

## What your colleague needs to do in Loki

1. Copy `libloki_action.so` into `app/src/main/jniLibs/arm64-v8a/`.
2. Make sure `libc++_shared.so` is also packaged if the app build does not already provide it.
3. Add a tiny JNI shim or direct NDK bridge that calls `loki_action_resolve_path(...)`.
4. Pass:
   - user prompt from the temp command
   - current screen JSON from accessibility
   - host `127.0.0.1`
   - port of the local `llama.cpp` server
5. Read on success:
   - `result->path_json`
   - `result->selected_id`
   - `result->action_type`
   - `result->text`
   - `result->done`
6. If `result->done == true`, stop the multi-step flow and do not perform any UI action.
7. If `result->action_type == "click"`, perform exactly one click on the returned path.
8. If `result->action_type == "set_text"`, perform exactly one text insertion with `result->text` on the returned path.
9. Always call `loki_action_result_destroy(result)` after reading the fields.

Debug interpretation:

- If there is no editable node on the current screen, a request like `напиши в поиске котики` should normally return `click` for the search bar container.
- After that click changes the screen and an editable field becomes visible, the next independent call should return `set_text` for that `EditText`.
- If a later screen already satisfies the goal, the next independent call may return `done = true`.

## Dynamic dependencies of the built ARM64 `.so`

The current artifact depends on:

- `liblog.so`
- `libm.so`
- `libc++_shared.so`
- `libdl.so`
- `libc.so`

## Verification done

- Host smoke test compiled and passed:
  - `tests/test-loki-action.cpp`
- Android ARM64 shared library built successfully with NDK r27.

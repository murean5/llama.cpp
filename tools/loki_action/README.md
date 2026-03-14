# Loki Action Native Library

`loki_action` is a standalone native library that takes:

- `user_prompt`
- `screen_json` in the same shape as Loki `ScreenTreeParser`

and performs the full pipeline:

1. parse the screen tree
2. group interactive candidates with stable numeric `id`
3. build the TOON payload
4. call local `llama.cpp` server at `POST /v1/chat/completions`
5. parse plain integer response like `7`
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
} loki_action_result_t;

loki_action_result_t * loki_action_resolve_path(
    const char * user_prompt,
    const char * screen_json,
    const char * host,
    int32_t port
);

void loki_action_result_destroy(loki_action_result_t * result);
```

## Runtime behavior

- Default endpoint: `http://127.0.0.1:8080/v1/chat/completions`
- Request settings:
  - `stream = false`
  - `temperature = 0.0`
  - `max_tokens = 16`
- Built-in system prompt asks the model to use the user request plus the visible screen and reply only with the id number or `-1`.
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
5. Read `result->path_json` on success.
6. Always call `loki_action_result_destroy(result)` after reading the fields.

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

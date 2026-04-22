#ifndef LOKI_ACTION_API_H
#define LOKI_ACTION_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef LOKI_ACTION_API_EXPORTS
        #define LOKI_ACTION_API __declspec(dllexport)
    #else
        #define LOKI_ACTION_API __declspec(dllimport)
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #ifdef LOKI_ACTION_API_EXPORTS
            #define LOKI_ACTION_API __attribute__((visibility("default")))
        #else
            #define LOKI_ACTION_API
        #endif
    #else
        #define LOKI_ACTION_API
    #endif
#endif

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

LOKI_ACTION_API loki_action_result_t * loki_action_resolve_path(
    const char * user_prompt,
    const char * screen_json,
    const char * host,
    int32_t port
);

LOKI_ACTION_API loki_action_result_t * loki_action_resolve_path_with_flags(
    const char * user_prompt,
    const char * screen_json,
    const char * host,
    int32_t port,
    const char * context_flags_json
);

LOKI_ACTION_API void loki_action_result_destroy(loki_action_result_t * result);

LOKI_ACTION_API const char * loki_action_get_version(void);

#ifdef __cplusplus
}
#endif

#endif

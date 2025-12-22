#ifndef LLAMA_SERVER_API_H
#define LLAMA_SERVER_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// Symbol export macros
#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef LLAMA_SERVER_API_EXPORTS
        #define LLAMA_SERVER_API __declspec(dllexport)
    #else
        #define LLAMA_SERVER_API __declspec(dllimport)
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #ifdef LLAMA_SERVER_API_EXPORTS
            #define LLAMA_SERVER_API __attribute__((visibility("default")))
        #else
            #define LLAMA_SERVER_API
        #endif
    #else
        #define LLAMA_SERVER_API
    #endif
#endif

// Opaque handle для server context
typedef void* llama_server_handle_t;

// Простая версия API для /v1/chat/completions

// Server state
typedef enum {
    LLAMA_SERVER_STATE_IDLE = 0,
    LLAMA_SERVER_STATE_LOADING_MODEL,
    LLAMA_SERVER_STATE_READY,
    LLAMA_SERVER_STATE_RUNNING,
    LLAMA_SERVER_STATE_ERROR
} llama_server_state_t;

// Server configuration
typedef struct {
    const char* model_path;
    const char* hostname;
    int32_t port;
    int32_t n_ctx;
    int32_t n_parallel;
    int32_t n_threads;
    int32_t n_threads_batch;
    bool kv_unified;
    const char* ssl_key_file;
    const char* ssl_cert_file;
    const char* api_key;
} llama_server_config_t;

// Callback для логов
typedef void (*llama_server_log_callback_t)(const char* message, void* user_data);

// API Functions

/**
 * Создать новый server handle
 * @return handle или NULL при ошибке
 */
LLAMA_SERVER_API llama_server_handle_t llama_server_create(void);

/**
 * Установить callback для логов
 */
LLAMA_SERVER_API void llama_server_set_log_callback(llama_server_handle_t handle, 
                                    llama_server_log_callback_t callback, 
                                    void* user_data);

/**
 * Инициализировать сервер с конфигурацией
 * @param handle server handle
 * @param config конфигурация сервера
 * @return true при успехе, false при ошибке
 */
LLAMA_SERVER_API bool llama_server_init(llama_server_handle_t handle, const llama_server_config_t* config);

/**
 * Загрузить модель
 * @param handle server handle
 * @return true при успехе, false при ошибке
 */
LLAMA_SERVER_API bool llama_server_load_model(llama_server_handle_t handle);

/**
 * Запустить сервер (блокирующий вызов)
 * @param handle server handle
 * @return 0 при успехе, ненулевое значение при ошибке
 */
LLAMA_SERVER_API int llama_server_run(llama_server_handle_t handle);

/**
 * Запустить сервер в отдельном потоке (неблокирующий)
 * @param handle server handle
 * @return true при успехе, false при ошибке
 */
LLAMA_SERVER_API bool llama_server_start(llama_server_handle_t handle);

/**
 * Остановить сервер
 * @param handle server handle
 */
LLAMA_SERVER_API void llama_server_stop(llama_server_handle_t handle);

/**
 * Получить текущее состояние сервера
 * @param handle server handle
 * @return состояние сервера
 */
LLAMA_SERVER_API llama_server_state_t llama_server_get_state(llama_server_handle_t handle);

/**
 * Проверить, запущен ли сервер
 * @param handle server handle
 * @return true если сервер запущен
 */
LLAMA_SERVER_API bool llama_server_is_running(llama_server_handle_t handle);

/**
 * Получить порт, на котором работает сервер
 * @param handle server handle
 * @return порт или -1 при ошибке
 */
LLAMA_SERVER_API int32_t llama_server_get_port(llama_server_handle_t handle);

/**
 * Освободить ресурсы сервера
 * @param handle server handle
 */
LLAMA_SERVER_API void llama_server_destroy(llama_server_handle_t handle);

/**
 * Получить версию библиотеки
 * @return версия в виде строки
 */
LLAMA_SERVER_API const char* llama_server_get_version(void);

#ifdef __cplusplus
}
#endif

#endif // LLAMA_SERVER_API_H


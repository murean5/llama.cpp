// Полноценная библиотечная версия llama-server для Android
// Версия 1.2.2 - Фикс типов и логики стриминга

#define LLAMA_SERVER_LIBRARY 1
#include <android/log.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>

// Тег для логов в Android
#define LOG_TAG "llama.cpp"
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#include "ggml.h"
#include "llama.h"

// Колбэк для перехвата логов самого llama.cpp
static void android_log_callback(ggml_log_level level, const char * text, void * user_data) {
    (void) user_data;
    if (!text) return;
    switch (level) {
        case GGML_LOG_LEVEL_ERROR: ALOGE("%s", text); break;
        case GGML_LOG_LEVEL_WARN:  ALOGW("%s", text); break;
        case GGML_LOG_LEVEL_INFO:  ALOGI("%s", text); break;
        case GGML_LOG_LEVEL_DEBUG: ALOGD("%s", text); break;
        default: ALOGD("%s", text); break;
    }
}

#include "chat.h"
#include "utils.hpp"
#include "arg.h"
#include "common.h"
#include "json-schema-to-grammar.h"
#include "log.h"
#include "sampling.h"
#include "speculative.h"
#include "mtmd.h"
#include "llama_server_api.h"

// Включаем реализацию сервера. 
#include "server.cpp"

#include <atomic>
#include <thread>
#include <memory>
#include <string>

struct llama_server_wrapper {
    std::unique_ptr<server_context> ctx;
    std::unique_ptr<httplib::Server> svr;
    std::thread server_thread;
    std::atomic<bool> running{false};
    common_params params;
    std::atomic<llama_server_state_t> state{LLAMA_SERVER_STATE_IDLE};
    int32_t actual_port = -1;
    
    ~llama_server_wrapper() {
        stop();
        if (server_thread.joinable()) server_thread.join();
    }
    
    void stop() {
        if (running.load()) {
            running = false;
            if (svr) svr->stop();
            if (ctx) ctx->queue_tasks.terminate();
        }
    }
};

static void register_routes(llama_server_wrapper* wrapper) {
    auto& svr = wrapper->svr;
    auto& ctx_server = *wrapper->ctx;

    svr->set_pre_routing_handler([](const httplib::Request & req, httplib::Response & res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "*");
        if (req.method == "OPTIONS") {
            res.set_content("", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    auto handle_health = [](const httplib::Request &, httplib::Response & res) {
        res_ok(res, {{"status", "ok"}});
    };
    svr->Get("/health", handle_health);
    svr->Get("/v1/health", handle_health);

    auto handle_models = [wrapper](const httplib::Request &, httplib::Response & res) {
        json model_meta = wrapper->ctx->model_meta();
        json models = {
            {"object", "list"},
            {"data", {{
                {"id", wrapper->params.model_alias.empty() ? wrapper->params.model.path : wrapper->params.model_alias},
                {"object", "model"},
                {"created", std::time(nullptr)},
                {"owned_by", "llama.cpp"},
                {"meta", model_meta}
            }}}
        };
        res_ok(res, models);
    };
    svr->Get("/models", handle_models);
    svr->Get("/v1/models", handle_models);

    auto handle_completions_logic = [wrapper](
        server_task_type type,
        json & data,
        const std::vector<raw_buffer> & files,
        const std::function<bool()> & is_connection_closed,
        httplib::Response & res,
        oaicompat_type oaicompat
    ) {
        ALOGI("=== handle_completions_logic START ===");
        ALOGI("oaicompat=%d, stream=%d", oaicompat, json_value(data, "stream", false));
        
        auto completion_id = gen_chatcmplid();
        const auto rd = std::make_shared<server_response_reader>(*wrapper->ctx);
        
        try {
            std::vector<server_task> tasks;
            const auto & prompt = data.at("prompt");
            std::vector<server_tokens> inputs;
            
            if (oaicompat && wrapper->ctx->mctx != nullptr) {
                inputs.push_back(process_mtmd_prompt(wrapper->ctx->mctx, prompt.get<std::string>(), files));
            } else {
                inputs = tokenize_input_prompts(wrapper->ctx->vocab, wrapper->ctx->mctx, prompt, true, true);
            }
            
            tasks.reserve(inputs.size());
            for (size_t i = 0; i < inputs.size(); i++) {
                server_task task = server_task(type);
                task.id = wrapper->ctx->queue_tasks.get_new_id();
                task.index = i;
                task.tokens = std::move(inputs[i]);
                task.params = server_task::params_from_json_cmpl(wrapper->ctx->ctx, wrapper->params, data);
                task.params.oaicompat = oaicompat;
                task.params.oaicompat_cmpl_id = completion_id;
                tasks.push_back(std::move(task));
            }
            rd->post_tasks(std::move(tasks));
        } catch (const std::exception & e) {
            res_error(res, format_error_response(e.what(), ERROR_TYPE_INVALID_REQUEST));
            return;
        }

        bool stream = json_value(data, "stream", false);
        if (!stream) {
            auto all_results = rd->wait_for_all(is_connection_closed);
            if (all_results.is_terminated) return;
            if (all_results.error) {
                res_error(res, all_results.error->to_json());
            } else {
                json arr = json::array();
                for (auto & r : all_results.results) arr.push_back(r->to_json());
                res_ok(res, arr.size() == 1 ? arr[0] : arr);
            }
        } else {
            // Реализация стриминга напрямую (без server_sent_event_provider)
            ALOGI("Streaming mode: waiting for first result...");
            server_task_result_ptr first = rd->next(is_connection_closed);
            if (!first) {
                ALOGW("No first result received!");
                return;
            }
            if (first->is_error()) {
                ALOGE("First result is error: %s", first->to_json().dump().c_str());
                res_error(res, first->to_json());
                return;
            }
            ALOGI("First result received, starting stream...");
            json first_json = first->to_json();
            auto chunk_provider = [first_json, rd, oaicompat](size_t, httplib::DataSink & sink) mutable -> bool {
                if (!first_json.empty()) {
                    if (!server_sent_event(sink, first_json)) { sink.done(); return false; }
                    first_json.clear();
                }
                auto res_ptr = rd->next([&sink]{ return !sink.is_writable(); });
                if (!res_ptr) { sink.done(); return false; }
                if (res_ptr->is_error()) {
                    server_sent_event(sink, json {{ "error", res_ptr->to_json() }});
                    sink.done(); return false;
                }
                if (!server_sent_event(sink, res_ptr->to_json())) { sink.done(); return false; }
                if (!rd->has_next()) {
                    if (oaicompat != OAICOMPAT_TYPE_NONE) sink.write("data: [DONE]\n\n", 13);
                    sink.done(); return false;
                }
                return true;
            };
            res.set_chunked_content_provider("text/event-stream", chunk_provider, [rd](bool){ rd->stop(); });
        }
    };

    svr->Post("/completion", [handle_completions_logic](const httplib::Request & req, httplib::Response & res) {
        try {
            json data = json::parse(req.body);
            std::vector<raw_buffer> files;
            handle_completions_logic(SERVER_TASK_TYPE_COMPLETION, data, files, req.is_connection_closed, res, OAICOMPAT_TYPE_NONE);
        } catch (const std::exception& e) { res_error(res, {{"error", e.what()}}); }
    });

    svr->Post("/v1/chat/completions", [wrapper, handle_completions_logic](const httplib::Request & req, httplib::Response & res) {
        ALOGI("=== CHAT COMPLETIONS REQUEST RECEIVED ===");
        ALOGI("Request body length: %zu", req.body.size());
        
        try {
            ALOGI("Parsing JSON body...");
            json body = json::parse(req.body);
            ALOGI("JSON parsed successfully. Messages count: %zu", body.contains("messages") ? body["messages"].size() : 0);
            
            // Логируем сообщения
            if (body.contains("messages") && body["messages"].is_array()) {
                for (size_t i = 0; i < body["messages"].size() && i < 5; i++) {
                    auto& msg = body["messages"][i];
                    std::string role = msg.contains("role") ? msg["role"].get<std::string>() : "unknown";
                    std::string content = msg.contains("content") ? msg["content"].get<std::string>() : "";
                    ALOGI("Message[%zu]: role='%s', content='%.100s'", i, role.c_str(), content.c_str());
                }
            }
            
            std::vector<raw_buffer> files;
            ALOGI("Calling oaicompat_chat_params_parse...");
            json data = oaicompat_chat_params_parse(body, wrapper->ctx->oai_parser_opt, files);
            ALOGI("oaicompat_chat_params_parse completed");
            
            // ДИАГНОСТИКА: Логируем сформированный промпт
            if (data.contains("prompt")) {
                std::string prompt = data["prompt"].get<std::string>();
                ALOGI("=== FORMATTED PROMPT LENGTH: %zu ===", prompt.size());
                // Логируем по частям (logcat ограничивает длину)
                for (size_t i = 0; i < prompt.size() && i < 2000; i += 400) {
                    ALOGI("PROMPT[%zu]: %.400s", i, prompt.c_str() + i);
                }
                ALOGI("=== END PROMPT ===");
            } else {
                ALOGE("WARNING: No 'prompt' field in parsed data!");
                ALOGI("Parsed data keys: %s", data.dump().substr(0, 500).c_str());
            }
            
            ALOGI("Calling handle_completions_logic...");
            handle_completions_logic(SERVER_TASK_TYPE_COMPLETION, data, files, req.is_connection_closed, res, OAICOMPAT_TYPE_CHAT);
            ALOGI("handle_completions_logic completed");
        } catch (const std::exception& e) {
            ALOGE("Chat error: %s", e.what());
            res_error(res, {{"error", std::string("Chat error: ") + e.what()}});
        }
    });

    svr->Get("/props", [wrapper](const httplib::Request &, httplib::Response & res) {
        std::string tmpl_source = "null";
        if (wrapper->ctx->chat_templates) {
            tmpl_source = common_chat_templates_source(wrapper->ctx->chat_templates.get());
        }
        json data = {
            {"model_path", wrapper->params.model.path},
            {"total_slots", wrapper->params.n_parallel},
            {"chat_template", tmpl_source},
            {"chat_template_valid", wrapper->ctx->chat_templates != nullptr},
            {"oai_parser_tmpls_valid", wrapper->ctx->oai_parser_opt.tmpls != nullptr},
            {"use_jinja", wrapper->ctx->oai_parser_opt.use_jinja},
            {"version", "1.2.3-android-debug"}
        };
        res_ok(res, data);
    });

    svr->Post("/tokenize", [wrapper](const httplib::Request & req, httplib::Response & res) {
        try {
            json body = json::parse(req.body);
            bool add_special = json_value(body, "add_special", false);
            llama_tokens tokens = tokenize_mixed(wrapper->ctx->vocab, body.at("content"), add_special, true);
            res_ok(res, {{"tokens", tokens}});
        } catch (...) { res_error(res, {{"error", "parse error"}}); }
    });
}

static int run_server_internal(llama_server_wrapper* wrapper) {
    try {
        ALOGI("=== LLAMA SERVER ANDROID STARTING ===");
        llama_log_set(android_log_callback, nullptr);
        common_init();
        llama_backend_init();
        llama_numa_init(wrapper->params.numa);
        wrapper->ctx = std::make_unique<server_context>();
        wrapper->state = LLAMA_SERVER_STATE_LOADING_MODEL;
        if (!wrapper->ctx->load_model(wrapper->params)) {
            wrapper->state = LLAMA_SERVER_STATE_ERROR;
            return -1;
        }
        
        // КРИТИЧЕСКИ ВАЖНО: Инициализация chat templates и oai_parser_opt
        // Без этого /v1/chat/completions работает некорректно!
        ALOGI("Initializing chat templates and OAI parser options...");
        ALOGI("Model pointer: %p", (void*)wrapper->ctx->model);
        ALOGI("Chat template param: '%s'", wrapper->params.chat_template.c_str());
        
        wrapper->ctx->chat_templates = common_chat_templates_init(
            wrapper->ctx->model, 
            wrapper->params.chat_template
        );
        
        // Проверяем что получилось
        if (wrapper->ctx->chat_templates) {
            std::string tmpl_src = common_chat_templates_source(wrapper->ctx->chat_templates.get());
            ALOGI("Chat template loaded! Source (first 200 chars): %.200s", tmpl_src.c_str());
        } else {
            ALOGE("CRITICAL: chat_templates is NULL after init!");
        }
        
        // Проверка валидности chat template
        try {
            common_chat_format_example(
                wrapper->ctx->chat_templates.get(), 
                wrapper->params.use_jinja, 
                wrapper->params.default_template_kwargs
            );
            ALOGI("Chat template validated successfully");
        } catch (const std::exception & e) {
            ALOGW("Chat template parsing error: %s", e.what());
            ALOGW("Falling back to chatml template");
            wrapper->ctx->chat_templates = common_chat_templates_init(wrapper->ctx->model, "chatml");
        }
        
        // Инициализация oai_parser_opt (КРИТИЧЕСКИ ВАЖНО!)
        const bool enable_thinking = wrapper->params.use_jinja 
            && wrapper->params.reasoning_budget != 0 
            && common_chat_templates_support_enable_thinking(wrapper->ctx->chat_templates.get());
        
        wrapper->ctx->oai_parser_opt = {
            /* use_jinja             */ wrapper->params.use_jinja,
            /* prefill_assistant     */ wrapper->params.prefill_assistant,
            /* reasoning_format      */ wrapper->params.reasoning_format,
            /* chat_template_kwargs  */ wrapper->params.default_template_kwargs,
            /* common_chat_templates */ wrapper->ctx->chat_templates.get(),
            /* allow_image           */ wrapper->ctx->mctx ? mtmd_support_vision(wrapper->ctx->mctx) : false,
            /* allow_audio           */ wrapper->ctx->mctx ? mtmd_support_audio(wrapper->ctx->mctx) : false,
            /* enable_thinking       */ enable_thinking,
        };
        
        ALOGI("OAI parser options initialized (use_jinja=%d, enable_thinking=%d)", 
              wrapper->params.use_jinja, enable_thinking);
        
        wrapper->ctx->init();
        wrapper->svr = std::make_unique<httplib::Server>();
        register_routes(wrapper);
        wrapper->state = LLAMA_SERVER_STATE_READY;
        wrapper->actual_port = wrapper->params.port;
        wrapper->running = true;
        wrapper->state = LLAMA_SERVER_STATE_RUNNING;
        std::thread task_thread([wrapper]() {
            wrapper->ctx->queue_tasks.on_new_task([wrapper](server_task && task) { wrapper->ctx->process_single_task(std::move(task)); });
            wrapper->ctx->queue_tasks.on_update_slots([wrapper]() { wrapper->ctx->update_slots(); });
            wrapper->ctx->queue_tasks.start_loop();
        });
        bool success = wrapper->svr->listen(wrapper->params.hostname.c_str(), wrapper->params.port);
        wrapper->ctx->queue_tasks.terminate();
        if (task_thread.joinable()) task_thread.join();
        wrapper->running = false;
        if (!success) wrapper->state = LLAMA_SERVER_STATE_ERROR;
        return success ? 0 : -1;
    } catch (const std::exception& e) {
        ALOGE("CRITICAL SERVER ERROR: %s", e.what());
        wrapper->state = LLAMA_SERVER_STATE_ERROR;
        return -1;
    }
}

extern "C" {
LLAMA_SERVER_API llama_server_handle_t llama_server_create(void) { return static_cast<llama_server_handle_t>(new llama_server_wrapper()); }
LLAMA_SERVER_API void llama_server_set_log_callback(llama_server_handle_t h, llama_server_log_callback_t c, void* d) { (void)h;(void)c;(void)d; }
LLAMA_SERVER_API bool llama_server_init(llama_server_handle_t h, const llama_server_config_t* c) {
    if (!h || !c) return false;
    auto* w = static_cast<llama_server_wrapper*>(h);
    w->params = {};
    w->params.model.path = c->model_path;
    w->params.hostname = c->hostname ? c->hostname : "127.0.0.1";
    w->params.port = c->port > 0 ? c->port : 8080;
    w->params.n_ctx = c->n_ctx > 0 ? c->n_ctx : 2048;
    w->params.n_parallel = c->n_parallel > 0 ? c->n_parallel : 1;
    
    // КРИТИЧНО: Включаем Jinja для корректной работы chat templates!
    // Без этого template не применяется и промпт отправляется "сырым"
    w->params.use_jinja = true;
    
    w->state = LLAMA_SERVER_STATE_IDLE;
    return true;
}
LLAMA_SERVER_API bool llama_server_load_model(llama_server_handle_t h) { return !!h; }
LLAMA_SERVER_API int llama_server_run(llama_server_handle_t h) { return h ? run_server_internal(static_cast<llama_server_wrapper*>(h)) : -1; }
LLAMA_SERVER_API bool llama_server_start(llama_server_handle_t h) {
    if (!h) return false;
    auto* w = static_cast<llama_server_wrapper*>(h);
    if (w->running.load()) return false;
    try {
        if (w->server_thread.joinable()) {
            w->server_thread.join();
        }

        w->server_thread = std::thread([w]() {
            try {
                (void) run_server_internal(w);
            } catch (const std::exception & e) {
                ALOGE("llama_server_start thread exception: %s", e.what());
                w->state = LLAMA_SERVER_STATE_ERROR;
                w->running = false;
            } catch (...) {
                ALOGE("llama_server_start thread unknown exception");
                w->state = LLAMA_SERVER_STATE_ERROR;
                w->running = false;
            }
        });

        return true;
    } catch (const std::exception & e) {
        ALOGE("llama_server_start exception: %s", e.what());
        w->state = LLAMA_SERVER_STATE_ERROR;
        w->running = false;
        return false;
    } catch (...) {
        ALOGE("llama_server_start unknown exception");
        w->state = LLAMA_SERVER_STATE_ERROR;
        w->running = false;
        return false;
    }
}
LLAMA_SERVER_API void llama_server_stop(llama_server_handle_t h) { if (h) static_cast<llama_server_wrapper*>(h)->stop(); }
LLAMA_SERVER_API llama_server_state_t llama_server_get_state(llama_server_handle_t h) { return h ? static_cast<llama_server_wrapper*>(h)->state.load() : LLAMA_SERVER_STATE_ERROR; }
LLAMA_SERVER_API bool llama_server_is_running(llama_server_handle_t h) { return h ? static_cast<llama_server_wrapper*>(h)->running.load() : false; }
LLAMA_SERVER_API int32_t llama_server_get_port(llama_server_handle_t h) { return h ? static_cast<llama_server_wrapper*>(h)->actual_port : -1; }
LLAMA_SERVER_API void llama_server_destroy(llama_server_handle_t h) {
    if (!h) return;
    auto* w = static_cast<llama_server_wrapper*>(h);
    w->stop();
    if (w->server_thread.joinable()) w->server_thread.join();
    delete w;
}
LLAMA_SERVER_API const char* llama_server_get_version(void) { return "1.2.2-android-fix-final"; }
}

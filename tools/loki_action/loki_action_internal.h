#ifndef LOKI_ACTION_INTERNAL_H
#define LOKI_ACTION_INTERNAL_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace loki_action {

using json = nlohmann::ordered_json;

struct model_action_response {
    int32_t selected_id = -1;
    std::string action_type;
    std::optional<std::string> text;
    bool done = false;
};

struct prompt_context {
    std::string task;
    std::vector<std::string> history_tokens;
    std::vector<std::string> history_labels;
    std::vector<std::string> history_apps;
    std::vector<std::string> history_entries;
    std::vector<std::string> history_signatures;
    std::vector<int32_t> history_ids;
    int step_number = 1;
    bool has_loop_hint = false;
    std::string loop_hint;
    int repeated_tail_clicks = 0;
    int repeated_tail_same_id = 0;
    int repeated_tail_same_signature = 0;
};

struct extracted_steps_plan {
    std::string goal;
    std::vector<std::string> apps;
    std::vector<std::string> steps;
    std::string done_when;
};

prompt_context parse_prompt_context(const std::string & raw_prompt);
std::optional<extracted_steps_plan> parse_steps_extractor_content(const std::string & content);

json group_by_attrs_textual(const json & tree);
json prepare_for_toon(const json & grouped);
std::string json_to_toon(const json & prepared);
std::string find_path_json_by_id(const json & grouped, int32_t selected_id);
json filter_grouped_by_ids(const json & grouped, const std::vector<int32_t> & ids);
bool prompt_requests_text_edit(const std::string & user_prompt);
std::string build_action_response_grammar(
    const std::vector<int32_t> & ids,
    const std::vector<int32_t> & editable_ids,
    bool allow_click = true,
    bool allow_back = false,
    bool allow_done = true
);
model_action_response extract_action_response_from_chat_response(const std::string & response_body);
void validate_action_response_for_grouped(const json & grouped, const model_action_response & response);

} // namespace loki_action

#endif

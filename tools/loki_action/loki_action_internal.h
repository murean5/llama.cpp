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
    int32_t text_index = -1;
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
    std::vector<std::string> phase_hints;
    std::string done_when;
    std::vector<std::string> target_text_candidates;
};

struct context_flags {
    bool hide_non_clickable_text = false;
    bool drop_text_size = false;
    bool drop_role_and_state = false;
    bool drop_top_region = false;
    bool drop_input_type = false;
    int max_depth = 15;
};

struct done_validation_result {
    bool accepted = false;
    std::string reason = "no strong done signal";
};

prompt_context parse_prompt_context(const std::string & raw_prompt);
std::optional<extracted_steps_plan> parse_steps_extractor_content(const std::string & content);
context_flags parse_context_flags_json_for_test(const std::string & raw_json);

json group_by_attrs_textual(const json & tree);
json group_by_attrs_textual_with_flags(const json & tree, const context_flags & flags);
json prepare_for_toon(const json & grouped);
std::string json_to_toon(const json & prepared);
std::string build_runtime_toon_for_test(
    const json & root,
    const json & grouped,
    const context_flags & flags = context_flags{}
);
std::string find_path_json_by_id(const json & grouped, int32_t selected_id);
json filter_grouped_by_ids(const json & grouped, const std::vector<int32_t> & ids);
bool prompt_requests_text_edit(const std::string & user_prompt);
std::vector<std::string> derive_text_candidates_for_test(
    const std::string & task,
    const std::optional<extracted_steps_plan> & extracted_plan
);
bool should_auto_accept_direct_click_for_test(
    const json & grouped,
    int32_t selected_id,
    const std::vector<std::string> & task_terms,
    const std::vector<std::string> & static_lines
);
std::string build_action_response_grammar(
    const std::vector<int32_t> & ids,
    const std::vector<int32_t> & editable_ids,
    size_t text_candidate_count = 0,
    bool allow_click = true,
    bool allow_back = false,
    bool allow_done = true
);
model_action_response extract_action_response_from_chat_response(const std::string & response_body);
void validate_action_response_for_grouped(const json & grouped, const model_action_response & response);
done_validation_result validate_done_response_for_test(
    const json & grouped,
    const std::string & task,
    const prompt_context & context,
    const std::optional<extracted_steps_plan> & plan,
    const context_flags & flags = context_flags{}
);

}

#endif

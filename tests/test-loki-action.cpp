#include "../tools/loki_action/loki_action_internal.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using json = nlohmann::ordered_json;

static std::string read_file(const std::string & path) {
    std::ifstream fs(path, std::ios_base::binary);
    if (!fs.is_open()) {
        throw std::runtime_error("failed to open file: " + path);
    }
    fs.seekg(0, std::ios_base::end);
    const auto size = fs.tellg();
    fs.seekg(0);
    std::string out;
    out.resize(static_cast<size_t>(size));
    fs.read(out.data(), static_cast<std::streamsize>(size));
    return out;
}

static void assert_equals(const std::string & expected, const std::string & actual, const std::string & message) {
    if (expected != actual) {
        std::cerr << message << "\nExpected:\n" << expected << "\nActual:\n" << actual << std::endl;
        throw std::runtime_error(message);
    }
}

static void assert_true(bool condition, const std::string & message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    const auto input = json::parse(read_file("tests/fixtures/loki_action/in.json"));
    const auto expected_grouped = json::parse(read_file("tests/fixtures/loki_action/out.json"));
    const auto expected_toon = read_file("tests/fixtures/loki_action/out.toon");

    const auto grouped = loki_action::group_by_attrs_textual(input.at("root"));
    assert_equals(expected_grouped.dump(), grouped.dump(), "grouped JSON mismatch");
    assert_equals(
        "ComposeRowIcon:Shortcuts",
        grouped.at("clickable")[0].at("resId").get<std::string>(),
        "grouped clickable should include shortened resId"
    );

    const auto prepared = loki_action::prepare_for_toon(grouped);
    const auto toon = loki_action::json_to_toon(prepared);
    assert_equals(expected_toon, toon, "TOON mismatch");

    const auto path_json = loki_action::find_path_json_by_id(grouped, 7);
    assert_equals("[0,1,0,0,0,0,3,1]", path_json, "path lookup mismatch");
    assert_equals("", loki_action::find_path_json_by_id(grouped, 999), "missing id should have empty path");

    const auto editable_only = loki_action::filter_grouped_by_ids(grouped, {2});
    assert_true(editable_only.contains("editable"), "filtered grouped should keep editable section");
    assert_true(editable_only.contains("clickable"), "filtered grouped should keep clickable mirror for same id");
    assert_true(editable_only.at("editable").size() == 1, "filtered editable size mismatch");
    assert_true(editable_only.at("editable")[0].at("id").get<int32_t>() == 2, "filtered editable id mismatch");

    assert_true(
        loki_action::prompt_requests_text_edit("find \"daddy\" in search"),
        "quoted search request should be treated as text-edit intent"
    );
    assert_true(
        loki_action::prompt_requests_text_edit(u8"напиши в поиске котики"),
        "russian text-entry request should be treated as text-edit intent"
    );
    assert_true(
        !loki_action::prompt_requests_text_edit("open settings"),
        "plain navigation request should not be treated as text-edit intent"
    );

    const auto grammar = loki_action::build_action_response_grammar({7, 12, 23}, {12}, 2, true, false, true);
    assert_true(grammar.find("done-response") != std::string::npos, "grammar should include done branch");
    assert_true(grammar.find("click-response") != std::string::npos, "grammar should include click branch");
    assert_true(grammar.find("set-text-response") != std::string::npos, "grammar should include set_text branch");
    assert_true(grammar.find("back-response") == std::string::npos, "grammar should not include back by default");
    assert_true(grammar.find(R"("7")") != std::string::npos, "grammar should include allowed ids");
    assert_true(grammar.find("set-text-id-value ::= \"12\"") != std::string::npos, "set_text ids should be limited to editable ids");
    assert_true(grammar.find("done-response ::= \"6\"") != std::string::npos, "grammar should use compact done");
    assert_true(grammar.find("text-index-value ::= \"0\" | \"1\"") != std::string::npos, "grammar should enumerate text indexes");

    const auto editable_grammar = loki_action::build_action_response_grammar({7, 12, 23}, {12}, 2, false, false, false);
    assert_true(
        editable_grammar.find("click-response") == std::string::npos,
        "editable grammar should not include click branch"
    );
    assert_true(
        editable_grammar.find("set-text-response") != std::string::npos,
        "editable grammar should still include set_text branch"
    );

    const auto click_only_grammar = loki_action::build_action_response_grammar({7, 12, 23}, {}, 0, true, false, false);
    assert_true(
        click_only_grammar.find("set-text-response") == std::string::npos,
        "grammar without editable ids should not include set_text branch"
    );

    const auto back_enabled_grammar = loki_action::build_action_response_grammar({7, 12, 23}, {12}, 2, true, true, true);
    assert_true(
        back_enabled_grammar.find("back-response") != std::string::npos,
        "grammar with allow_back should include back branch"
    );
    assert_true(
        back_enabled_grammar.find("back-response ::= \"5\"") != std::string::npos,
        "back grammar should use compact opcode"
    );

    const auto click_response = std::string(
        R"({"choices":[{"message":{"content":"1:7"}}]})"
    );
    const auto click_action = loki_action::extract_action_response_from_chat_response(click_response);
    assert_true(click_action.selected_id == 7, "click selected id mismatch");
    assert_equals("click", click_action.action_type, "click action mismatch");
    assert_true(!click_action.text.has_value(), "click action should not have text");
    assert_true(!click_action.done, "click action should not be done");

    const auto set_text_response = std::string(
        R"({"choices":[{"message":{"content":"2:2:1"}}]})"
    );
    const auto set_text_action = loki_action::extract_action_response_from_chat_response(set_text_response);
    assert_true(set_text_action.selected_id == 2, "set_text selected id mismatch");
    assert_equals("set_text", set_text_action.action_type, "set_text action mismatch");
    assert_true(!set_text_action.text.has_value(), "set_text compact action should not carry raw text");
    assert_true(set_text_action.text_index == 1, "set_text text_index mismatch");
    assert_true(!set_text_action.done, "set_text action should not be done");

    const auto set_text_phrase_response = std::string(
        R"({"choices":[{"message":{"content":"{\"id\":2,\"action\":\"set_text\",\"text\":\"привет, как дела?\"}"}}]})"
    );
    const auto set_text_phrase_action = loki_action::extract_action_response_from_chat_response(set_text_phrase_response);
    assert_true(set_text_phrase_action.text.has_value(), "phrase set_text should have text");
    assert_equals("привет, как дела?", *set_text_phrase_action.text, "phrase set_text payload mismatch");
    assert_true(!set_text_phrase_action.done, "phrase set_text should not be done");

    const auto click_done_false_response = std::string(
        R"({"choices":[{"message":{"content":"{\"id\":7,\"action\":\"click\",\"done\":false}"}}]})"
    );
    const auto click_done_false_action =
        loki_action::extract_action_response_from_chat_response(click_done_false_response);
    assert_true(click_done_false_action.selected_id == 7, "click done=false selected id mismatch");
    assert_equals("click", click_done_false_action.action_type, "click done=false action mismatch");
    assert_true(!click_done_false_action.done, "click done=false should stay false");

    const auto back_response = std::string(
        R"({"choices":[{"message":{"content":"5"}}]})"
    );
    const auto back_action = loki_action::extract_action_response_from_chat_response(back_response);
    assert_true(back_action.selected_id == -1, "back selected id mismatch");
    assert_equals("back", back_action.action_type, "back action mismatch");
    assert_true(!back_action.text.has_value(), "back action should not have text");
    assert_true(!back_action.done, "back action should not be done");

    const auto legacy_back_response = std::string(
        R"({"choices":[{"message":{"content":"{\"action\":\"back\",\"done\":false}"}}]})"
    );
    const auto legacy_back_action = loki_action::extract_action_response_from_chat_response(legacy_back_response);
    assert_true(legacy_back_action.selected_id == -1, "legacy back selected id mismatch");
    assert_equals("back", legacy_back_action.action_type, "legacy back action mismatch");

    const auto done_response = std::string(R"({"choices":[{"message":{"content":"6"}}]})");
    const auto done_action = loki_action::extract_action_response_from_chat_response(done_response);
    assert_true(done_action.done, "done response should set done=true");
    assert_true(done_action.selected_id == -1, "done response should not select id");
    assert_true(done_action.action_type.empty(), "done response should not have action");
    assert_true(!done_action.text.has_value(), "done response should not have text");

    loki_action::validate_action_response_for_grouped(grouped, click_action);
    loki_action::validate_action_response_for_grouped(grouped, set_text_action);
    loki_action::validate_action_response_for_grouped(grouped, back_action);
    loki_action::validate_action_response_for_grouped(grouped, done_action);

    const auto no_match_response = std::string(R"({"choices":[{"message":{"content":"0"}}]})");
    const auto no_match_action = loki_action::extract_action_response_from_chat_response(no_match_response);
    assert_true(no_match_action.selected_id == -1, "no match selected id mismatch");
    assert_true(no_match_action.action_type.empty(), "no match should not have action");
    assert_true(!no_match_action.text.has_value(), "no match should not have text");
    assert_true(!no_match_action.done, "no match should not mark task as done");

    bool did_throw = false;
    try {
        (void) loki_action::extract_action_response_from_chat_response(
            R"({"choices":[{"message":{"content":"{\"id\":12,\"action\":\"set_text\"}"}}]})"
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "set_text without text should throw");

    did_throw = false;
    try {
        (void) loki_action::extract_action_response_from_chat_response(
            R"({"choices":[{"message":{"content":"{\"id\":12,\"action\":\"set_text\",\"text\":\"   \"}"}}]})"
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "blank set_text should throw");

    did_throw = false;
    try {
        loki_action::validate_action_response_for_grouped(
            grouped,
            loki_action::model_action_response{7, "set_text", std::optional<std::string>("котики")}
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "set_text on non-editable id should throw");

    did_throw = false;
    try {
        (void) loki_action::extract_action_response_from_chat_response(
            R"({"choices":[{"message":{"content":"{\"action_type\":\"back\",\"done\":true}"}}]})"
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "back with done=true should throw");

    did_throw = false;
    try {
        loki_action::validate_action_response_for_grouped(
            grouped,
            loki_action::model_action_response{7, "back", std::nullopt, false}
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "back with selected id should throw");

    did_throw = false;
    try {
        (void) loki_action::extract_action_response_from_chat_response(
            R"({"choices":[{"message":{"content":"2:7"}}]})"
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "malformed compact response should throw");

    // parse_prompt_context: single action -- counts itself as 1
    {
        const auto ctx = loki_action::parse_prompt_context(
            "Task: find contact\nHistory (old->new): 1. click id=5 label='Contacts' app=contacts"
        );
        assert_true(ctx.repeated_tail_same_id == 1, "single action same_id should be 1");
        assert_true(ctx.repeated_tail_same_signature == 1, "single action same_sig should be 1");
        assert_true(ctx.repeated_tail_clicks == 1, "single action clicks should be 1");
        assert_true(ctx.step_number == 2, "single history entry: step_number should be 2");
        assert_equals("find contact", ctx.task, "task should be extracted");
    }

    // parse_prompt_context: two identical actions -- indicates stuck (repeated_tail >= 2)
    {
        const auto ctx = loki_action::parse_prompt_context(
            "Task: find contact\nHistory (old->new): "
            "1. click id=5 label='Contacts' app=contacts "
            "2. click id=5 label='Contacts' app=contacts"
        );
        assert_true(ctx.repeated_tail_same_id == 2, "repeated same_id should be 2");
        assert_true(ctx.repeated_tail_same_signature == 2, "repeated same_sig should be 2");
    }

    // parse_prompt_context: three different clicks -- not stuck by id
    {
        const auto ctx = loki_action::parse_prompt_context(
            "Task: navigate\nHistory (old->new): "
            "1. click id=3 label='A' "
            "2. click id=5 label='B' "
            "3. click id=7 label='C'"
        );
        assert_true(ctx.repeated_tail_same_id == 1, "different id clicks should not be stuck");
        assert_true(ctx.repeated_tail_clicks == 3, "three clicks counted correctly");
    }

    // parse_steps_extractor_content: valid content
    {
        const auto plan = loki_action::parse_steps_extractor_content(
            R"({"goal":"Open contacts and find Mom","apps":["contacts"],"steps":["click Contacts","set_text Mom"],"phase_hints":["Open contacts","Type Mom"]})"
        );
        assert_true(plan.has_value(), "valid extractor content should parse");
        assert_equals("Open contacts and find Mom", plan->goal, "plan goal mismatch");
        assert_true(plan->apps.size() == 1, "plan should have 1 app");
        assert_true(plan->steps.size() == 2, "plan should have 2 steps");
        assert_true(plan->phase_hints.size() == 2, "plan should parse phase_hints");
    }

    // parse_steps_extractor_content: invalid/empty content returns nullopt
    assert_true(!loki_action::parse_steps_extractor_content("").has_value(), "empty returns nullopt");
    assert_true(!loki_action::parse_steps_extractor_content("{}").has_value(), "empty obj returns nullopt");
    assert_true(!loki_action::parse_steps_extractor_content("not json").has_value(), "invalid json returns nullopt");

    {
        const json synthetic_tree = {
            {"class", "EditText"},
            {"id", "com.example:id/message_input"},
            {"hintText", "Type message"},
            {"inputType", "textEmailAddress"},
            {"roleDescription", "input"},
            {"stateDescription", "empty"},
            {"error", "Required"},
            {"attrs", json::array({"editable", "clickable"})}
        };
        const auto synthetic_grouped = loki_action::group_by_attrs_textual(synthetic_tree);
        const auto synthetic_prepared = loki_action::prepare_for_toon(synthetic_grouped);
        const auto synthetic_toon = loki_action::json_to_toon(synthetic_prepared);
        assert_true(
            synthetic_grouped.at("editable")[0].at("resId").get<std::string>() == "message_input",
            "synthetic grouped should include shortened resId"
        );
        assert_true(
            synthetic_grouped.at("editable")[0].at("hint").get<std::string>() == "Type message",
            "synthetic grouped should include hint"
        );
        assert_true(
            synthetic_toon.find("id | resId | role | state | error | inputType | hint") != std::string::npos,
            "legacy TOON should use preferred metadata columns"
        );
        assert_true(
            synthetic_toon.find("message_input") != std::string::npos &&
            synthetic_toon.find("Type message") != std::string::npos,
            "legacy TOON should include metadata values"
        );

        const auto runtime_toon = loki_action::build_runtime_toon_for_test(json::object(), synthetic_grouped);
        assert_true(
            runtime_toon.find("id | label | resId | hint | meta | attrs") != std::string::npos,
            "runtime TOON should expose richer interactive columns"
        );
        assert_true(
            runtime_toon.find("message_input") != std::string::npos &&
            runtime_toon.find("Type message") != std::string::npos,
            "runtime TOON should include resId and hint"
        );
        assert_true(
            runtime_toon.find("role=input;state=empty;type=textEmailAddress;error=Required") != std::string::npos,
            "runtime TOON should compact metadata into meta column"
        );
    }

    {
        const json done_grouped = {
            {"clickable", json::array({
                json{
                    {"id", 1},
                    {"path", json::array({0, 0})},
                    {"text", "Alice thread"}
                }
            })}
        };
        loki_action::prompt_context ctx;
        ctx.task = "open thread alice";
        ctx.history_tokens = {"c", "c"};
        ctx.history_ids = {7, 8};

        loki_action::extracted_steps_plan plan;
        plan.steps = {"Open messages", "Open thread Alice", "Read thread"};
        const auto reject_done = loki_action::validate_done_response_for_test(done_grouped, ctx.task, ctx, plan);
        assert_true(!reject_done.accepted, "done should respect plan-aware minimum history");
        assert_true(
            reject_done.reason.find("required=3") != std::string::npos,
            "done rejection should mention required history"
        );

        ctx.history_tokens.push_back("c");
        ctx.history_ids.push_back(9);
        const auto accept_done = loki_action::validate_done_response_for_test(done_grouped, ctx.task, ctx, plan);
        assert_true(accept_done.accepted, "done should pass after plan-aware history floor");

        loki_action::prompt_context fallback_ctx;
        fallback_ctx.task = "open thread alice";
        fallback_ctx.history_tokens = {"c", "c"};
        fallback_ctx.history_ids = {1, 2};
        const auto fallback_done =
            loki_action::validate_done_response_for_test(done_grouped, fallback_ctx.task, fallback_ctx, std::nullopt);
        assert_true(fallback_done.accepted, "done without plan should keep baseline threshold");
    }

    return 0;
}

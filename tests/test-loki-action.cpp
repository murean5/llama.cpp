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

    const auto grammar = loki_action::build_action_response_grammar({7, 12, 23}, {12});
    assert_true(grammar.find("click-response") != std::string::npos, "grammar should include click branch");
    assert_true(grammar.find("set-text-response") != std::string::npos, "grammar should include set_text branch");
    assert_true(grammar.find(R"("7")") != std::string::npos, "grammar should include allowed ids");
    assert_true(grammar.find("set-text-id-value ::= \"12\"") != std::string::npos, "set_text ids should be limited to editable ids");

    const auto editable_grammar = loki_action::build_action_response_grammar({7, 12, 23}, {12}, false);
    assert_true(
        editable_grammar.find("click-response") == std::string::npos,
        "editable grammar should not include click branch"
    );
    assert_true(
        editable_grammar.find("set-text-response") != std::string::npos,
        "editable grammar should still include set_text branch"
    );

    const auto click_only_grammar = loki_action::build_action_response_grammar({7, 12, 23}, {});
    assert_true(
        click_only_grammar.find("set-text-response") == std::string::npos,
        "grammar without editable ids should not include set_text branch"
    );

    const auto click_response = std::string(
        R"({"choices":[{"message":{"content":"{\"id\":7,\"action\":\"click\"}"}}]})"
    );
    const auto click_action = loki_action::extract_action_response_from_chat_response(click_response);
    assert_true(click_action.selected_id == 7, "click selected id mismatch");
    assert_equals("click", click_action.action_type, "click action mismatch");
    assert_true(!click_action.text.has_value(), "click action should not have text");

    const auto set_text_response = std::string(
        R"({"choices":[{"message":{"content":"{\"id\":2,\"action\":\"set_text\",\"text\":\"котики\"}"}}]})"
    );
    const auto set_text_action = loki_action::extract_action_response_from_chat_response(set_text_response);
    assert_true(set_text_action.selected_id == 2, "set_text selected id mismatch");
    assert_equals("set_text", set_text_action.action_type, "set_text action mismatch");
    assert_true(set_text_action.text.has_value(), "set_text action should have text");
    assert_equals("котики", *set_text_action.text, "set_text payload mismatch");

    const auto set_text_phrase_response = std::string(
        R"({"choices":[{"message":{"content":"{\"id\":2,\"action\":\"set_text\",\"text\":\"привет, как дела?\"}"}}]})"
    );
    const auto set_text_phrase_action = loki_action::extract_action_response_from_chat_response(set_text_phrase_response);
    assert_true(set_text_phrase_action.text.has_value(), "phrase set_text should have text");
    assert_equals("привет, как дела?", *set_text_phrase_action.text, "phrase set_text payload mismatch");

    loki_action::validate_action_response_for_grouped(grouped, click_action);
    loki_action::validate_action_response_for_grouped(grouped, set_text_action);

    const auto no_match_response = std::string(R"({"choices":[{"message":{"content":"{\"id\":-1}"}}]})");
    const auto no_match_action = loki_action::extract_action_response_from_chat_response(no_match_response);
    assert_true(no_match_action.selected_id == -1, "no match selected id mismatch");
    assert_true(no_match_action.action_type.empty(), "no match should not have action");
    assert_true(!no_match_action.text.has_value(), "no match should not have text");

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
            R"({"choices":[{"message":{"content":"{\"id\":7,\"action\":\"scroll\"}"}}]})"
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "unknown action should throw");

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
            R"({"choices":[{"message":{"content":"{\"id\":7,\"action\":\"click\""}}]})"
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "malformed json should throw");

    return 0;
}

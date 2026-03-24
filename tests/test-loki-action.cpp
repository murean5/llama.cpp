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

    const auto response = std::string(R"({"choices":[{"message":{"content":"7"}}]})");
    const auto selected_id = loki_action::extract_selected_id_from_chat_response(response);
    assert_true(selected_id == 7, "selected id mismatch");

    const auto response_with_newline = std::string(R"({"choices":[{"message":{"content":"19\n"}}]})");
    assert_true(
        loki_action::extract_selected_id_from_chat_response(response_with_newline) == 19,
        "newline content should parse"
    );

    const auto response_with_json_string = std::string(R"({"choices":[{"message":{"content":"{\"id\":\"23\"}"}}]})");
    assert_true(
        loki_action::extract_selected_id_from_chat_response(response_with_json_string) == 23,
        "json string content should parse"
    );

    const auto response_with_parts = std::string(R"({"choices":[{"message":{"content":[{"type":"text","text":"31"}]}}]})");
    assert_true(
        loki_action::extract_selected_id_from_chat_response(response_with_parts) == 31,
        "content parts should parse"
    );

    bool did_throw = false;
    try {
        (void) loki_action::extract_selected_id_from_chat_response(
            R"({"choices":[{"message":{"content":"id=1"}}]})"
        );
    } catch (const std::exception &) {
        did_throw = true;
    }
    assert_true(did_throw, "invalid response should throw");

    return 0;
}

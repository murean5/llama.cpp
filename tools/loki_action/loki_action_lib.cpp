#include "loki_action_api.h"
#include "loki_action_internal.h"

#include <nlohmann/json.hpp>

#if defined(ANDROID)
#include <android/log.h>
#endif

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <optional>
#include <typeinfo>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loki_action {

namespace {

constexpr const char * SYSTEM_PROMPT =
    "You get a user request and visible Android UI elements. "
    "Pick the best matching element and action using both the request and the screen. "
    "Reply only as JSON. "
    "Use {\"id\":<id>,\"action\":\"click\"} for one-step clicks and for non-editable search bars or containers that only open a text field. "
    "Use {\"id\":<id>,\"action\":\"set_text\",\"text\":\"...\"} only when that id belongs to an editable element already visible on the screen. "
    "For set_text, return only the exact value to type, not the whole user command. "
    "Extract the intended query or replacement text from the request, for example user 'find daddy in search' -> text 'daddy', user 'найди мне котиков' -> text 'котики'. "
    "Use {\"id\":-1} if no match.";

constexpr const char * EDITABLE_PRIORITY_PROMPT =
    "You get a user request and only candidates that are already editable. "
    "If the request is about typing, editing, searching, renaming, or changing text, choose the best editable target and return only "
    "{\"id\":<id>,\"action\":\"set_text\",\"text\":\"...\"}. "
    "For text, return only the exact value to type, not the whole command. "
    "Extract the intended query or replacement text from the request, for example user 'find daddy in search' -> text 'daddy', user 'найди мне котиков' -> text 'котики'. "
    "If none of these editable elements fit, return only {\"id\":-1}.";

constexpr const char * CLICK_FALLBACK_PROMPT =
    "You get a user request and visible Android UI elements. "
    "No editable match was chosen. Pick the best remaining one-step click target. "
    "Reply only as JSON. "
    "Use {\"id\":<id>,\"action\":\"click\"} for clicks. "
    "Use {\"id\":-1} if no match.";

#if defined(ANDROID)
constexpr const char * LOG_TAG = "loki_action";
#define LOKI_LOGI(...) __android_log_print(ANDROID_LOG_INFO, loki_action::LOG_TAG, __VA_ARGS__)
#define LOKI_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, loki_action::LOG_TAG, __VA_ARGS__)
#else
#define LOKI_LOGI(...) ((void) 0)
#define LOKI_LOGE(...) ((void) 0)
#endif

std::string trim_copy(const std::string & input) {
    const auto first = std::find_if_not(input.begin(), input.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    if (first == input.end()) {
        return "";
    }
    const auto last = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return std::string(first, last);
}

std::optional<std::string> json_value_to_string(const json & value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (value.is_string()) {
        const auto trimmed = trim_copy(value.get<std::string>());
        if (trimmed.empty()) {
            return std::nullopt;
        }
        return trimmed;
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "True" : "False";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        char buffer[64];
        const auto written = std::snprintf(buffer, sizeof(buffer), "%.17g", value.get<double>());
        if (written <= 0) {
            return std::nullopt;
        }
        return trim_copy(std::string(buffer, static_cast<size_t>(written)));
    }
    const auto dumped = trim_copy(value.dump());
    if (dumped.empty()) {
        return std::nullopt;
    }
    return dumped;
}

std::optional<std::string> normalized_field(const json & node, const char * key) {
    if (!node.is_object()) {
        return std::nullopt;
    }
    const auto it = node.find(key);
    if (it == node.end()) {
        return std::nullopt;
    }
    return json_value_to_string(*it);
}

json build_text_fields(const json & node) {
    json result = json::object();

    const auto text = normalized_field(node, "text");
    const auto content_desc = normalized_field(node, "contentDesc");

    if (!text && !content_desc) {
        return result;
    }

    if (text && content_desc) {
        if (*text == *content_desc) {
            result["contentDesc"] = *content_desc;
        } else {
            result["text"] = *text;
            result["contentDesc"] = *content_desc;
        }
        return result;
    }

    if (content_desc) {
        result["contentDesc"] = *content_desc;
        return result;
    }

    result["text"] = *text;
    return result;
}

json find_text_fields_in_subtree(const json & node) {
    if (!node.is_object()) {
        return json::object();
    }

    json current = build_text_fields(node);
    if (!current.empty()) {
        return current;
    }

    const auto it = node.find("children");
    if (it == node.end() || !it->is_array()) {
        return json::object();
    }

    for (const auto & child : *it) {
        if (!child.is_object()) {
            continue;
        }
        json nested = find_text_fields_in_subtree(child);
        if (!nested.empty()) {
            return nested;
        }
    }

    return json::object();
}

std::string attr_to_string(const json & value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return value.dump();
}

void walk_tree(const json & node, std::vector<int> & path, json & grouped, int32_t & next_id) {
    if (!node.is_object()) {
        return;
    }

    json attrs = json::array();
    const auto attrs_it = node.find("attrs");
    if (attrs_it != node.end() && attrs_it->is_array()) {
        attrs = *attrs_it;
    }

    json text_fields = build_text_fields(node);
    if (!attrs.empty() && text_fields.empty()) {
        text_fields = find_text_fields_in_subtree(node);
    }

    if (!attrs.empty() && !text_fields.empty()) {
        const int32_t item_id = next_id++;
        json item = json::object();
        item["id"] = item_id;
        if (const auto class_name = normalized_field(node, "class")) {
            item["class"] = *class_name;
        } else {
            item["class"] = nullptr;
        }
        item["path"] = json::array();
        for (int step : path) {
            item["path"].push_back(step);
        }
        for (auto it = text_fields.begin(); it != text_fields.end(); ++it) {
            item[it.key()] = it.value();
        }

        for (const auto & attr : attrs) {
            const auto key = attr_to_string(attr);
            if (!grouped.contains(key)) {
                grouped[key] = json::array();
            }
            grouped[key].push_back(item);
        }
    }

    const auto children_it = node.find("children");
    if (children_it == node.end() || !children_it->is_array()) {
        return;
    }

    for (size_t index = 0; index < children_it->size(); ++index) {
        const auto & child = (*children_it)[index];
        if (!child.is_object()) {
            continue;
        }
        path.push_back(static_cast<int>(index));
        walk_tree(child, path, grouped, next_id);
        path.pop_back();
    }
}

std::string stringify_cell(const json & value) {
    if (value.is_null()) {
        return "";
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        char buffer[64];
        const auto written = std::snprintf(buffer, sizeof(buffer), "%.17g", value.get<double>());
        if (written <= 0) {
            return "";
        }
        return std::string(buffer, static_cast<size_t>(written));
    }
    return value.dump();
}

std::string join_cells(const std::vector<std::string> & values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += " | ";
        }
        out += values[i];
    }
    return out;
}

std::string escape_for_log(const std::string & input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (const char ch : input) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}

std::string truncate_for_log(const std::string & input, size_t max_size = 1200) {
    if (input.size() <= max_size) {
        return input;
    }
    return input.substr(0, max_size) + "...<truncated>";
}

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_ascii(const std::string & input) {
    return trim_copy(input);
}

struct HttpPostResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

bool send_all_bytes(int fd, const std::string & data, std::string & error) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (written < 0) {
            error = std::string("send failed: ") + std::strerror(errno);
            return false;
        }
        if (written == 0) {
            error = "send returned 0 bytes";
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

std::string decode_chunked_body(const std::string & body) {
    std::string decoded;
    size_t cursor = 0;
    while (true) {
        const size_t line_end = body.find("\r\n", cursor);
        if (line_end == std::string::npos) {
            throw std::runtime_error("invalid chunked response");
        }

        const std::string size_line = trim_ascii(body.substr(cursor, line_end - cursor));
        const size_t separator = size_line.find(';');
        const std::string hex_size = separator == std::string::npos ? size_line : size_line.substr(0, separator);

        char * end_ptr = nullptr;
        const unsigned long chunk_size = std::strtoul(hex_size.c_str(), &end_ptr, 16);
        if (end_ptr == hex_size.c_str() || (end_ptr != nullptr && *end_ptr != '\0')) {
            throw std::runtime_error("invalid chunk size");
        }

        cursor = line_end + 2;
        if (chunk_size == 0) {
            break;
        }

        if (cursor + chunk_size + 2 > body.size()) {
            throw std::runtime_error("chunk exceeds body size");
        }

        decoded.append(body, cursor, chunk_size);
        cursor += chunk_size;
        if (body.compare(cursor, 2, "\r\n") != 0) {
            throw std::runtime_error("invalid chunk terminator");
        }
        cursor += 2;
    }
    return decoded;
}

HttpPostResult post_json_via_socket(
    const std::string & host,
    int32_t port,
    const std::string & path,
    const std::string & body,
    int connection_timeout_sec,
    int read_timeout_sec,
    int write_timeout_sec
) {
    HttpPostResult result;

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo * addresses = nullptr;
    const std::string port_string = std::to_string(port);
    const int gai_error = ::getaddrinfo(host.c_str(), port_string.c_str(), &hints, &addresses);
    if (gai_error != 0) {
        result.error = std::string("getaddrinfo failed: ") + gai_strerror(gai_error);
        return result;
    }

    int socket_fd = -1;
    std::string connect_error = "connect failed";

    for (auto * addr = addresses; addr != nullptr; addr = addr->ai_next) {
        socket_fd = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (socket_fd < 0) {
            connect_error = std::string("socket failed: ") + std::strerror(errno);
            continue;
        }

        const struct timeval recv_timeout = {read_timeout_sec, 0};
        const struct timeval send_timeout = {write_timeout_sec, 0};
        const struct timeval conn_timeout = {connection_timeout_sec, 0};
        ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
        ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
#if defined(SO_RCVTIMEO)
        (void) conn_timeout;
#endif

        if (::connect(socket_fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            break;
        }

        connect_error = std::string("connect failed: ") + std::strerror(errno);
        ::close(socket_fd);
        socket_fd = -1;
    }

    ::freeaddrinfo(addresses);

    if (socket_fd < 0) {
        result.error = connect_error;
        return result;
    }

    const std::string request =
        "POST " + path + " HTTP/1.1\r\n"
        "Host: " + host + ":" + std::to_string(port) + "\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" +
        body;

    std::string io_error;
    if (!send_all_bytes(socket_fd, request, io_error)) {
        ::close(socket_fd);
        result.error = io_error;
        return result;
    }

    std::string raw_response;
    char buffer[8192];
    while (true) {
        const ssize_t read_size = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (read_size > 0) {
            raw_response.append(buffer, static_cast<size_t>(read_size));
            continue;
        }
        if (read_size == 0) {
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.error = "recv timeout";
        } else {
            result.error = std::string("recv failed: ") + std::strerror(errno);
        }
        ::close(socket_fd);
        return result;
    }

    ::close(socket_fd);

    const size_t header_end = raw_response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        result.error = "invalid HTTP response";
        return result;
    }

    const std::string header_block = raw_response.substr(0, header_end);
    std::string response_body = raw_response.substr(header_end + 4);

    const size_t first_line_end = header_block.find("\r\n");
    const std::string status_line = first_line_end == std::string::npos
        ? header_block
        : header_block.substr(0, first_line_end);

    size_t first_space = status_line.find(' ');
    if (first_space == std::string::npos) {
        result.error = "invalid status line";
        return result;
    }
    size_t second_space = status_line.find(' ', first_space + 1);
    const std::string status_code = second_space == std::string::npos
        ? status_line.substr(first_space + 1)
        : status_line.substr(first_space + 1, second_space - first_space - 1);

    char * end_ptr = nullptr;
    const long parsed_status = std::strtol(status_code.c_str(), &end_ptr, 10);
    if (end_ptr == status_code.c_str() || (end_ptr != nullptr && *end_ptr != '\0')) {
        result.error = "invalid status code";
        return result;
    }

    bool is_chunked = false;
    size_t header_cursor = first_line_end == std::string::npos ? header_block.size() : first_line_end + 2;
    while (header_cursor < header_block.size()) {
        const size_t line_end = header_block.find("\r\n", header_cursor);
        const std::string line = header_block.substr(
            header_cursor,
            line_end == std::string::npos ? std::string::npos : line_end - header_cursor
        );
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string key = to_lower_ascii(trim_ascii(line.substr(0, colon)));
            const std::string value = to_lower_ascii(trim_ascii(line.substr(colon + 1)));
            if (key == "transfer-encoding" && value.find("chunked") != std::string::npos) {
                is_chunked = true;
            }
        }
        if (line_end == std::string::npos) {
            break;
        }
        header_cursor = line_end + 2;
    }

    if (is_chunked) {
        response_body = decode_chunked_body(response_body);
    }

    result.ok = true;
    result.status = static_cast<int>(parsed_status);
    result.body = std::move(response_body);
    return result;
}

std::string build_user_content(
    const std::string & user_prompt,
    const std::string & screen_name,
    const std::string & toon
) {
    std::string user_content;
    user_content.reserve(user_prompt.size() + screen_name.size() + toon.size() + 32);
    user_content += "Goal: ";
    user_content += user_prompt;
    user_content += "\nPkg: ";
    user_content += screen_name;
    user_content += "\nUI:\n";
    user_content += toon;
    return user_content;
}

bool contains_any_substring(const std::string & haystack, const std::vector<std::string> & needles) {
    for (const auto & needle : needles) {
        if (!needle.empty() && haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool has_quoted_text_payload(const std::string & input) {
    const std::array<std::string, 6> quote_marks = {"\"", "'", u8"«", u8"»", u8"“", u8"”"};
    for (const auto & quote : quote_marks) {
        const auto first = input.find(quote);
        if (first == std::string::npos) {
            continue;
        }
        const auto second = input.find(quote, first + quote.size());
        if (second != std::string::npos && second > first + quote.size()) {
            return true;
        }
    }
    return false;
}

void log_multiline_toon(const std::string & toon) {
    if (toon.empty()) {
        LOKI_LOGI("TOON: <empty>");
        return;
    }

    constexpr size_t chunk_size = 300;
    LOKI_LOGI("TOON BEGIN");
    for (size_t i = 0; i < toon.size(); i += chunk_size) {
        const auto chunk = toon.substr(i, std::min(chunk_size, toon.size() - i));
        LOKI_LOGI("TOON[%zu]: %s", i, chunk.c_str());
    }
    LOKI_LOGI("TOON END");
}

json build_chat_request_payload(
    const std::string & user_prompt,
    const std::string & screen_name,
    const std::string & toon
) {
    return json{
        {"messages", json::array({
            json{{"role", "system"}, {"content", SYSTEM_PROMPT}},
            json{{"role", "user"}, {"content", build_user_content(user_prompt, screen_name, toon)}},
        })},
        {"max_tokens", 16},
        {"stream", false},
        {"temperature", 0.0},
    };
}

std::string extract_textual_content(const json & value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_array()) {
        std::string out;
        for (const auto & part : value) {
            if (part.is_string()) {
                out += part.get<std::string>();
                continue;
            }
            if (!part.is_object()) {
                continue;
            }
            const auto type_it = part.find("type");
            const auto text_it = part.find("text");
            if (text_it != part.end()) {
                if (text_it->is_string()) {
                    out += text_it->get<std::string>();
                } else if (text_it->is_object()) {
                    const auto value_it = text_it->find("value");
                    if (value_it != text_it->end() && value_it->is_string()) {
                        out += value_it->get<std::string>();
                    }
                }
                continue;
            }
            if (type_it != part.end() && type_it->is_string() && type_it->get<std::string>() == "text") {
                const auto value_it = part.find("value");
                if (value_it != part.end() && value_it->is_string()) {
                    out += value_it->get<std::string>();
                }
            }
        }
        return out;
    }
    if (value.is_object()) {
        return value.dump();
    }
    throw std::runtime_error("chat message content is not textual");
}

std::string extract_message_content(const json & root) {
    const auto choices_it = root.find("choices");
    if (choices_it == root.end() || !choices_it->is_array() || choices_it->empty()) {
        throw std::runtime_error("chat response has no choices");
    }

    const auto & first = (*choices_it)[0];
    if (!first.is_object()) {
        throw std::runtime_error("chat choice is not an object");
    }

    const auto message_it = first.find("message");
    if (message_it == first.end() || !message_it->is_object()) {
        throw std::runtime_error("chat choice has no message");
    }

    const auto content_it = message_it->find("content");
    if (content_it == message_it->end()) {
        throw std::runtime_error("chat message content is missing");
    }

    return extract_textual_content(*content_it);
}

int32_t parse_action_id_field(const json & value) {
    if (value.is_number_integer()) {
        return value.get<int32_t>();
    }
    if (value.is_string()) {
        const auto trimmed = trim_copy(value.get<std::string>());
        if (trimmed.empty()) {
            throw std::runtime_error("model response id is empty");
        }

        char * end_ptr = nullptr;
        const long parsed = std::strtol(trimmed.c_str(), &end_ptr, 10);
        if (end_ptr != nullptr && end_ptr != trimmed.c_str() && *end_ptr == '\0') {
            return static_cast<int32_t>(parsed);
        }
    }

    throw std::runtime_error("model response id is not an integer");
}

int32_t parse_action_id_text(const std::string & raw_text) {
    const auto trimmed = trim_copy(raw_text);
    if (trimmed.empty()) {
        throw std::runtime_error("model response id is empty");
    }

    char * end_ptr = nullptr;
    const long value = std::strtol(trimmed.c_str(), &end_ptr, 10);
    if (end_ptr != nullptr && end_ptr != trimmed.c_str() && *end_ptr == '\0') {
        return static_cast<int32_t>(value);
    }

    throw std::runtime_error("model response id is not an integer");
}

model_action_response parse_action_response_json(const json & parsed);

model_action_response parse_action_response_text(const std::string & raw_text) {
    const auto trimmed = trim_copy(raw_text);
    if (trimmed.empty()) {
        throw std::runtime_error("chat response content is empty");
    }

    try {
        const json parsed = json::parse(trimmed);
        return parse_action_response_json(parsed);
    } catch (const json::exception &) {
    }

    model_action_response fallback;
    fallback.selected_id = parse_action_id_text(trimmed);
    if (fallback.selected_id >= 0) {
        fallback.action_type = "click";
    }
    return fallback;
}

model_action_response parse_action_response_json(const json & parsed) {
    if (parsed.is_number_integer()) {
        model_action_response result;
        result.selected_id = parsed.get<int32_t>();
        if (result.selected_id >= 0) {
            result.action_type = "click";
        }
        return result;
    }
    if (parsed.is_string()) {
        return parse_action_response_text(parsed.get<std::string>());
    }
    if (!parsed.is_object()) {
        throw std::runtime_error("model response must be a JSON object");
    }

    const auto id_it = parsed.find("id");
    if (id_it == parsed.end()) {
        throw std::runtime_error("model response is missing id");
    }

    model_action_response result;
    result.selected_id = parse_action_id_field(*id_it);
    if (result.selected_id < 0) {
        return result;
    }

    const auto action_it = parsed.find("action");
    if (action_it == parsed.end() || !action_it->is_string()) {
        throw std::runtime_error("model response is missing action");
    }

    result.action_type = action_it->get<std::string>();
    if (result.action_type == "click") {
        return result;
    }
    if (result.action_type != "set_text") {
        throw std::runtime_error("model response action must be click or set_text");
    }

    const auto text_it = parsed.find("text");
    if (text_it == parsed.end() || !text_it->is_string()) {
        throw std::runtime_error("model response set_text action requires text");
    }

    const std::string raw_insert_text = text_it->get<std::string>();
    if (trim_copy(raw_insert_text).empty()) {
        throw std::runtime_error("model response text must not be empty");
    }
    result.text = raw_insert_text;
    return result;
}

model_action_response extract_action_response_from_chat_response_with_content(
    const std::string & response_body,
    std::string * normalized_content
) {
    const json root = json::parse(response_body);
    const std::string content = extract_message_content(root);
    if (normalized_content != nullptr) {
        *normalized_content = content;
    }
    return parse_action_response_text(content);
}

const char * duplicate_c_string(const std::string & value) {
    char * buffer = static_cast<char *>(std::malloc(value.size() + 1));
    if (buffer == nullptr) {
        return nullptr;
    }
    std::memcpy(buffer, value.c_str(), value.size() + 1);
    return buffer;
}

} // namespace

json group_by_attrs_textual(const json & tree) {
    json grouped = json::object();
    int32_t next_id = 1;
    std::vector<int> path;
    walk_tree(tree, path, grouped, next_id);
    return grouped;
}

json filter_grouped_by_ids(const json & grouped, const std::vector<int32_t> & ids) {
    json filtered = json::object();
    if (ids.empty()) {
        return filtered;
    }

    const std::unordered_set<int32_t> allowed(ids.begin(), ids.end());
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        if (!it.value().is_array()) {
            continue;
        }

        json items = json::array();
        for (const auto & item : it.value()) {
            const auto id_it = item.find("id");
            if (id_it == item.end() || !id_it->is_number_integer()) {
                continue;
            }
            if (allowed.find(id_it->get<int32_t>()) != allowed.end()) {
                items.push_back(item);
            }
        }

        if (!items.empty()) {
            filtered[it.key()] = std::move(items);
        }
    }

    return filtered;
}

bool prompt_requests_text_edit(const std::string & user_prompt) {
    const auto lowered_ascii = to_lower_ascii(user_prompt);
    const std::vector<std::string> english_keywords = {
        "type", "enter", "input", "write", "fill", "search", "find", "edit", "update",
        "change", "rename", "replace", "set text", "message", "text", "query"
    };
    const std::vector<std::string> russian_keywords = {
        u8"напиши", u8"впиши", u8"введи", u8"ввести", u8"ввод", u8"поиск", u8"поиске",
        u8"найди", u8"найти", u8"измени", u8"изменить", u8"поменяй", u8"поменять",
        u8"замени", u8"заменить", u8"отредактируй", u8"редактируй", u8"переименуй",
        u8"назови", u8"текст", u8"сообщение"
    };

    return has_quoted_text_payload(user_prompt) ||
        contains_any_substring(lowered_ascii, english_keywords) ||
        contains_any_substring(user_prompt, russian_keywords);
}

json prepare_for_toon(const json & grouped) {
    json cleaned = grouped;
    for (auto it = cleaned.begin(); it != cleaned.end(); ++it) {
        if (!it.value().is_array()) {
            continue;
        }
        for (auto & item : it.value()) {
            if (!item.is_object()) {
                continue;
            }
            item.erase("path");
            item.erase("class");
        }
    }
    return cleaned;
}

std::string json_to_toon(const json & prepared) {
    std::string out;
    for (auto group_it = prepared.begin(); group_it != prepared.end(); ++group_it) {
        out += group_it.key();
        out += ":\n";

        std::vector<std::string> columns;
        std::unordered_set<std::string> seen_columns;

        if (group_it.value().is_array()) {
            for (const auto & item : group_it.value()) {
                if (!item.is_object()) {
                    continue;
                }
                for (auto item_it = item.begin(); item_it != item.end(); ++item_it) {
                    if (seen_columns.insert(item_it.key()).second) {
                        columns.push_back(item_it.key());
                    }
                }
            }
        }

        out += "  ";
        out += join_cells(columns);
        out += "\n";

        if (group_it.value().is_array()) {
            for (const auto & item : group_it.value()) {
                std::vector<std::string> row;
                row.reserve(columns.size());
                for (const auto & column : columns) {
                    const auto value_it = item.find(column);
                    row.push_back(value_it == item.end() ? "" : stringify_cell(*value_it));
                }
                out += "  ";
                out += join_cells(row);
                out += "\n";
            }
        }
    }
    return out;
}

std::string find_path_json_by_id(const json & grouped, int32_t selected_id) {
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        if (!it.value().is_array()) {
            continue;
        }
        for (const auto & item : it.value()) {
            const auto id_it = item.find("id");
            const auto path_it = item.find("path");
            if (id_it == item.end() || path_it == item.end()) {
                continue;
            }
            if (id_it->is_number_integer() && id_it->get<int32_t>() == selected_id) {
                return path_it->dump();
            }
        }
    }
    return "";
}

static std::vector<int32_t> collect_candidate_ids(const json & grouped) {
    std::vector<int32_t> ids;
    std::unordered_set<int32_t> seen;
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        if (!it.value().is_array()) {
            continue;
        }
        for (const auto & item : it.value()) {
            const auto id_it = item.find("id");
            if (id_it == item.end() || !id_it->is_number_integer()) {
                continue;
            }
            const int32_t id = id_it->get<int32_t>();
            if (seen.insert(id).second) {
                ids.push_back(id);
            }
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

static std::vector<int32_t> collect_candidate_ids_for_attr(const json & grouped, const char * attr_name) {
    std::vector<int32_t> ids;
    std::unordered_set<int32_t> seen;
    const auto attr_it = grouped.find(attr_name);
    if (attr_it == grouped.end() || !attr_it->is_array()) {
        return ids;
    }

    for (const auto & item : *attr_it) {
        const auto id_it = item.find("id");
        if (id_it == item.end() || !id_it->is_number_integer()) {
            continue;
        }
        const int32_t id = id_it->get<int32_t>();
        if (seen.insert(id).second) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

static std::string build_id_rule(const std::vector<int32_t> & ids) {
    std::string id_rule;
    for (size_t index = 0; index < ids.size(); ++index) {
        if (index != 0) {
            id_rule += " | ";
        }
        id_rule += "\"";
        id_rule += std::to_string(ids[index]);
        id_rule += "\"";
    }
    if (id_rule.empty()) {
        id_rule = "\"0\"";
    }
    return id_rule;
}

std::string build_action_response_grammar(
    const std::vector<int32_t> & ids,
    const std::vector<int32_t> & editable_ids,
    bool allow_click
) {
    const std::string click_id_rule = build_id_rule(ids);
    const std::string editable_id_rule = build_id_rule(editable_ids);
    std::string grammar;
    grammar += "root ::= ws (no-match-response";
    if (allow_click && !ids.empty()) {
        grammar += " | click-response";
    }
    if (!editable_ids.empty()) {
        grammar += " | set-text-response";
    }
    grammar += ") ws\n";
    grammar += "no-match-response ::= \"{\" ws \"\\\"id\\\"\" ws \":\" ws \"-1\" ws \"}\"\n";
    if (allow_click && !ids.empty()) {
        grammar += "click-response ::= \"{\" ws \"\\\"id\\\"\" ws \":\" ws click-id-value ws \",\" ws \"\\\"action\\\"\" ws \":\" ws \"\\\"click\\\"\" ws \"}\"\n";
        grammar += "click-id-value ::= ";
        grammar += click_id_rule;
        grammar += "\n";
    }
    if (!editable_ids.empty()) {
        grammar += "set-text-response ::= \"{\" ws \"\\\"id\\\"\" ws \":\" ws set-text-id-value ws \",\" ws \"\\\"action\\\"\" ws \":\" ws \"\\\"set_text\\\"\" ws \",\" ws \"\\\"text\\\"\" ws \":\" ws json-string ws \"}\"\n";
        grammar += "set-text-id-value ::= ";
        grammar += editable_id_rule;
        grammar += "\n";
    }
    grammar += "json-string ::= \"\\\"\" json-char* \"\\\"\"\n";
    grammar += "json-char ::= [^\"\\\\\\x0A\\x0D] | escape\n";
    grammar += "escape ::= \"\\\\\" ([\"\\\\/bfnrt] | (\"u\" hex hex hex hex))\n";
    grammar += "hex ::= [0-9a-fA-F]\n";
    grammar += "ws ::= | \" \" ws | \"\\n\" ws | \"\\r\" ws | \"\\t\" ws\n";
    return grammar;
}

model_action_response extract_action_response_from_chat_response(const std::string & response_body) {
    return extract_action_response_from_chat_response_with_content(response_body, nullptr);
}

void validate_action_response_for_grouped(const json & grouped, const model_action_response & response) {
    if (response.selected_id < 0 || response.action_type.empty()) {
        return;
    }

    if (response.action_type == "click") {
        return;
    }

    if (response.action_type != "set_text") {
        throw std::runtime_error("model response action must be click or set_text");
    }

    if (!response.text.has_value() || trim_copy(*response.text).empty()) {
        throw std::runtime_error("model response set_text action requires non-empty text");
    }

    const auto editable_ids = collect_candidate_ids_for_attr(grouped, "editable");
    if (std::find(editable_ids.begin(), editable_ids.end(), response.selected_id) == editable_ids.end()) {
        throw std::runtime_error("model returned set_text for non-editable id");
    }
}

} // namespace loki_action

namespace {

const char * duplicate_c_string_global(const std::string & value) {
    char * buffer = static_cast<char *>(std::malloc(value.size() + 1));
    if (buffer == nullptr) {
        return nullptr;
    }
    std::memcpy(buffer, value.c_str(), value.size() + 1);
    return buffer;
}

loki_action_result_t * make_result_global(
    loki_action_status_t status,
    int32_t selected_id,
    const std::string & path_json,
    const std::string & error_message,
    const std::optional<std::string> & action_type = std::nullopt,
    const std::optional<std::string> & text = std::nullopt
) {
    auto * result = static_cast<loki_action_result_t *>(std::calloc(1, sizeof(loki_action_result_t)));
    if (result == nullptr) {
        return nullptr;
    }
    result->status = status;
    result->selected_id = selected_id;
    result->path_json = duplicate_c_string_global(path_json);
    result->error_message = duplicate_c_string_global(error_message);
    result->action_type = action_type ? duplicate_c_string_global(*action_type) : nullptr;
    result->text = text ? duplicate_c_string_global(*text) : nullptr;
    if ((result->path_json == nullptr && !path_json.empty()) ||
        (result->error_message == nullptr && !error_message.empty()) ||
        (action_type.has_value() && result->action_type == nullptr) ||
        (text.has_value() && result->text == nullptr)) {
        std::free(const_cast<char *>(result->path_json));
        std::free(const_cast<char *>(result->error_message));
        std::free(const_cast<char *>(result->action_type));
        std::free(const_cast<char *>(result->text));
        std::free(result);
        return nullptr;
    }
    return result;
}

loki_action::json build_chat_request_payload_global(
    const std::string & user_prompt,
    const std::string & screen_name,
    const std::string & toon,
    const std::string & grammar,
    const char * system_prompt
) {
    return loki_action::json{
        {"model", "default"},
        {"messages", loki_action::json::array({
            loki_action::json{
                {"role", "system"},
                {"content", system_prompt},
            },
            loki_action::json{{"role", "user"}, {"content", loki_action::build_user_content(user_prompt, screen_name, toon)}},
        })},
        {"grammar", grammar},
        {"cache_prompt", true},
        {"max_tokens", 96},
        {"stream", false},
        {"temperature", 0.0},
        {"top_k", 1},
        {"top_p", 1.0},
        {"min_p", 0.0},
        {"repeat_penalty", 1.0},
    };
}

} // namespace

extern "C" {

LOKI_ACTION_API loki_action_result_t * loki_action_resolve_path(
    const char * user_prompt,
    const char * screen_json,
    const char * host,
    int32_t port
) {
    if (user_prompt == nullptr || screen_json == nullptr) {
        return make_result_global(
            LOKI_ACTION_STATUS_INVALID_INPUT,
            -1,
            "[]",
            "user_prompt and screen_json are required"
        );
    }

    std::string stage = "init";
    std::string last_model_response;

    try {
        stage = "copy_inputs";
        const std::string user_prompt_copy(user_prompt);
        const std::string screen_json_copy(screen_json);
        const std::string host_copy = host != nullptr ? std::string(host) : std::string("127.0.0.1");
        const int32_t resolved_port = port > 0 ? port : 8080;

        stage = "parse_screen_json";
        const auto screen = loki_action::json::parse(screen_json_copy);
        const auto root_it = screen.find("root");
        if (root_it == screen.end() || !root_it->is_object()) {
            return make_result_global(
                LOKI_ACTION_STATUS_INVALID_INPUT,
                -1,
                "[]",
                "screen_json must contain object root"
            );
        }

        stage = "group_candidates";
        const auto grouped = loki_action::group_by_attrs_textual(*root_it);
        if (grouped.empty()) {
            return make_result_global(
                LOKI_ACTION_STATUS_NO_CANDIDATES,
                -1,
                "[]",
                "no interactive candidates found"
            );
        }

        stage = "build_toon";
        const auto prepared = loki_action::prepare_for_toon(grouped);
        const auto full_toon = loki_action::json_to_toon(prepared);
        loki_action::log_multiline_toon(full_toon);
        const auto candidate_ids = loki_action::collect_candidate_ids(grouped);
        const auto editable_candidate_ids = loki_action::collect_candidate_ids_for_attr(grouped, "editable");
        std::vector<int32_t> non_editable_candidate_ids;
        non_editable_candidate_ids.reserve(candidate_ids.size());
        {
            const std::unordered_set<int32_t> editable_set(editable_candidate_ids.begin(), editable_candidate_ids.end());
            for (const int32_t id : candidate_ids) {
                if (editable_set.find(id) == editable_set.end()) {
                    non_editable_candidate_ids.push_back(id);
                }
            }
        }

        const auto screen_name_it = screen.find("screen");
        const std::string screen_name =
            screen_name_it != screen.end() && screen_name_it->is_string()
                ? screen_name_it->get<std::string>()
                : "unknown";

        auto run_model_pass = [&](const char * pass_name,
                                  const char * system_prompt,
                                  const std::vector<int32_t> & pass_ids,
                                  const std::vector<int32_t> & pass_editable_ids,
                                  bool allow_click) -> std::optional<loki_action::model_action_response> {
            if (pass_ids.empty()) {
                LOKI_LOGI("MODEL PASS: %s skipped (no candidates)", pass_name);
                return std::nullopt;
            }

            const std::string grammar = loki_action::build_action_response_grammar(
                pass_ids,
                pass_editable_ids,
                allow_click
            );

            LOKI_LOGI(
                "MODEL PASS: %s ids=%zu editable_ids=%zu allow_click=%s",
                pass_name,
                pass_ids.size(),
                pass_editable_ids.size(),
                allow_click ? "true" : "false"
            );
            LOKI_LOGI(
                "MODEL GRAMMAR: \"%s\"",
                loki_action::truncate_for_log(loki_action::escape_for_log(grammar), 400).c_str()
            );

            stage = std::string("prepare_http:") + pass_name;
            const auto payload = build_chat_request_payload_global(
                user_prompt_copy,
                screen_name,
                full_toon,
                grammar,
                system_prompt
            );
            const std::string payload_body = payload.dump();
            LOKI_LOGI(
                "MODEL REQUEST: pass=%s body=\"%s\"",
                pass_name,
                loki_action::truncate_for_log(loki_action::escape_for_log(payload_body)).c_str()
            );

            stage = std::string("http_post:") + pass_name;
            const auto http_result = loki_action::post_json_via_socket(
                host_copy,
                resolved_port,
                "/v1/chat/completions",
                payload_body,
                5,
                120,
                5
            );

            if (!http_result.ok) {
                const std::string detailed_error =
                    std::string("http_post failed: ") + http_result.error +
                    "; request=\"" +
                    loki_action::truncate_for_log(loki_action::escape_for_log(payload_body), 400) +
                    "\"";
                LOKI_LOGE("%s", detailed_error.c_str());
                throw std::runtime_error(detailed_error);
            }

            stage = std::string("handle_http_response:") + pass_name;
            const std::string response_body = http_result.body;
            last_model_response = response_body;
            LOKI_LOGI(
                "MODEL RESPONSE: pass=%s status=%d body=\"%s\"",
                pass_name,
                http_result.status,
                loki_action::truncate_for_log(loki_action::escape_for_log(response_body)).c_str()
            );

            if (http_result.status != 200) {
                throw std::runtime_error(
                    response_body.empty()
                        ? std::string("llama.cpp server returned HTTP ") + std::to_string(http_result.status)
                        : response_body
                );
            }

            stage = std::string("parse_model_response:") + pass_name;
            std::string normalized_content;
            auto action_response = loki_action::extract_action_response_from_chat_response_with_content(
                response_body,
                &normalized_content
            );
            LOKI_LOGI(
                "MODEL CONTENT: pass=%s \"%s\"",
                pass_name,
                loki_action::truncate_for_log(loki_action::escape_for_log(normalized_content), 400).c_str()
            );
            LOKI_LOGI(
                "PARSED ACTION: pass=%s id=%d action=\"%s\" text=\"%s\"",
                pass_name,
                action_response.selected_id,
                action_response.action_type.c_str(),
                action_response.text
                    ? loki_action::truncate_for_log(loki_action::escape_for_log(*action_response.text), 400).c_str()
                    : ""
            );
            return action_response;
        };

        loki_action::model_action_response action_response;
        bool has_action_response = false;
        const bool prefers_text_edit = loki_action::prompt_requests_text_edit(user_prompt_copy);
        LOKI_LOGI(
            "PROMPT MODE: prefers_text_edit=%s editable_candidates=%zu total_candidates=%zu",
            prefers_text_edit ? "true" : "false",
            editable_candidate_ids.size(),
            candidate_ids.size()
        );

        try {
            if (prefers_text_edit && !editable_candidate_ids.empty()) {
                const auto editable_response = run_model_pass(
                    "editable-priority",
                    loki_action::EDITABLE_PRIORITY_PROMPT,
                    editable_candidate_ids,
                    editable_candidate_ids,
                    false
                );
                if (editable_response.has_value() && editable_response->selected_id >= 0) {
                    action_response = *editable_response;
                    has_action_response = true;
                }
            }

            if (!has_action_response) {
                const bool fallback_to_non_editable = prefers_text_edit;
                const auto & fallback_ids = fallback_to_non_editable ? non_editable_candidate_ids : candidate_ids;
                std::vector<int32_t> fallback_editable_ids;
                if (!fallback_to_non_editable) {
                    fallback_editable_ids = editable_candidate_ids;
                }
                const auto fallback_response = run_model_pass(
                    fallback_to_non_editable ? "non-editable-fallback" : "default",
                    fallback_to_non_editable ? loki_action::CLICK_FALLBACK_PROMPT : loki_action::SYSTEM_PROMPT,
                    fallback_ids,
                    fallback_editable_ids,
                    true
                );
                if (fallback_response.has_value()) {
                    action_response = *fallback_response;
                    has_action_response = true;
                }
            }
        } catch (const std::bad_cast &) {
            const std::string detailed_error =
                std::string("bad_cast while parsing model response; raw=\"") +
                loki_action::truncate_for_log(loki_action::escape_for_log(last_model_response), 400) +
                "\"";
            LOKI_LOGE("%s", detailed_error.c_str());
            return make_result_global(
                LOKI_ACTION_STATUS_INVALID_RESPONSE,
                -1,
                "[]",
                detailed_error
            );
        } catch (const std::exception & e) {
            const std::string detailed_error =
                std::string(e.what()) +
                "; raw=\"" +
                loki_action::truncate_for_log(loki_action::escape_for_log(last_model_response), 400) +
                "\"";
            LOKI_LOGE("failed to resolve model action: %s", detailed_error.c_str());
            const bool is_http = detailed_error.find("http_post failed:") != std::string::npos ||
                detailed_error.find("llama.cpp server returned HTTP ") != std::string::npos;
            return make_result_global(
                is_http ? LOKI_ACTION_STATUS_HTTP_ERROR : LOKI_ACTION_STATUS_INVALID_RESPONSE,
                -1,
                "[]",
                detailed_error
            );
        }

        if (!has_action_response) {
            return make_result_global(
                LOKI_ACTION_STATUS_ID_NOT_FOUND,
                -1,
                "[]",
                "model returned no matching id"
            );
        }

        if (action_response.selected_id < 0) {
            return make_result_global(
                LOKI_ACTION_STATUS_ID_NOT_FOUND,
                action_response.selected_id,
                "[]",
                "model returned no matching id"
            );
        }

        const auto path_json = loki_action::find_path_json_by_id(grouped, action_response.selected_id);
        if (path_json.empty()) {
            return make_result_global(
                LOKI_ACTION_STATUS_ID_NOT_FOUND,
                action_response.selected_id,
                "[]",
                "selected id not found in path map"
            );
        }

        stage = "validate_action_response";
        try {
            loki_action::validate_action_response_for_grouped(grouped, action_response);
        } catch (const std::exception & e) {
            const std::string detailed_error =
                std::string(e.what()) +
                "; action=\"" +
                loki_action::escape_for_log(action_response.action_type) +
                "\"";
            LOKI_LOGE("invalid action response: %s", detailed_error.c_str());
            return make_result_global(
                LOKI_ACTION_STATUS_INVALID_RESPONSE,
                action_response.selected_id,
                "[]",
                detailed_error
            );
        }

        stage = "return_success";
        return make_result_global(
            LOKI_ACTION_STATUS_OK,
            action_response.selected_id,
            path_json,
            "",
            action_response.action_type,
            action_response.text
        );
    } catch (const std::bad_cast & e) {
        const std::string detailed_error =
            std::string("std::bad_cast at stage=") + stage +
            "; raw=\"" +
            loki_action::truncate_for_log(loki_action::escape_for_log(last_model_response), 400) +
            "\"";
        LOKI_LOGE("%s", detailed_error.c_str());
        return make_result_global(
            LOKI_ACTION_STATUS_INVALID_INPUT,
            -1,
            "[]",
            detailed_error
        );
    } catch (const std::exception & e) {
        const std::string detailed_error =
            std::string(e.what()) +
            "; stage=" + stage +
            "; raw=\"" +
            loki_action::truncate_for_log(loki_action::escape_for_log(last_model_response), 400) +
            "\"";
        LOKI_LOGE("resolve failed: %s", detailed_error.c_str());
        return make_result_global(
            LOKI_ACTION_STATUS_INVALID_INPUT,
            -1,
            "[]",
            detailed_error
        );
    }
}

LOKI_ACTION_API void loki_action_result_destroy(loki_action_result_t * result) {
    if (result == nullptr) {
        return;
    }
    std::free(const_cast<char *>(result->path_json));
    std::free(const_cast<char *>(result->error_message));
    std::free(const_cast<char *>(result->action_type));
    std::free(const_cast<char *>(result->text));
    std::free(result);
}

LOKI_ACTION_API const char * loki_action_get_version(void) {
    return "1.1.2";
}

} // extern "C"

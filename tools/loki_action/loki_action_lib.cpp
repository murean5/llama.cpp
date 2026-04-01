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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loki_action {

namespace {

constexpr const char * SYSTEM_PROMPT =
    "Choose the next Android UI step from task, history, and current UI. "
    "Reply only as JSON. "
    "Use checked/unchecked rows as current toggle state when deciding done or next click. "
    "Return {\"done\":true} only when the final goal is already visibly satisfied, never on intermediate app, search, or form screens. "
    "Otherwise return {\"id\":<id>,\"action\":\"click\",\"done\":false} or {\"id\":<id>,\"action\":\"set_text\",\"text\":\"...\",\"done\":false}. "
    "Use set_text only for visible editable ids and return only the exact value to type. "
    "If a visible editable search field can directly narrow to the requested item, prefer set_text over clicking that same field again. "
    "Use history to avoid loops and repeated recent ids. "
    "Use {\"id\":-1} if no fit.";

constexpr const char * EDITABLE_PRIORITY_PROMPT =
    "Only visible editable ids are available. "
    "Return {\"done\":true} only when the final goal is already visibly satisfied, not before typing or editing. "
    "Otherwise return only {\"id\":<id>,\"action\":\"set_text\",\"text\":\"...\",\"done\":false}. "
    "Return only the exact value to type. "
    "If this is a search field, prefer entering the entity name instead of clicking again. "
    "Use history to avoid loops. "
    "Use {\"id\":-1} if no fit.";

constexpr const char * CLICK_FALLBACK_PROMPT =
    "No editable match was chosen. "
    "Return {\"done\":true} only when the final goal is already visibly satisfied, not on intermediate screens. "
    "Otherwise return only {\"id\":<id>,\"action\":\"click\",\"done\":false}. "
    "Use history to avoid loops and repeated recent ids. Do not click the same search field again if typing there is the correct next step. "
    "Use {\"id\":-1} if no fit.";

constexpr const char * DIRECT_CLICK_PRIORITY_PROMPT =
    "These clickable ids already match the task target by visible text/content description. "
    "Prefer a direct click on that target instead of opening or typing into search. "
    "Return {\"done\":true} only when the goal is already completed. "
    "Otherwise return only {\"id\":<id>,\"action\":\"click\",\"done\":false}. "
    "Use {\"id\":-1} if no fit.";

constexpr const char * DONE_CHECK_PROMPT =
    "Decide completion only. "
    "Use checked/unchecked rows as explicit current state for toggles and options. "
    "Return {\"done\":true} if the final user goal is already visibly satisfied on this screen. "
    "Otherwise return {\"id\":-1}.";

constexpr const char * STATE_PRIORITY_PROMPT =
    "State change task detected. "
    "Use checked/unchecked rows to understand current state and choose one best state-related click target. "
    "Return {\"done\":true} only if requested state is already satisfied now. "
    "Otherwise return only {\"id\":<id>,\"action\":\"click\",\"done\":false}. "
    "Use {\"id\":-1} if no fit.";

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

struct prompt_context {
    std::string task;
    std::vector<std::string> history_tokens;
    std::vector<std::string> history_labels;
    int step_number = 1;
    bool has_loop_hint = false;
    std::string loop_hint;
    int repeated_tail_clicks = 0;
};

bool starts_with_literal(const std::string & value, const std::string & prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string trim_action_prefix(const std::string & history_line) {
    size_t pos = 0;
    while (pos < history_line.size() &&
           (std::isdigit(static_cast<unsigned char>(history_line[pos])) != 0 ||
            history_line[pos] == '.' || std::isspace(static_cast<unsigned char>(history_line[pos])) != 0)) {
        ++pos;
    }
    return trim_copy(history_line.substr(pos));
}

std::string compact_history_label(const std::string & action_line, char action_code) {
    std::string label;
    const auto colon = action_line.find(':');
    if (colon != std::string::npos && colon + 1 < action_line.size()) {
        label = trim_copy(action_line.substr(colon + 1));
    } else {
        label = trim_copy(action_line);
    }

    if (label.empty()) {
        return "";
    }

    if (label.size() > 36) {
        label = label.substr(0, 36);
    }
    return std::string(1, action_code) + ":" + label;
}

prompt_context parse_prompt_context(const std::string & raw_prompt) {
    prompt_context context;
    context.task = trim_copy(raw_prompt);

    bool in_history = false;
    size_t cursor = 0;
    while (cursor <= raw_prompt.size()) {
        const size_t line_end = raw_prompt.find('\n', cursor);
        const std::string line = raw_prompt.substr(
            cursor,
            line_end == std::string::npos ? std::string::npos : line_end - cursor
        );
        const std::string trimmed = trim_copy(line);

        if (starts_with_literal(trimmed, u8"Команда пользователя:")) {
            context.task = trim_copy(trimmed.substr(std::strlen(u8"Команда пользователя:")));
        } else if (starts_with_literal(trimmed, "User command:")) {
            context.task = trim_copy(trimmed.substr(std::strlen("User command:")));
        } else if (starts_with_literal(trimmed, "Task:")) {
            context.task = trim_copy(trimmed.substr(std::strlen("Task:")));
        } else if (trimmed == u8"Уже выполненные действия:" || trimmed == "Completed actions:" || trimmed == "History:") {
            in_history = true;
        } else if (in_history) {
            if (starts_with_literal(trimmed, u8"Определи следующее действие") ||
                starts_with_literal(trimmed, "Determine the next action") ||
                starts_with_literal(trimmed, "Next action")) {
                in_history = false;
            } else if (!trimmed.empty() && std::isdigit(static_cast<unsigned char>(trimmed[0])) != 0) {
                const auto action_line = trim_action_prefix(trimmed);
                char action_code = 'a';
                if (action_line.find("set_text") != std::string::npos) {
                    action_code = 't';
                } else if (action_line.find("click") != std::string::npos) {
                    action_code = 'c';
                }
                context.history_tokens.emplace_back(1, action_code);

                const auto label = compact_history_label(action_line, action_code);
                if (!label.empty()) {
                    context.history_labels.push_back(label);
                }
            }
        }

        if (line_end == std::string::npos) {
            break;
        }
        cursor = line_end + 1;
    }

    context.step_number = static_cast<int>(context.history_tokens.size()) + 1;
    if (context.history_tokens.size() >= 4) {
        const size_t n = context.history_tokens.size();
        if (context.history_tokens[n - 1] == context.history_tokens[n - 3] &&
            context.history_tokens[n - 2] == context.history_tokens[n - 4]) {
            context.has_loop_hint = true;
            context.loop_hint = context.history_tokens[n - 2] + ">" + context.history_tokens[n - 1];
        }
    }
    for (auto it = context.history_tokens.rbegin(); it != context.history_tokens.rend(); ++it) {
        if (*it != "c") {
            break;
        }
        context.repeated_tail_clicks += 1;
    }

    return context;
}

std::string build_user_content(
    const std::string & user_prompt,
    const std::string & screen_name,
    const std::string & toon
) {
    const auto context = parse_prompt_context(user_prompt);
    std::string user_content;
    user_content.reserve(user_prompt.size() + screen_name.size() + toon.size() + 64);
    user_content += "Task: ";
    user_content += context.task;
    if (!context.history_tokens.empty()) {
        user_content += "\nHist:";
        const size_t keep = 6;
        const size_t start = context.history_tokens.size() > keep ? context.history_tokens.size() - keep : 0;
        if (start > 0) {
            user_content += " ...";
        }
        for (size_t i = start; i < context.history_tokens.size(); ++i) {
            if (i != start) {
                user_content += ">";
            }
            user_content += context.history_tokens[i];
        }
    }
    if (!context.history_labels.empty()) {
        user_content += "\nHistLbl:";
        const size_t keep = 4;
        const size_t start = context.history_labels.size() > keep ? context.history_labels.size() - keep : 0;
        if (start > 0) {
            user_content += " ...";
        }
        for (size_t i = start; i < context.history_labels.size(); ++i) {
            if (i != start) {
                user_content += ">";
            }
            user_content += context.history_labels[i];
        }
    }
    user_content += "\nStep:";
    user_content += std::to_string(context.step_number);
    if (context.has_loop_hint) {
        user_content += "\nLoop:";
        user_content += context.loop_hint;
    }
    if (context.repeated_tail_clicks >= 2) {
        user_content += "\nRepeat:clickx";
        user_content += std::to_string(context.repeated_tail_clicks);
    }
    user_content += "\nApp: ";
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

uint32_t decode_utf8_codepoint(const std::string & input, size_t & cursor) {
    const unsigned char lead = static_cast<unsigned char>(input[cursor]);
    if (lead < 0x80) {
        ++cursor;
        return lead;
    }

    const auto remain = [&](size_t need) {
        return cursor + need <= input.size();
    };

    if ((lead & 0xE0) == 0xC0 && remain(2)) {
        const unsigned char b1 = static_cast<unsigned char>(input[cursor + 1]);
        if ((b1 & 0xC0) == 0x80) {
            const uint32_t cp = static_cast<uint32_t>(((lead & 0x1F) << 6) | (b1 & 0x3F));
            cursor += 2;
            return cp;
        }
    } else if ((lead & 0xF0) == 0xE0 && remain(3)) {
        const unsigned char b1 = static_cast<unsigned char>(input[cursor + 1]);
        const unsigned char b2 = static_cast<unsigned char>(input[cursor + 2]);
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            const uint32_t cp = static_cast<uint32_t>(
                ((lead & 0x0F) << 12) |
                ((b1 & 0x3F) << 6) |
                (b2 & 0x3F)
            );
            cursor += 3;
            return cp;
        }
    } else if ((lead & 0xF8) == 0xF0 && remain(4)) {
        const unsigned char b1 = static_cast<unsigned char>(input[cursor + 1]);
        const unsigned char b2 = static_cast<unsigned char>(input[cursor + 2]);
        const unsigned char b3 = static_cast<unsigned char>(input[cursor + 3]);
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            const uint32_t cp = static_cast<uint32_t>(
                ((lead & 0x07) << 18) |
                ((b1 & 0x3F) << 12) |
                ((b2 & 0x3F) << 6) |
                (b3 & 0x3F)
            );
            cursor += 4;
            return cp;
        }
    }

    ++cursor;
    return lead;
}

void append_utf8_codepoint(std::string & out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
        return;
    }
    if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        return;
    }
    out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

uint32_t lowercase_codepoint(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32;
    }
    if (cp >= 0x0410 && cp <= 0x042F) {
        return cp + 32;
    }
    if (cp == 0x0401) {
        return 0x0451;
    }
    return cp;
}

std::string to_lower_basic_multilang(const std::string & input) {
    std::string out;
    out.reserve(input.size());
    size_t cursor = 0;
    while (cursor < input.size()) {
        const uint32_t cp = decode_utf8_codepoint(input, cursor);
        append_utf8_codepoint(out, lowercase_codepoint(cp));
    }
    return out;
}

bool ends_with_literal(const std::string & value, const std::string & suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> split_terms_for_matching(const std::string & value) {
    std::vector<std::string> terms;
    std::string current;
    current.reserve(24);

    for (const unsigned char ch : value) {
        const bool keep = (ch >= 0x80) || (std::isalnum(ch) != 0);
        if (keep) {
            current.push_back(static_cast<char>(ch));
            continue;
        }
        if (!current.empty()) {
            terms.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        terms.push_back(current);
    }
    return terms;
}

std::string normalize_match_term(std::string term) {
    static const std::unordered_set<std::string> stop_words = {
        "task", "history", "step", "goal", "next", "action", "done",
        "open", "find", "search", "lookup", "look", "contact", "contacts", "video",
        "videos", "channel", "song", "playlist", "person", "message", "text",
        "please", "with", "from", "into", "for", "the", "and", "or", "to", "in", "on",
        u8"задача", u8"история", u8"шаг", u8"цель",
        u8"открой", u8"открыть", u8"найди", u8"найти", u8"поиск", u8"в", u8"во",
        u8"на", u8"с", u8"со", u8"к", u8"ко", u8"по", u8"и", u8"или", u8"мне",
        u8"контакт", u8"контакты", u8"контакта", u8"контактов", u8"видео", u8"канал",
        u8"плейлист", u8"песня", u8"песню", u8"песни", u8"человек", u8"номер", u8"телефон"
    };

    if (term.size() < 3 || stop_words.find(term) != stop_words.end()) {
        return "";
    }

    static const std::vector<std::string> russian_suffixes = {
        u8"ями", u8"ами", u8"ого", u8"ему", u8"ому", u8"ыми", u8"ими",
        u8"ой", u8"ей", u8"ам", u8"ям", u8"ах", u8"ях", u8"ом", u8"ем",
        u8"ую", u8"юю", u8"а", u8"я", u8"у", u8"ю", u8"е", u8"ы", u8"и"
    };
    for (const auto & suffix : russian_suffixes) {
        if (term.size() > suffix.size() + 2 && ends_with_literal(term, suffix)) {
            term.resize(term.size() - suffix.size());
            break;
        }
    }

    if (term.size() < 3 || stop_words.find(term) != stop_words.end()) {
        return "";
    }
    return term;
}

std::vector<std::string> extract_task_match_terms(const std::string & task) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;

    const std::string lowered = to_lower_basic_multilang(task);
    const auto raw_terms = split_terms_for_matching(lowered);
    for (const auto & raw_term : raw_terms) {
        const auto normalized = normalize_match_term(raw_term);
        if (normalized.empty()) {
            continue;
        }
        if (seen.insert(normalized).second) {
            result.push_back(normalized);
        }
    }
    return result;
}

void log_multiline_toon(const std::string & toon, int step_number) {
#if !defined(ANDROID)
    (void) step_number;
#endif
    if (toon.empty()) {
        LOKI_LOGI("STEP %d TOON: <empty>", step_number);
        return;
    }

    constexpr size_t chunk_size = 300;
    LOKI_LOGI("STEP %d TOON BEGIN", step_number);
    for (size_t i = 0; i < toon.size(); i += chunk_size) {
        const auto chunk = toon.substr(i, std::min(chunk_size, toon.size() - i));
        LOKI_LOGI("STEP %d TOON[%zu]: %s", step_number, i, chunk.c_str());
    }
    LOKI_LOGI("STEP %d TOON END", step_number);
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

bool parse_done_field(const json & value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_string()) {
        const auto lowered = to_lower_ascii(trim_copy(value.get<std::string>()));
        if (lowered == "true") {
            return true;
        }
        if (lowered == "false") {
            return false;
        }
    }
    throw std::runtime_error("model response done is not a boolean");
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

    const auto done_it = parsed.find("done");
    if (done_it != parsed.end() && parse_done_field(*done_it)) {
        model_action_response result;
        result.done = true;
        return result;
    }

    const auto id_it = parsed.find("id");
    if (id_it == parsed.end()) {
        throw std::runtime_error("model response is missing id");
    }

    model_action_response result;
    result.selected_id = parse_action_id_field(*id_it);
    result.done = false;
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
        "type", "enter", "input", "write", "fill", "edit", "update",
        "change", "rename", "replace", "set text", "message", "paste"
    };
    const std::vector<std::string> russian_keywords = {
        u8"напиши", u8"впиши", u8"введи", u8"ввести", u8"ввод", u8"измени", u8"изменить", u8"поменяй", u8"поменять",
        u8"замени", u8"заменить", u8"отредактируй", u8"редактируй", u8"переименуй",
        u8"назови", u8"вставь", u8"вставить"
    };

    return has_quoted_text_payload(user_prompt) ||
        contains_any_substring(lowered_ascii, english_keywords) ||
        contains_any_substring(user_prompt, russian_keywords);
}

static bool prompt_requests_lookup_input(const std::string & user_prompt) {
    const auto lowered_ascii = to_lower_ascii(user_prompt);
    const std::vector<std::string> english_keywords = {
        "find", "open", "search", "look up", "lookup", "contact", "video", "channel", "song", "playlist", "person"
    };
    const std::vector<std::string> russian_keywords = {
        u8"найди", u8"найти", u8"открой", u8"открыть", u8"поиск", u8"контакт", u8"видео",
        u8"канал", u8"песн", u8"плейлист", u8"челов", u8"маму", u8"мама"
    };

    return contains_any_substring(lowered_ascii, english_keywords) ||
        contains_any_substring(user_prompt, russian_keywords);
}

static bool prompt_requests_state_change(const std::string & user_prompt) {
    const auto lowered_ascii = to_lower_ascii(user_prompt);
    const std::vector<std::string> english_keywords = {
        "turn on", "turn off", "enable", "disable", "switch on", "switch off",
        "toggle", "check", "uncheck", "activate", "deactivate"
    };
    const std::vector<std::string> russian_keywords = {
        u8"включи", u8"включить", u8"выключи", u8"выключить",
        u8"активируй", u8"деактивируй", u8"переключи", u8"переключить",
        u8"отметь", u8"сними отметку", u8"галочка"
    };

    return contains_any_substring(lowered_ascii, english_keywords) ||
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

static std::vector<int32_t> collect_state_candidate_ids(const json & grouped) {
    std::unordered_set<int32_t> unique;
    for (const auto id : collect_candidate_ids_for_attr(grouped, "checked")) {
        unique.insert(id);
    }
    for (const auto id : collect_candidate_ids_for_attr(grouped, "unchecked")) {
        unique.insert(id);
    }

    std::vector<int32_t> ids(unique.begin(), unique.end());
    std::sort(ids.begin(), ids.end());
    return ids;
}

struct item_match_stats {
    int score = 0;
    int matched_terms = 0;
    int longest_term = 0;
};

static item_match_stats score_item_for_terms(
    const json & item,
    const std::vector<std::string> & task_terms
) {
    item_match_stats stats;
    if (!item.is_object() || task_terms.empty()) {
        return stats;
    }

    std::string searchable;
    if (const auto text = normalized_field(item, "text")) {
        searchable += *text;
        searchable.push_back(' ');
    }
    if (const auto content_desc = normalized_field(item, "contentDesc")) {
        searchable += *content_desc;
        searchable.push_back(' ');
    }
    searchable = to_lower_basic_multilang(searchable);
    if (searchable.empty()) {
        return stats;
    }

    for (const auto & term : task_terms) {
        if (term.size() < 3) {
            continue;
        }
        if (searchable.find(term) != std::string::npos) {
            stats.score += static_cast<int>(term.size());
            stats.matched_terms += 1;
            stats.longest_term = std::max(stats.longest_term, static_cast<int>(term.size()));
        }
    }
    return stats;
}

static bool item_is_upper_region(const json & item) {
    const auto path_it = item.find("path");
    if (path_it == item.end() || !path_it->is_array() || path_it->empty()) {
        return false;
    }

    int prefix_sum = 0;
    const size_t depth = std::min<size_t>(3, path_it->size());
    for (size_t i = 0; i < depth; ++i) {
        const auto & step = (*path_it)[i];
        if (!step.is_number_integer()) {
            return false;
        }
        prefix_sum += step.get<int>();
    }

    return prefix_sum <= 3;
}

static std::vector<const json *> collect_unique_item_refs(const json & grouped) {
    std::vector<const json *> refs;
    std::unordered_set<int32_t> seen;

    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        if (!it.value().is_array()) {
            continue;
        }
        for (const auto & item : it.value()) {
            if (!item.is_object()) {
                continue;
            }
            const auto id_it = item.find("id");
            if (id_it == item.end() || !id_it->is_number_integer()) {
                continue;
            }
            const int32_t id = id_it->get<int32_t>();
            if (seen.insert(id).second) {
                refs.push_back(&item);
            }
        }
    }
    return refs;
}

static int best_match_score_for_attr(
    const json & grouped,
    const char * attr_name,
    const std::vector<std::string> & task_terms
) {
    int best = 0;
    const auto attr_it = grouped.find(attr_name);
    if (attr_it == grouped.end() || !attr_it->is_array()) {
        return best;
    }

    for (const auto & item : *attr_it) {
        const auto stats = score_item_for_terms(item, task_terms);
        best = std::max(best, stats.score);
    }
    return best;
}

static bool prompt_wants_enabled_state(const std::string & user_prompt) {
    const auto lowered_ascii = to_lower_ascii(user_prompt);
    const std::vector<std::string> english_keywords = {
        "turn on", "enable", "switch on", "check", "activate"
    };
    const std::vector<std::string> russian_keywords = {
        u8"включи", u8"включить", u8"включен", u8"включено", u8"активируй", u8"отметь"
    };

    return contains_any_substring(lowered_ascii, english_keywords) ||
        contains_any_substring(user_prompt, russian_keywords);
}

static bool prompt_wants_disabled_state(const std::string & user_prompt) {
    const auto lowered_ascii = to_lower_ascii(user_prompt);
    const std::vector<std::string> english_keywords = {
        "turn off", "disable", "switch off", "uncheck", "deactivate"
    };
    const std::vector<std::string> russian_keywords = {
        u8"выключи", u8"выключить", u8"выключен", u8"выключено", u8"деактивируй", u8"сними отметку"
    };

    return contains_any_substring(lowered_ascii, english_keywords) ||
        contains_any_substring(user_prompt, russian_keywords);
}

struct done_validation_result {
    bool accepted = false;
    std::string reason = "no strong done signal";
};

static done_validation_result validate_done_response(
    const json & grouped,
    const std::string & task,
    const prompt_context & context
) {
    done_validation_result result;

    if (context.history_tokens.empty()) {
        result.reason = "history empty";
        return result;
    }

    const auto task_terms = extract_task_match_terms(task);
    const bool state_task = prompt_requests_state_change(task);

    if (state_task) {
        const bool wants_on = prompt_wants_enabled_state(task);
        const bool wants_off = prompt_wants_disabled_state(task);
        const int checked_best = best_match_score_for_attr(grouped, "checked", task_terms);
        const int unchecked_best = best_match_score_for_attr(grouped, "unchecked", task_terms);
        const int min_state_score = 4;

        if (wants_on &&
            checked_best >= min_state_score &&
            checked_best >= unchecked_best) {
            result.accepted = true;
            result.reason = "state target is checked";
            return result;
        }
        if (wants_off &&
            unchecked_best >= min_state_score &&
            unchecked_best >= checked_best) {
            result.accepted = true;
            result.reason = "state target is unchecked";
            return result;
        }

        result.reason = "state target not yet reached";
        return result;
    }

    const bool lookup_task = prompt_requests_lookup_input(task);
    if (!lookup_task || task_terms.empty()) {
        result.reason = "no lookup/state completion trigger";
        return result;
    }

    const auto item_refs = collect_unique_item_refs(grouped);
    for (const json * item_ptr : item_refs) {
        const auto stats = score_item_for_terms(*item_ptr, task_terms);
        if (stats.score < 4 || stats.matched_terms == 0 || stats.longest_term < 3) {
            continue;
        }
        if (!item_is_upper_region(*item_ptr)) {
            continue;
        }

        result.accepted = true;
        result.reason = "target visible in upper region";
        return result;
    }

    result.reason = "target not in upper region";
    return result;
}

static int score_clickable_item_for_terms(
    const json & item,
    const std::vector<std::string> & task_terms
) {
    if (!item.is_object() || task_terms.empty()) {
        return 0;
    }

    std::string searchable;
    if (const auto text = normalized_field(item, "text")) {
        searchable += *text;
        searchable.push_back(' ');
    }
    if (const auto content_desc = normalized_field(item, "contentDesc")) {
        searchable += *content_desc;
        searchable.push_back(' ');
    }
    searchable = to_lower_basic_multilang(searchable);
    if (searchable.empty()) {
        return 0;
    }

    int score = 0;
    for (const auto & term : task_terms) {
        if (term.size() < 3) {
            continue;
        }
        if (searchable.find(term) != std::string::npos) {
            score += static_cast<int>(term.size());
        }
    }
    return score;
}

static std::vector<int32_t> collect_direct_click_match_ids(
    const json & grouped,
    const std::string & task
) {
    std::vector<int32_t> ids;
    const auto clickable_it = grouped.find("clickable");
    if (clickable_it == grouped.end() || !clickable_it->is_array()) {
        return ids;
    }

    const auto task_terms = extract_task_match_terms(task);
    if (task_terms.empty()) {
        return ids;
    }

    int best_score = 0;
    for (const auto & item : *clickable_it) {
        const auto id_it = item.find("id");
        if (id_it == item.end() || !id_it->is_number_integer()) {
            continue;
        }

        const int score = score_clickable_item_for_terms(item, task_terms);
        if (score <= 0) {
            continue;
        }

        const int32_t id = id_it->get<int32_t>();
        if (score > best_score) {
            best_score = score;
            ids.clear();
            ids.push_back(id);
        } else if (score == best_score) {
            ids.push_back(id);
        }
    }

    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

static bool item_looks_searchable(const json & item) {
    const auto matches = [](const std::optional<std::string> & value) {
        if (!value.has_value()) {
            return false;
        }
        const auto lowered = to_lower_ascii(*value);
        return lowered.find("search") != std::string::npos ||
            lowered.find("find") != std::string::npos ||
            value->find(u8"поиск") != std::string::npos ||
            value->find(u8"най") != std::string::npos;
    };

    return matches(normalized_field(item, "text")) ||
        matches(normalized_field(item, "contentDesc")) ||
        matches(normalized_field(item, "class"));
}

static bool grouped_has_searchable_editable(const json & grouped) {
    const auto editable_it = grouped.find("editable");
    if (editable_it == grouped.end() || !editable_it->is_array()) {
        return false;
    }

    for (const auto & item : *editable_it) {
        if (item.is_object() && item_looks_searchable(item)) {
            return true;
        }
    }
    return false;
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
    grammar += "root ::= ws (done-response | no-match-response";
    if (allow_click && !ids.empty()) {
        grammar += " | click-response";
    }
    if (!editable_ids.empty()) {
        grammar += " | set-text-response";
    }
    grammar += ") ws\n";
    grammar += "done-response ::= \"{\" ws \"\\\"done\\\"\" ws \":\" ws \"true\" ws \"}\"\n";
    grammar += "no-match-response ::= \"{\" ws \"\\\"id\\\"\" ws \":\" ws \"-1\" ws \"}\"\n";
    if (allow_click && !ids.empty()) {
        grammar += "click-response ::= \"{\" ws \"\\\"id\\\"\" ws \":\" ws click-id-value ws \",\" ws \"\\\"action\\\"\" ws \":\" ws \"\\\"click\\\"\" ws \",\" ws \"\\\"done\\\"\" ws \":\" ws \"false\" ws \"}\"\n";
        grammar += "click-id-value ::= ";
        grammar += click_id_rule;
        grammar += "\n";
    }
    if (!editable_ids.empty()) {
        grammar += "set-text-response ::= \"{\" ws \"\\\"id\\\"\" ws \":\" ws set-text-id-value ws \",\" ws \"\\\"action\\\"\" ws \":\" ws \"\\\"set_text\\\"\" ws \",\" ws \"\\\"text\\\"\" ws \":\" ws json-string ws \",\" ws \"\\\"done\\\"\" ws \":\" ws \"false\" ws \"}\"\n";
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
    if (response.done || response.selected_id < 0 || response.action_type.empty()) {
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
    const std::optional<std::string> & text = std::nullopt,
    bool done = false
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
    result->done = done;
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
        const auto prompt_context = loki_action::parse_prompt_context(user_prompt_copy);
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
        loki_action::log_multiline_toon(full_toon, prompt_context.step_number);
        const auto candidate_ids = loki_action::collect_candidate_ids(grouped);
        const auto editable_candidate_ids = loki_action::collect_candidate_ids_for_attr(grouped, "editable");
        const auto state_candidate_ids = loki_action::collect_state_candidate_ids(grouped);
        const auto direct_click_candidate_ids = loki_action::collect_direct_click_match_ids(
            grouped,
            prompt_context.task
        );
        const bool has_direct_click_match = !direct_click_candidate_ids.empty();
        const bool has_searchable_editable = loki_action::grouped_has_searchable_editable(grouped);
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
                                  bool allow_click,
                                  bool allow_empty_candidates = false) -> std::optional<loki_action::model_action_response> {
            if (pass_ids.empty() && !allow_empty_candidates) {
                LOKI_LOGI("STEP %d MODEL PASS: %s skipped (no candidates)", prompt_context.step_number, pass_name);
                return std::nullopt;
            }

            const std::string grammar = loki_action::build_action_response_grammar(
                pass_ids,
                pass_editable_ids,
                allow_click
            );

            LOKI_LOGI(
                "STEP %d MODEL PASS: %s ids=%zu editable_ids=%zu allow_click=%s",
                prompt_context.step_number,
                pass_name,
                pass_ids.size(),
                pass_editable_ids.size(),
                allow_click ? "true" : "false"
            );
            LOKI_LOGI(
                "STEP %d MODEL GRAMMAR: \"%s\"",
                prompt_context.step_number,
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
                "STEP %d MODEL REQUEST: pass=%s body=\"%s\"",
                prompt_context.step_number,
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
                "STEP %d MODEL RESPONSE: pass=%s status=%d body=\"%s\"",
                prompt_context.step_number,
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
                "STEP %d MODEL CONTENT: pass=%s \"%s\"",
                prompt_context.step_number,
                pass_name,
                loki_action::truncate_for_log(loki_action::escape_for_log(normalized_content), 400).c_str()
            );
            LOKI_LOGI(
                "STEP %d PARSED ACTION: pass=%s id=%d action=\"%s\" text=\"%s\" done=%s",
                prompt_context.step_number,
                pass_name,
                action_response.selected_id,
                action_response.action_type.c_str(),
                action_response.text
                    ? loki_action::truncate_for_log(loki_action::escape_for_log(*action_response.text), 400).c_str()
                    : "",
                action_response.done ? "true" : "false"
            );
            return action_response;
        };

        loki_action::model_action_response action_response;
        bool has_action_response = false;
        const bool prefers_text_edit = loki_action::prompt_requests_text_edit(prompt_context.task);
        const bool prefers_lookup_input = loki_action::prompt_requests_lookup_input(prompt_context.task);
        const bool prefers_state_action =
            loki_action::prompt_requests_state_change(prompt_context.task) && !state_candidate_ids.empty();
        const bool prefers_editable_input = !editable_candidate_ids.empty() && (
            prefers_text_edit ||
            (has_searchable_editable && prefers_lookup_input) ||
            (has_searchable_editable && prompt_context.repeated_tail_clicks >= 2)
        ) && !has_direct_click_match && !prefers_state_action;
        LOKI_LOGI(
            "STEP %d PROMPT MODE: prefers_text_edit=%s prefers_lookup_input=%s prefers_state_action=%s prefers_editable_input=%s direct_click_match=%s direct_click_ids=%zu state_candidates=%zu searchable_editable=%s editable_candidates=%zu total_candidates=%zu history=%zu",
            prompt_context.step_number,
            prefers_text_edit ? "true" : "false",
            prefers_lookup_input ? "true" : "false",
            prefers_state_action ? "true" : "false",
            prefers_editable_input ? "true" : "false",
            has_direct_click_match ? "true" : "false",
            direct_click_candidate_ids.size(),
            state_candidate_ids.size(),
            has_searchable_editable ? "true" : "false",
            editable_candidate_ids.size(),
            candidate_ids.size(),
            prompt_context.history_tokens.size()
        );

        auto try_accept_model_response = [&](const loki_action::model_action_response & candidate,
                                             const char * pass_name) -> bool {
            if (candidate.done) {
                const auto done_check = loki_action::validate_done_response(grouped, prompt_context.task, prompt_context);
                if (!done_check.accepted) {
                    LOKI_LOGI(
                        "STEP %d DONE REJECTED: pass=%s reason=%s",
                        prompt_context.step_number,
                        pass_name,
                        done_check.reason.c_str()
                    );
                    return false;
                }

                LOKI_LOGI(
                    "STEP %d DONE ACCEPTED: pass=%s reason=%s",
                    prompt_context.step_number,
                    pass_name,
                    done_check.reason.c_str()
                );
            }

            action_response = candidate;
            has_action_response = true;
            return true;
        };

        try {
            if (!has_action_response && !prompt_context.history_tokens.empty()) {
                const auto done_check_response = run_model_pass(
                    "done-check",
                    loki_action::DONE_CHECK_PROMPT,
                    {},
                    {},
                    false,
                    true
                );
                if (done_check_response.has_value() && done_check_response->done) {
                    (void) try_accept_model_response(*done_check_response, "done-check");
                }
            }

            if (!has_action_response && prefers_state_action) {
                const auto state_response = run_model_pass(
                    "state-priority",
                    loki_action::STATE_PRIORITY_PROMPT,
                    state_candidate_ids,
                    {},
                    true
                );
                if (state_response.has_value() &&
                    (state_response->done || state_response->selected_id >= 0)) {
                    (void) try_accept_model_response(*state_response, "state-priority");
                }
            }

            if (!has_action_response && !prefers_text_edit && !prefers_state_action && has_direct_click_match) {
                const auto direct_click_response = run_model_pass(
                    "direct-click-priority",
                    loki_action::DIRECT_CLICK_PRIORITY_PROMPT,
                    direct_click_candidate_ids,
                    {},
                    true
                );
                if (direct_click_response.has_value() &&
                    (direct_click_response->done || direct_click_response->selected_id >= 0)) {
                    (void) try_accept_model_response(*direct_click_response, "direct-click-priority");
                }
            }

            if (prefers_editable_input) {
                const auto editable_response = run_model_pass(
                    "editable-priority",
                    loki_action::EDITABLE_PRIORITY_PROMPT,
                    editable_candidate_ids,
                    editable_candidate_ids,
                    false
                );
                if (editable_response.has_value() && editable_response->selected_id >= 0) {
                    (void) try_accept_model_response(*editable_response, "editable-priority");
                }
            }

            if (!has_action_response) {
                const bool fallback_to_non_editable =
                    prefers_editable_input || (has_direct_click_match && !prefers_text_edit);
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
                    (void) try_accept_model_response(
                        *fallback_response,
                        fallback_to_non_editable ? "non-editable-fallback" : "default"
                    );
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

        if (action_response.done) {
            return make_result_global(
                LOKI_ACTION_STATUS_OK,
                -1,
                "[]",
                "",
                std::nullopt,
                std::nullopt,
                true
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
            action_response.text,
            false
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
    return "1.2.7";
}

} // extern "C"

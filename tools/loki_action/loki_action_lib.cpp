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

constexpr const char* SYSTEM_PROMPT = R"(Android UI planner.
Think briefly:
1) Is the goal already satisfied now?
2) Is it satisfied as the user asked?
3) What single next action remains?

Follow PhaseGoal first.
Prefer current visible tree over search/input.
Use set_text only for visible editable ids.
Use back only for explicit back/exit/wrong-app situations.
Never repeat blocked history signatures.
Return only the compact numeric code allowed by grammar.)";

constexpr const char * EDITABLE_PRIORITY_PROMPT =
    "Editable ids only. "
    "Pick one visible editable target and one text candidate index. "
    "Return only compact numeric code.";

constexpr const char * CLICK_FALLBACK_PROMPT =
    "Direct visible action only. "
    "Prefer target already visible on current screen. "
    "Back only for explicit mismatch/back intent. "
    "Return only compact numeric code.";

constexpr const char * DIRECT_CLICK_PRIORITY_PROMPT =
    "Current screen already shows strong visible target. "
    "Prefer click on that target, not search or input. "
    "Return only compact numeric code.";

constexpr const char * DONE_CHECK_PROMPT =
    "Current app already matches expected app. "
    "Answer only 1 if goal is visibly complete now, else 0.";

constexpr const char * STATE_PRIORITY_PROMPT =
    "State-change task. "
    "Use checked/unchecked evidence first. "
    "Return only compact numeric code.";

constexpr const char * STEP_EXTRACTOR_PROMPT = R"(Break user task into Android UI execution steps. JSON only:
{"goal":"...","apps":["..."],"steps":["..."],"phase_hints":["..."],"done_when":"..."}
- Use CurrentApp, ScreenMode, VisibleStatic and TopInteractive.
- If CurrentApp already matches target app, do not start with "Open <app>".
- If ScreenMode is other_app and task targets another app, first hint should return to launcher/home.
- 2-4 imperative steps (verbs: click, set_text, back)
- 0-4 app names (contacts, phone, dialer, settings, etc.)
- goal: concise, concrete
- phase_hints: short screen-aware next-phase hints
- done_when: screen state that means task is complete)";

#if defined(ANDROID)
constexpr const char * LOG_TAG = "loki_action";
#define LOKI_LOGI(...) __android_log_print(ANDROID_LOG_INFO, loki_action::LOG_TAG, __VA_ARGS__)
#define LOKI_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, loki_action::LOG_TAG, __VA_ARGS__)
#else
#define LOKI_LOGI(...) ((void) 0)
#define LOKI_LOGE(...) ((void) 0)
#endif

struct action_record {
    int step_index = 0;
    char action_code = 'a';
    std::optional<int32_t> id;
    std::string label;
    std::string app;
    std::string signature;
    std::string result = "ok";
};

struct decision_context {
    std::string current_app;
    std::string current_app_short;
    std::string screen_mode;
    std::string goal;
    std::string phase_goal;
    std::string done_when;
    std::vector<std::string> expected_apps;
    std::vector<std::string> target_text_candidates;
    std::vector<int32_t> candidate_ids;
    std::vector<int32_t> editable_ids;
    std::vector<int32_t> direct_click_ids;
    std::vector<std::string> static_lines;
    std::vector<std::string> history_signatures;
    std::vector<action_record> history_records;
    bool app_matches_expected = false;
    bool prefers_text_edit = false;
    bool prefers_lookup_input = false;
    bool prefers_back_navigation = false;
    bool prefers_state_action = false;
    bool prefers_editable_input = false;
    bool has_direct_click_match = false;
    bool allow_back = false;
    bool allow_done = false;
};

enum class screen_mode_t {
    target_app,
    launcher_or_app_drawer,
    other_app,
};

struct phase_gate_result {
    std::string phase_goal;
    std::string phase_reason;
    std::vector<int32_t> direct_click_ids;
    bool allow_click = true;
    bool allow_set_text = false;
    bool allow_back = false;
    bool force_back = false;
};


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

static const std::unordered_set<std::string> GENERIC_RESOURCE_IDS = {
    "root", "content", "main", "container", "layout", "frame",
    "view", "item", "list", "scroll", "text", "image", "icon"
};

static std::string shorten_resource_id(const std::string & raw_id) {
    if (raw_id.empty()) {
        return "";
    }

    const auto colon_pos = raw_id.find(":id/");
    std::string short_id = colon_pos != std::string::npos
        ? raw_id.substr(colon_pos + 4)
        : raw_id;
    short_id = trim_copy(short_id);
    if (short_id.empty() || short_id.size() > 40) {
        return "";
    }

    std::string lowered = short_id;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (GENERIC_RESOURCE_IDS.find(lowered) != GENERIC_RESOURCE_IDS.end()) {
        return "";
    }
    return short_id;
}

json build_text_fields(const json & node) {
    json result = json::object();

    const auto text = normalized_field(node, "text");
    const auto content_desc = normalized_field(node, "contentDesc");
    const auto hint = normalized_field(node, "hintText");

    if (!text && !content_desc && !hint) {
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
    } else if (text) {
        result["text"] = *text;
    }

    if (!text && hint) {
        result["hint"] = *hint;
    }

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
        if (const auto raw_res_id = normalized_field(node, "id")) {
            const auto short_id = shorten_resource_id(*raw_res_id);
            if (!short_id.empty()) {
                item["resId"] = short_id;
            }
        }
        if (const auto input_type = normalized_field(node, "inputType")) {
            item["inputType"] = *input_type;
        }
        if (const auto role = normalized_field(node, "roleDescription")) {
            item["role"] = *role;
        }
        if (const auto state = normalized_field(node, "stateDescription")) {
            item["state"] = *state;
        }
        if (const auto error = normalized_field(node, "error")) {
            item["error"] = *error;
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

std::optional<int32_t> extract_history_id(const std::string & action_line) {
    const std::array<std::string, 2> markers = {"id=", "selected_id="};
    for (const auto & marker : markers) {
        const auto marker_pos = action_line.find(marker);
        if (marker_pos == std::string::npos) {
            continue;
        }

        size_t value_pos = marker_pos + marker.size();
        while (value_pos < action_line.size() &&
               std::isspace(static_cast<unsigned char>(action_line[value_pos])) != 0) {
            ++value_pos;
        }
        if (value_pos >= action_line.size()) {
            continue;
        }

        const bool negative = action_line[value_pos] == '-';
        if (negative) {
            ++value_pos;
        }

        size_t end_pos = value_pos;
        while (end_pos < action_line.size() &&
               std::isdigit(static_cast<unsigned char>(action_line[end_pos])) != 0) {
            ++end_pos;
        }

        if (end_pos == value_pos) {
            continue;
        }

        const std::string number = action_line.substr(
            negative ? value_pos - 1 : value_pos,
            end_pos - (negative ? value_pos - 1 : value_pos)
        );
        char * end_ptr = nullptr;
        const long parsed = std::strtol(number.c_str(), &end_ptr, 10);
        if (end_ptr != nullptr && end_ptr != number.c_str() && *end_ptr == '\0') {
            return static_cast<int32_t>(parsed);
        }
    }
    return std::nullopt;
}

std::optional<std::string> extract_history_marker_value(const std::string & action_line, const std::string & marker) {
    const auto marker_pos = action_line.find(marker);
    if (marker_pos == std::string::npos) {
        return std::nullopt;
    }

    size_t value_pos = marker_pos + marker.size();
    while (value_pos < action_line.size() &&
           std::isspace(static_cast<unsigned char>(action_line[value_pos])) != 0) {
        ++value_pos;
    }
    if (value_pos >= action_line.size()) {
        return std::nullopt;
    }

    std::string result;
    if (action_line[value_pos] == '\'' || action_line[value_pos] == '"') {
        const char quote = action_line[value_pos];
        ++value_pos;
        size_t end_pos = value_pos;
        while (end_pos < action_line.size() && action_line[end_pos] != quote) {
            ++end_pos;
        }
        result = action_line.substr(value_pos, end_pos - value_pos);
    } else {
        size_t end_pos = value_pos;
        while (end_pos < action_line.size() &&
               std::isspace(static_cast<unsigned char>(action_line[end_pos])) == 0) {
            ++end_pos;
        }
        result = action_line.substr(value_pos, end_pos - value_pos);
    }

    result = trim_copy(result);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

std::string shorten_app_name(const std::string & app) {
    const auto trimmed = trim_copy(app);
    if (trimmed.empty()) {
        return "";
    }
    const auto dot_pos = trimmed.find_last_of('.');
    if (dot_pos != std::string::npos && dot_pos + 1 < trimmed.size()) {
        return trimmed.substr(dot_pos + 1);
    }
    return trimmed;
}

std::string compact_history_label(const std::string & action_line, char action_code) {
    std::string label = extract_history_marker_value(action_line, "label=").value_or("");
    if (label.empty()) {
        const auto colon = action_line.find(':');
        if (colon != std::string::npos && colon + 1 < action_line.size()) {
            label = trim_copy(action_line.substr(colon + 1));
        } else {
            label = trim_copy(action_line);
        }
    }

    if (label.empty()) {
        return "";
    }

    if (label.size() > 36) {
        label = label.substr(0, 36);
    }
    return std::string(1, action_code) + ":" + label;
}

std::string build_history_signature(
    char action_code,
    const std::optional<int32_t> & id,
    const std::optional<std::string> & label,
    const std::optional<std::string> & app
) {
    std::string out(1, action_code);
    out += "#";
    out += id.has_value() ? std::to_string(*id) : "-1";
    if (app.has_value() && !trim_copy(*app).empty()) {
        out += "@";
        out += shorten_app_name(*app);
    }
    if (label.has_value() && !trim_copy(*label).empty()) {
        out += ":";
        auto compact = trim_copy(*label);
        if (compact.size() > 24) {
            compact = compact.substr(0, 24);
        }
        out += compact;
    }
    return out;
}

char detect_history_action_code(const std::string & action_line) {
    if (action_line.find("set_text") != std::string::npos) {
        return 't';
    }
    if (action_line.find("click") != std::string::npos) {
        return 'c';
    }
    if (action_line.find("back") != std::string::npos || action_line.find(u8"назад") != std::string::npos) {
        return 'b';
    }
    return 'a';
}

void append_history_entry(prompt_context & context, const std::string & raw_line) {
    const auto action_line = trim_action_prefix(trim_copy(raw_line));
    if (action_line.empty()) {
        return;
    }

    const char action_code = detect_history_action_code(action_line);
    const auto parsed_id = extract_history_id(action_line);
    const auto label = extract_history_marker_value(action_line, "label=");
    const auto app = extract_history_marker_value(action_line, "app=");
    context.history_tokens.emplace_back(1, action_code);
    if (parsed_id.has_value()) {
        context.history_ids.push_back(*parsed_id);
    }
    const auto compact_label = compact_history_label(action_line, action_code);
    if (!compact_label.empty()) {
        context.history_labels.push_back(compact_label);
    }
    if (app.has_value()) {
        context.history_apps.push_back(shorten_app_name(*app));
    }
    context.history_entries.push_back(action_line);
    context.history_signatures.push_back(build_history_signature(action_code, parsed_id, label, app));
}

std::vector<std::string> split_inline_history_entries(const std::string & history_blob) {
    std::vector<std::string> entries;
    size_t cursor = 0;
    while (cursor < history_blob.size()) {
        while (cursor < history_blob.size() &&
               std::isspace(static_cast<unsigned char>(history_blob[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= history_blob.size()) {
            break;
        }

        if (std::isdigit(static_cast<unsigned char>(history_blob[cursor])) == 0) {
            break;
        }
        while (cursor < history_blob.size() &&
               std::isdigit(static_cast<unsigned char>(history_blob[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= history_blob.size() || history_blob[cursor] != '.') {
            break;
        }
        ++cursor;
        while (cursor < history_blob.size() &&
               std::isspace(static_cast<unsigned char>(history_blob[cursor])) != 0) {
            ++cursor;
        }
        const size_t entry_begin = cursor;
        size_t entry_end = history_blob.size();
        for (size_t i = cursor; i + 2 < history_blob.size(); ++i) {
            if (history_blob[i] == '\n') {
                entry_end = i;
                break;
            }
            if (std::isdigit(static_cast<unsigned char>(history_blob[i])) != 0) {
                size_t digits_end = i;
                while (digits_end < history_blob.size() &&
                       std::isdigit(static_cast<unsigned char>(history_blob[digits_end])) != 0) {
                    ++digits_end;
                }
                if (digits_end < history_blob.size() &&
                    history_blob[digits_end] == '.' &&
                    digits_end + 1 < history_blob.size() &&
                    std::isspace(static_cast<unsigned char>(history_blob[digits_end + 1])) != 0) {
                    entry_end = i;
                    break;
                }
            }
        }
        const auto entry = trim_copy(history_blob.substr(entry_begin, entry_end - entry_begin));
        if (!entry.empty()) {
            entries.push_back(entry);
        }
        cursor = entry_end;
    }
    return entries;
}

std::string compact_single_line(const std::string & raw, size_t max_len = 80) {
    std::string compact;
    compact.reserve(raw.size());
    bool prev_space = false;
    for (const char ch : raw) {
        const bool is_space = std::isspace(static_cast<unsigned char>(ch)) != 0;
        if (is_space) {
            if (!prev_space) {
                compact.push_back(' ');
                prev_space = true;
            }
            continue;
        }
        compact.push_back(ch);
        prev_space = false;
    }
    compact = trim_copy(compact);
    if (compact.size() > max_len) {
        compact.resize(max_len);
    }
    return compact;
}

std::string join_compact(const std::vector<std::string> & items, const char * separator, size_t max_items = 0) {
    std::string out;
    size_t emitted = 0;
    for (const auto & item : items) {
        const auto compact = compact_single_line(item);
        if (compact.empty()) {
            continue;
        }
        if (max_items > 0 && emitted >= max_items) {
            break;
        }
        if (!out.empty()) {
            out += separator;
        }
        out += compact;
        emitted += 1;
    }
    return out;
}

std::string build_user_content_from_context(
    const prompt_context & context,
    const std::string & screen_name,
    const std::string & toon,
    const std::optional<extracted_steps_plan> & extracted_plan,
    const std::string & screen_mode = "",
    const std::string & phase_goal = ""
) {
    std::string user_content;
    user_content.reserve(context.task.size() + screen_name.size() + toon.size() + 192);
    user_content += "Task: ";
    user_content += context.task;
    user_content += "\nApp: ";
    user_content += screen_name;
    user_content += "\nAppShort: ";
    user_content += shorten_app_name(screen_name);
    if (!screen_mode.empty()) {
        user_content += "\nScreenMode: ";
        user_content += screen_mode;
    }
    if (extracted_plan.has_value()) {
        if (!trim_copy(extracted_plan->goal).empty()) {
            user_content += "\nPlanGoal: ";
            user_content += compact_single_line(extracted_plan->goal, 96);
        }
        if (!phase_goal.empty()) {
            user_content += "\nPhaseGoal: ";
            user_content += compact_single_line(phase_goal, 96);
        }
        const auto apps_line = join_compact(extracted_plan->apps, ",", 3);
        if (!apps_line.empty()) {
            user_content += "\nExpectedApps: ";
            user_content += apps_line;
        }
        if (!extracted_plan->steps.empty()) {
            user_content += "\nPlanSteps:";
            for (size_t i = 0; i < extracted_plan->steps.size() && i < 4; ++i) {
                user_content += "\n";
                user_content += std::to_string(i + 1);
                user_content += ") ";
                user_content += compact_single_line(extracted_plan->steps[i], 96);
            }
        }
        if (!extracted_plan->done_when.empty()) {
            user_content += "\nDoneWhen: ";
            user_content += compact_single_line(extracted_plan->done_when, 96);
        }
        if (!extracted_plan->target_text_candidates.empty()) {
            user_content += "\nTextChoices:";
            for (size_t i = 0; i < extracted_plan->target_text_candidates.size(); ++i) {
                user_content += "\n";
                user_content += std::to_string(i);
                user_content += ") ";
                user_content += compact_single_line(extracted_plan->target_text_candidates[i], 64);
            }
        }
    }
    if (!context.history_entries.empty()) {
        user_content += "\nHistory:";
        for (size_t i = 0; i < context.history_entries.size(); ++i) {
            user_content += "\n";
            user_content += "S";
            user_content += std::to_string(i + 1);
            user_content += " ";
            user_content += context.history_entries[i];
        }
    }
    if (!context.history_signatures.empty()) {
        user_content += "\nHistSig:";
        for (size_t i = 0; i < context.history_signatures.size(); ++i) {
            if (i != 0) {
                user_content += ">";
            }
            user_content += context.history_signatures[i];
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
    if (context.repeated_tail_same_id >= 2 && !context.history_ids.empty()) {
        user_content += "\nRepeatId:";
        user_content += std::to_string(context.history_ids.back());
        user_content += "x";
        user_content += std::to_string(context.repeated_tail_same_id);
    }
    if (context.repeated_tail_same_signature >= 2 && !context.history_signatures.empty()) {
        user_content += "\nRepeatSig:";
        user_content += context.history_signatures.back();
        user_content += "x";
        user_content += std::to_string(context.repeated_tail_same_signature);
    }
    user_content += "\nUI:\n";
    user_content += toon;
    return user_content;
}

std::string build_user_content(
    const std::string & user_prompt,
    const std::string & screen_name,
    const std::string & toon
) {
    const auto context = parse_prompt_context(user_prompt);
    return build_user_content_from_context(context, screen_name, toon, std::nullopt, "", "");
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

std::string build_steps_extractor_grammar() {
    std::string grammar;
    grammar += "root ::= ws \"{\" ws \"\\\"goal\\\"\" ws \":\" ws json-string ws \",\" ws \"\\\"apps\\\"\" ws \":\" ws apps-array ws \",\" ws \"\\\"steps\\\"\" ws \":\" ws steps-array ws \",\" ws \"\\\"phase_hints\\\"\" ws \":\" ws phase-array ws \",\" ws \"\\\"done_when\\\"\" ws \":\" ws json-string ws \"}\" ws\n";
    grammar += "apps-array ::= \"[\" ws \"]\" | \"[\" ws json-string ws \"]\" | \"[\" ws json-string ws \",\" ws json-string ws \"]\" | \"[\" ws json-string ws \",\" ws json-string ws \",\" ws json-string ws \"]\"\n";
    grammar += "steps-array ::= \"[\" ws json-string ws \"]\" | \"[\" ws json-string ws \",\" ws json-string ws \"]\" | \"[\" ws json-string ws \",\" ws json-string ws \",\" ws json-string ws \"]\" | \"[\" ws json-string ws \",\" ws json-string ws \",\" ws json-string ws \",\" ws json-string ws \"]\"\n";
    grammar += "phase-array ::= \"[\" ws json-string ws \"]\" | \"[\" ws json-string ws \",\" ws json-string ws \"]\" | \"[\" ws json-string ws \",\" ws json-string ws \",\" ws json-string ws \"]\" | \"[\" ws json-string ws \",\" ws json-string ws \",\" ws json-string ws \",\" ws json-string ws \"]\"\n";
    grammar += "json-string ::= \"\\\"\" json-char* \"\\\"\"\n";
    grammar += "json-char ::= [^\"\\\\\\x0A\\x0D] | escape\n";
    grammar += "escape ::= \"\\\\\" ([\"\\\\/bfnrt] | (\"u\" hex hex hex hex))\n";
    grammar += "hex ::= [0-9a-fA-F]\n";
    grammar += "ws ::= | \" \" ws | \"\\n\" ws | \"\\r\" ws | \"\\t\" ws\n";
    return grammar;
}

std::string build_steps_extractor_user_content(
    const prompt_context & context,
    const std::string & screen_name,
    const std::string & current_app_short,
    const std::string & screen_mode,
    const std::vector<std::string> & static_lines,
    const std::vector<std::string> & top_interactive
) {
    std::string out;
    out.reserve(context.task.size() + screen_name.size() + 256);
    out += "Task: ";
    out += context.task;
    out += "\nCurrentApp: ";
    out += screen_name;
    out += "\nCurrentAppShort: ";
    out += current_app_short;
    out += "\nScreenMode: ";
    out += screen_mode;
    out += "\nVisibleStatic:";
    if (static_lines.empty()) {
        out += " none";
    } else {
        for (const auto & line : static_lines) {
            out += "\n- ";
            out += compact_single_line(line, 72);
        }
    }
    out += "\nTopInteractive:";
    if (top_interactive.empty()) {
        out += " none";
    } else {
        for (const auto & line : top_interactive) {
            out += "\n- ";
            out += compact_single_line(line, 72);
        }
    }
    out += "\nHistoryRecent:";
    if (context.history_entries.empty()) {
        out += " none";
    } else {
        const size_t begin = context.history_entries.size() > 2 ? context.history_entries.size() - 2 : 0;
        for (size_t i = begin; i < context.history_entries.size(); ++i) {
            out += "\n- ";
            out += compact_single_line(context.history_entries[i], 88);
        }
    }
    return out;
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

std::vector<std::string> parse_compact_string_array(
    const json & value,
    size_t max_items,
    size_t max_item_len
) {
    std::vector<std::string> out;
    if (!value.is_array()) {
        return out;
    }

    for (const auto & item : value) {
        if (!item.is_string()) {
            continue;
        }
        const auto compact = compact_single_line(item.get<std::string>(), max_item_len);
        if (compact.empty()) {
            continue;
        }
        out.push_back(compact);
        if (out.size() >= max_items) {
            break;
        }
    }
    return out;
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

static int32_t parse_protocol_int(const std::string & value, const char * error_message) {
    const auto trimmed = trim_copy(value);
    if (trimmed.empty()) {
        throw std::runtime_error(error_message);
    }
    char * end_ptr = nullptr;
    const long parsed = std::strtol(trimmed.c_str(), &end_ptr, 10);
    if (end_ptr == nullptr || end_ptr == trimmed.c_str() || *end_ptr != '\0') {
        throw std::runtime_error(error_message);
    }
    return static_cast<int32_t>(parsed);
}

static std::vector<std::string> split_protocol_fields(const std::string & value) {
    std::vector<std::string> out;
    size_t cursor = 0;
    while (cursor <= value.size()) {
        const auto next = value.find(':', cursor);
        if (next == std::string::npos) {
            out.push_back(trim_copy(value.substr(cursor)));
            break;
        }
        out.push_back(trim_copy(value.substr(cursor, next - cursor)));
        cursor = next + 1;
    }
    return out;
}

static model_action_response parse_numeric_action_response(const std::string & raw_text) {
    const auto fields = split_protocol_fields(raw_text);
    if (fields.empty()) {
        throw std::runtime_error("empty compact response");
    }

    const int32_t opcode = parse_protocol_int(fields[0], "invalid compact response opcode");
    model_action_response result;
    switch (opcode) {
        case 0:
            if (fields.size() != 1) {
                throw std::runtime_error("no-match compact response must be 0");
            }
            result.selected_id = -1;
            return result;
        case 1:
            if (fields.size() != 2) {
                throw std::runtime_error("click compact response must be 1:<id>");
            }
            result.action_type = "click";
            result.selected_id = parse_protocol_int(fields[1], "click compact response id is invalid");
            return result;
        case 2:
            if (fields.size() != 3) {
                throw std::runtime_error("set_text compact response must be 2:<id>:<text_index>");
            }
            result.action_type = "set_text";
            result.selected_id = parse_protocol_int(fields[1], "set_text compact response id is invalid");
            result.text_index = parse_protocol_int(fields[2], "set_text compact response text_index is invalid");
            return result;
        case 5:
            if (fields.size() != 1) {
                throw std::runtime_error("back compact response must be 5");
            }
            result.action_type = "back";
            result.selected_id = -1;
            return result;
        case 6:
            if (fields.size() != 1) {
                throw std::runtime_error("done compact response must be 6");
            }
            result.done = true;
            result.selected_id = -1;
            return result;
        default:
            throw std::runtime_error("unknown compact response opcode");
    }
}

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
    try {
        return parse_numeric_action_response(trimmed);
    } catch (const std::exception &) {
    }

    model_action_response fallback;
    if (to_lower_ascii(trimmed) == "back") {
        fallback.action_type = "back";
        fallback.selected_id = -1;
        return fallback;
    }
    fallback.selected_id = parse_action_id_text(trimmed);
    if (fallback.selected_id >= 0) {
        fallback.action_type = "click";
    }
    return fallback;
}

model_action_response parse_action_response_json(const json & parsed) {
    if (parsed.is_number_integer()) {
        return parse_numeric_action_response(std::to_string(parsed.get<int32_t>()));
    }
    if (parsed.is_string()) {
        return parse_action_response_text(parsed.get<std::string>());
    }
    if (!parsed.is_object()) {
        throw std::runtime_error("model response must be a JSON object");
    }

    auto parse_action_name = [&parsed]() -> std::optional<std::string> {
        const auto action_type_it = parsed.find("action_type");
        if (action_type_it != parsed.end() && action_type_it->is_string()) {
            return action_type_it->get<std::string>();
        }
        const auto action_it = parsed.find("action");
        if (action_it != parsed.end() && action_it->is_string()) {
            return action_it->get<std::string>();
        }
        return std::nullopt;
    };

    const auto action_name = parse_action_name();
    if (action_name.has_value()) {
        model_action_response result;
        const auto lowered_action = to_lower_ascii(trim_copy(*action_name));
        if (lowered_action == "back") {
            const auto done_it = parsed.find("done");
            if (done_it != parsed.end() && parse_done_field(*done_it)) {
                throw std::runtime_error("model response back action must have done=false");
            }

            const auto id_it = parsed.find("id");
            if (id_it != parsed.end()) {
                const int32_t id = parse_action_id_field(*id_it);
                if (id >= 0) {
                    throw std::runtime_error("model response back action must not include positive id");
                }
            }

            const auto text_it = parsed.find("text");
            if (text_it != parsed.end()) {
                if (!text_it->is_string()) {
                    throw std::runtime_error("model response back action text must be a string");
                }
                if (!trim_copy(text_it->get<std::string>()).empty()) {
                    throw std::runtime_error("model response back action must not include text");
                }
            }

            result.selected_id = -1;
            result.action_type = "back";
            result.done = false;
            return result;
        }

        const auto id_it = parsed.find("id");
        if (id_it == parsed.end()) {
            throw std::runtime_error("model response is missing id");
        }

        result.selected_id = parse_action_id_field(*id_it);
        result.done = false;

        const auto done_it = parsed.find("done");
        if (done_it != parsed.end() && parse_done_field(*done_it)) {
            throw std::runtime_error("model response action must have done=false");
        }

        if (result.selected_id < 0) {
            return result;
        }

        if (lowered_action == "click") {
            result.action_type = "click";
            return result;
        }
        if (lowered_action != "set_text") {
            throw std::runtime_error("model response action must be click, set_text, or back");
        }

        const auto text_index_it = parsed.find("text_index");
        if (text_index_it != parsed.end()) {
            result.action_type = "set_text";
            result.text_index = parse_action_id_field(*text_index_it);
            return result;
        }
        const auto text_it = parsed.find("text");
        if (text_it == parsed.end() || !text_it->is_string()) {
            throw std::runtime_error("model response set_text action requires text or text_index");
        }

        const std::string raw_insert_text = text_it->get<std::string>();
        if (trim_copy(raw_insert_text).empty()) {
            throw std::runtime_error("model response text must not be empty");
        }
        result.action_type = "set_text";
        result.text = raw_insert_text;
        return result;
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

    result.action_type = to_lower_ascii(trim_copy(action_it->get<std::string>()));
    if (result.action_type == "click") {
        return result;
    }
    if (result.action_type != "set_text") {
        throw std::runtime_error("model response action must be click, set_text, or back");
    }

    const auto text_index_it = parsed.find("text_index");
    if (text_index_it != parsed.end()) {
        result.text_index = parse_action_id_field(*text_index_it);
        return result;
    }
    const auto text_it = parsed.find("text");
    if (text_it == parsed.end() || !text_it->is_string()) {
        throw std::runtime_error("model response set_text action requires text or text_index");
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

        if (starts_with_literal(trimmed, u8"Команда пользователя:") ||
            starts_with_literal(trimmed, "User command:") ||
            starts_with_literal(trimmed, "Task:")) {
            std::string remainder;
            if (starts_with_literal(trimmed, u8"Команда пользователя:")) {
                remainder = trim_copy(trimmed.substr(std::strlen(u8"Команда пользователя:")));
            } else if (starts_with_literal(trimmed, "User command:")) {
                remainder = trim_copy(trimmed.substr(std::strlen("User command:")));
            } else {
                remainder = trim_copy(trimmed.substr(std::strlen("Task:")));
            }

            const std::array<std::string, 4> history_markers = {
                std::string("History (old->new):"),
                std::string("History:"),
                std::string("Completed actions:"),
                std::string(u8"Уже выполненные действия:")
            };
            bool found_inline_history = false;
            for (const auto & marker : history_markers) {
                const auto pos = remainder.find(marker);
                if (pos == std::string::npos) {
                    continue;
                }
                context.task = trim_copy(remainder.substr(0, pos));
                for (const auto & entry : split_inline_history_entries(
                    remainder.substr(pos + marker.size()))) {
                    append_history_entry(context, entry);
                }
                found_inline_history = true;
                in_history = true;
                break;
            }
            if (!found_inline_history) {
                context.task = remainder;
            }
        } else if (trimmed == u8"Уже выполненные действия:" ||
                   trimmed == "Completed actions:" ||
                   trimmed == "History:" ||
                   starts_with_literal(trimmed, "History (old->new):")) {
            in_history = true;
            const auto marker_pos = trimmed.find(':');
            if (marker_pos != std::string::npos && marker_pos + 1 < trimmed.size()) {
                for (const auto & entry : split_inline_history_entries(trimmed.substr(marker_pos + 1))) {
                    append_history_entry(context, entry);
                }
            }
        } else if (in_history) {
            if (starts_with_literal(trimmed, u8"Определи следующее действие") ||
                starts_with_literal(trimmed, "Determine the next action") ||
                starts_with_literal(trimmed, "Next action")) {
                in_history = false;
            } else if (!trimmed.empty() && std::isdigit(static_cast<unsigned char>(trimmed[0])) != 0) {
                append_history_entry(context, trimmed);
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
    if (!context.history_ids.empty()) {
        const int32_t last_id = context.history_ids.back();
        for (auto it = context.history_ids.rbegin(); it != context.history_ids.rend(); ++it) {
            if (*it != last_id) {
                break;
            }
            context.repeated_tail_same_id += 1;
        }
    }
    if (!context.history_signatures.empty()) {
        const auto & last_signature = context.history_signatures.back();
        for (auto it = context.history_signatures.rbegin(); it != context.history_signatures.rend(); ++it) {
            if (*it != last_signature) {
                break;
            }
            context.repeated_tail_same_signature += 1;
        }
    }

    return context;
}

std::optional<extracted_steps_plan> parse_steps_extractor_content(const std::string & content) {
    const auto trimmed = trim_copy(content);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    try {
        const auto parsed = json::parse(trimmed);
        if (!parsed.is_object()) {
            return std::nullopt;
        }

        const auto goal_it = parsed.find("goal");
        const auto steps_it = parsed.find("steps");
        if (goal_it == parsed.end() || !goal_it->is_string() || steps_it == parsed.end()) {
            return std::nullopt;
        }

        extracted_steps_plan plan;
        plan.goal = compact_single_line(goal_it->get<std::string>(), 96);
        plan.steps = parse_compact_string_array(*steps_it, 4, 96);
        if (plan.goal.empty() || plan.steps.empty()) {
            return std::nullopt;
        }

        const auto apps_it = parsed.find("apps");
        if (apps_it != parsed.end()) {
            plan.apps = parse_compact_string_array(*apps_it, 3, 32);
        }
        const auto phase_hints_it = parsed.find("phase_hints");
        if (phase_hints_it != parsed.end()) {
            plan.phase_hints = parse_compact_string_array(*phase_hints_it, 4, 96);
        }
        const auto done_when_it = parsed.find("done_when");
        if (done_when_it != parsed.end() && done_when_it->is_string()) {
            plan.done_when = compact_single_line(done_when_it->get<std::string>(), 96);
        }
        const auto target_text_it = parsed.find("target_text_candidates");
        if (target_text_it != parsed.end()) {
            plan.target_text_candidates = parse_compact_string_array(*target_text_it, 6, 64);
        }
        return plan;
    } catch (const json::exception &) {
        return std::nullopt;
    }
}

static std::string normalize_compact_label(const std::string & value, size_t max_len = 64) {
    auto out = compact_single_line(value, max_len);
    std::replace(out.begin(), out.end(), '|', ' ');
    return trim_copy(out);
}

static std::vector<std::string> extract_quoted_segments(const std::string & input) {
    std::vector<std::string> result;
    const std::array<std::pair<std::string, std::string>, 4> quotes = {{
        {"\"", "\""},
        {"'", "'"},
        {u8"«", u8"»"},
        {u8"“", u8"”"},
    }};

    for (const auto & [open, close] : quotes) {
        size_t cursor = 0;
        while (cursor < input.size()) {
            const auto start = input.find(open, cursor);
            if (start == std::string::npos) {
                break;
            }
            const auto value_start = start + open.size();
            const auto end = input.find(close, value_start);
            if (end == std::string::npos) {
                break;
            }
            const auto candidate = normalize_compact_label(input.substr(value_start, end - value_start), 64);
            if (!candidate.empty()) {
                result.push_back(candidate);
            }
            cursor = end + close.size();
        }
    }
    return result;
}

static std::string strip_common_target_prefixes(std::string value) {
    static const std::vector<std::string> prefixes = {
        "find ", "search ", "search for ", "open ", "call ", "message ", "text ",
        u8"найди ", u8"найти ", u8"поиск ", u8"открой ", u8"позвони ", u8"напиши ",
        u8"создай контакт ", u8"создай новый контакт ", u8"контакт ", u8"контакт с ",
        u8"для ", u8"с "
    };
    const auto lowered = to_lower_basic_multilang(value);
    for (const auto & prefix : prefixes) {
        if (starts_with_literal(lowered, prefix) && value.size() > prefix.size()) {
            value = trim_copy(value.substr(prefix.size()));
            break;
        }
    }
    return trim_copy(value);
}

static std::vector<std::string> derive_text_candidates(
    const std::string & task,
    const std::optional<extracted_steps_plan> & extracted_plan
) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;

    const auto push_candidate = [&](const std::string & raw_value) {
        auto value = normalize_compact_label(strip_common_target_prefixes(raw_value), 64);
        if (value.empty()) {
            return;
        }
        const auto lowered = to_lower_basic_multilang(value);
        static const std::vector<std::string> bad_values = {
            "search", "search contacts", "search web and more", "find", "lookup",
            u8"поиск", u8"найти", u8"поиск контактов"
        };
        for (const auto & bad : bad_values) {
            if (lowered == bad) {
                return;
            }
        }
        if (seen.insert(lowered).second) {
            result.push_back(value);
        }
    };

    for (const auto & quoted : extract_quoted_segments(task)) {
        push_candidate(quoted);
    }

    const std::array<std::string, 10> markers = {
        "find ", "search ", "search for ", "call ", "message ",
        u8"найди ", u8"найти ", u8"позвони ", u8"напиши ", u8"создай контакт "
    };
    const auto lowered_task = to_lower_basic_multilang(task);
    for (const auto & marker : markers) {
        const auto pos = lowered_task.find(marker);
        if (pos == std::string::npos) {
            continue;
        }
        push_candidate(task.substr(pos + marker.size()));
    }

    for (const auto & term : extract_task_match_terms(task)) {
        push_candidate(term);
    }

    if (extracted_plan.has_value()) {
        if (!extracted_plan->target_text_candidates.empty()) {
            for (const auto & candidate : extracted_plan->target_text_candidates) {
                push_candidate(candidate);
            }
        }
        push_candidate(extracted_plan->goal);
        for (const auto & step : extracted_plan->steps) {
            push_candidate(step);
        }
    }

    if (result.size() > 6) {
        result.resize(6);
    }
    return result;
}

static std::string build_item_label_for_prompt(const json & item);
static std::vector<const json *> collect_unique_item_refs(const json & grouped);
static std::vector<int32_t> collect_direct_click_match_ids(const json & grouped, const std::string & task);

static std::vector<std::string> expand_expected_apps(const std::vector<std::string> & apps) {
    static const std::unordered_map<std::string, std::vector<std::string>> aliases = {
        {"contacts", {"contacts", "contact", "people", "phone", "dialer"}},
        {"contact", {"contacts", "contact", "people", "phone", "dialer"}},
        {"phone", {"phone", "dialer", "contacts"}},
        {"dialer", {"dialer", "phone", "contacts"}},
        {"settings", {"settings", "system settings"}},
        {"alarm", {"alarm", "clock"}},
        {"clock", {"clock", "alarm"}},
        {"youtube", {"youtube"}},
        {"messages", {"messages", "messaging"}},
        {"chrome", {"chrome", "browser"}}
    };

    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    const auto push_value = [&](const std::string & raw_value) {
        const auto lowered = to_lower_basic_multilang(normalize_compact_label(shorten_app_name(raw_value), 32));
        if (lowered.empty()) {
            return;
        }
        if (seen.insert(lowered).second) {
            out.push_back(lowered);
        }
    };

    for (const auto & app : apps) {
        push_value(app);
        const auto lowered = to_lower_basic_multilang(app);
        const auto it = aliases.find(lowered);
        if (it == aliases.end()) {
            continue;
        }
        for (const auto & alias : it->second) {
            push_value(alias);
        }
    }
    return out;
}

static bool app_matches_expected(const std::string & current_app, const std::vector<std::string> & expected_apps);

static bool looks_like_launcher_name(const std::string & app_short) {
    const auto lowered = to_lower_basic_multilang(app_short);
    return lowered.find("launcher") != std::string::npos ||
        lowered.find("launcher3") != std::string::npos ||
        lowered.find("nexuslauncher") != std::string::npos ||
        lowered.find("quickstep") != std::string::npos ||
        lowered.find("pixel") != std::string::npos;
}

static std::vector<std::string> build_top_interactive_summary(const json & grouped, size_t max_items = 8) {
    std::vector<std::string> out;
    const auto item_refs = collect_unique_item_refs(grouped);
    for (const auto * item_ptr : item_refs) {
        const auto label = build_item_label_for_prompt(*item_ptr);
        if (label.empty()) {
            continue;
        }
        out.push_back(label);
        if (out.size() >= max_items) {
            break;
        }
    }
    return out;
}

static bool looks_like_launcher_grid(const std::vector<std::string> & top_interactive) {
    size_t short_labels = 0;
    for (const auto & label : top_interactive) {
        if (!label.empty() && label.size() <= 24) {
            short_labels += 1;
        }
    }
    return top_interactive.size() >= 5 && short_labels >= 4;
}

static screen_mode_t detect_screen_mode(
    const std::string & screen_name,
    const std::vector<std::string> & expected_apps,
    const std::vector<std::string> & static_lines,
    const std::vector<std::string> & top_interactive
) {
    if (app_matches_expected(screen_name, expected_apps)) {
        return screen_mode_t::target_app;
    }

    const auto app_short = shorten_app_name(screen_name);
    if (looks_like_launcher_name(app_short)) {
        return screen_mode_t::launcher_or_app_drawer;
    }

    std::string static_blob = to_lower_basic_multilang(join_compact(static_lines, " ", 8));
    if (static_blob.find("google search") != std::string::npos ||
        static_blob.find("play store") != std::string::npos ||
        static_blob.find(u8"поиск google") != std::string::npos) {
        return screen_mode_t::launcher_or_app_drawer;
    }
    if (looks_like_launcher_grid(top_interactive)) {
        return screen_mode_t::launcher_or_app_drawer;
    }

    return screen_mode_t::other_app;
}

static const char * screen_mode_name(screen_mode_t mode) {
    switch (mode) {
        case screen_mode_t::target_app:
            return "target_app";
        case screen_mode_t::launcher_or_app_drawer:
            return "launcher_or_app_drawer";
        case screen_mode_t::other_app:
        default:
            return "other_app";
    }
}

static bool step_mentions_any(const std::string & step, const std::vector<std::string> & keywords) {
    const auto lowered = to_lower_basic_multilang(step);
    for (const auto & keyword : keywords) {
        if (!keyword.empty() && lowered.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool step_mentions_input_like(const std::string & step) {
    return step_mentions_any(step, {
        "set text", "type", "enter", "input", "first name", "name", "search",
        u8"введ", u8"впиш", u8"напиш", u8"имя", u8"поиск"
    });
}

static bool step_mentions_open_like(const std::string & step) {
    return step_mentions_any(step, {
        "open ", "launch ", "open app", u8"открой", u8"открыть", u8"запусти"
    });
}

static bool step_mentions_back_home_like(const std::string & step) {
    return step_mentions_any(step, {
        "back to home", "return to home", "launcher", "app drawer",
        u8"вернись домой", u8"на главный экран", u8"вернись на главный"
    });
}

static bool step_is_open_target_app_step(const std::string & step, const std::vector<std::string> & expected_apps) {
    if (!step_mentions_open_like(step) || expected_apps.empty()) {
        return false;
    }
    const auto lowered = to_lower_basic_multilang(step);
    for (const auto & expected : expected_apps) {
        if (!expected.empty() && lowered.find(expected) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static std::vector<std::string> infer_apps_from_task(const std::string & task) {
    const auto lowered = to_lower_basic_multilang(task);
    if (lowered.find("contact") != std::string::npos ||
        lowered.find("dial") != std::string::npos ||
        lowered.find("call") != std::string::npos ||
        lowered.find(u8"контакт") != std::string::npos ||
        lowered.find(u8"позвон") != std::string::npos) {
        return {"contacts", "phone", "dialer"};
    }
    if (lowered.find("battery") != std::string::npos ||
        lowered.find("wifi") != std::string::npos ||
        lowered.find("bluetooth") != std::string::npos ||
        lowered.find("settings") != std::string::npos ||
        lowered.find(u8"настрой") != std::string::npos ||
        lowered.find(u8"батар") != std::string::npos) {
        return {"settings"};
    }
    if (lowered.find("alarm") != std::string::npos ||
        lowered.find("clock") != std::string::npos ||
        lowered.find(u8"будиль") != std::string::npos ||
        lowered.find(u8"часы") != std::string::npos) {
        return {"clock", "alarm"};
    }
    if (lowered.find("video") != std::string::npos ||
        lowered.find("youtube") != std::string::npos ||
        lowered.find(u8"видео") != std::string::npos ||
        lowered.find(u8"ютуб") != std::string::npos) {
        return {"youtube"};
    }
    if (lowered.find("message") != std::string::npos ||
        lowered.find(u8"сообщ") != std::string::npos ||
        lowered.find(u8"смс") != std::string::npos) {
        return {"messages"};
    }
    return {};
}

static extracted_steps_plan enrich_extracted_plan(
    const prompt_context & context,
    const std::optional<extracted_steps_plan> & parsed_plan,
    bool app_match,
    bool has_direct_click_match,
    bool prefers_editable_input
) {
    extracted_steps_plan enriched = parsed_plan.value_or(extracted_steps_plan{});
    if (trim_copy(enriched.goal).empty()) {
        enriched.goal = compact_single_line(context.task, 96);
    }

    for (const auto & app : infer_apps_from_task(context.task)) {
        enriched.apps.push_back(app);
    }
    enriched.apps = expand_expected_apps(enriched.apps);

    if (enriched.target_text_candidates.empty()) {
        enriched.target_text_candidates = derive_text_candidates(context.task, parsed_plan);
    }

    if (enriched.phase_hints.empty()) {
        enriched.phase_hints = enriched.steps;
    }

    if (prefers_editable_input) {
        enriched.steps.insert(enriched.steps.begin(), "Use visible editable field");
        enriched.phase_hints.insert(enriched.phase_hints.begin(), "Set text in visible editable field");
    } else if (has_direct_click_match) {
        enriched.steps.insert(enriched.steps.begin(), "Use direct visible target");
        enriched.phase_hints.insert(enriched.phase_hints.begin(), "Click visible target already on screen");
    }

    if (app_match) {
        enriched.goal = compact_single_line("Inside target app: " + enriched.goal, 96);
    }
    return enriched;
}

static std::optional<extracted_steps_plan> normalize_plan_for_done(
    const std::optional<extracted_steps_plan> & raw_plan,
    bool app_match
) {
    if (!raw_plan.has_value()) {
        return std::nullopt;
    }

    extracted_steps_plan normalized = *raw_plan;
    normalized.apps = expand_expected_apps(normalized.apps);

    const auto filter_steps = [&](std::vector<std::string> & steps) {
        std::vector<std::string> filtered;
        filtered.reserve(steps.size());
        for (const auto & step : steps) {
            const auto compact = compact_single_line(step, 96);
            if (trim_copy(compact).empty()) {
                continue;
            }
            if (app_match && step_is_open_target_app_step(compact, normalized.apps)) {
                continue;
            }
            filtered.push_back(compact);
        }
        steps = std::move(filtered);
    };

    filter_steps(normalized.steps);
    filter_steps(normalized.phase_hints);
    return normalized;
}

static size_t normalized_plan_step_count(const std::optional<extracted_steps_plan> & plan) {
    if (!plan.has_value()) {
        return 0;
    }

    size_t count = 0;
    for (const auto & step : plan->steps) {
        if (!trim_copy(step).empty()) {
            ++count;
        }
    }
    return count;
}

static size_t required_done_history(const std::optional<extracted_steps_plan> & plan) {
    return std::max<size_t>(2, normalized_plan_step_count(plan));
}

static bool app_matches_expected(const std::string & current_app, const std::vector<std::string> & expected_apps) {
    if (expected_apps.empty()) {
        return false;
    }

    const auto full = to_lower_basic_multilang(current_app);
    const auto short_name = to_lower_basic_multilang(shorten_app_name(current_app));
    for (const auto & expected : expected_apps) {
        if (expected.empty()) {
            continue;
        }
        if (full.find(expected) != std::string::npos || short_name.find(expected) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static std::string select_relevant_phase_step(
    const extracted_steps_plan & plan,
    bool app_match
) {
    const auto & source = !plan.phase_hints.empty() ? plan.phase_hints : plan.steps;
    for (const auto & step : source) {
        if (trim_copy(step).empty()) {
            continue;
        }
        if (app_match && step_is_open_target_app_step(step, plan.apps)) {
            continue;
        }
        return compact_single_line(step, 96);
    }

    if (app_match && !plan.apps.empty()) {
        return "Find visible target inside current app";
    }
    return compact_single_line(plan.goal, 96);
}

static phase_gate_result build_phase_gate_result(
    const prompt_context & context,
    const extracted_steps_plan & plan,
    const std::string & screen_name,
    screen_mode_t screen_mode,
    const json & grouped,
    const std::vector<int32_t> & task_direct_click_ids,
    const std::vector<int32_t> & editable_ids,
    bool prefers_back_navigation
) {
    phase_gate_result gate;
    const bool app_match = app_matches_expected(screen_name, plan.apps);
    const auto phase_step = select_relevant_phase_step(plan, app_match);
    gate.phase_goal = phase_step.empty() ? plan.goal : phase_step;

    if (!plan.apps.empty() && !app_match && screen_mode == screen_mode_t::other_app && !prefers_back_navigation) {
        gate.phase_goal = "Return to launcher/home first";
        gate.phase_reason = "wrong_app";
        gate.allow_click = false;
        gate.allow_set_text = false;
        gate.allow_back = true;
        gate.force_back = true;
        return gate;
    }

    if (!plan.apps.empty() && !app_match && screen_mode == screen_mode_t::launcher_or_app_drawer) {
        gate.phase_goal = "Open target app";
        gate.phase_reason = "open_target_app";
        gate.allow_click = true;
        gate.allow_set_text = false;
        gate.allow_back = prefers_back_navigation;
        gate.direct_click_ids = collect_direct_click_match_ids(grouped, join_compact(plan.apps, " ", 4));
        return gate;
    }

    if (step_mentions_back_home_like(phase_step)) {
        gate.phase_reason = "phase_back_home";
        gate.allow_click = false;
        gate.allow_set_text = false;
        gate.allow_back = true;
        gate.force_back = !app_match && screen_mode != screen_mode_t::launcher_or_app_drawer;
        return gate;
    }

    if (app_match) {
        gate.direct_click_ids = collect_direct_click_match_ids(grouped, phase_step);
        if (gate.direct_click_ids.empty()) {
            gate.direct_click_ids = task_direct_click_ids;
        }
        if (!gate.direct_click_ids.empty()) {
            gate.phase_reason = "phase_direct_click";
            gate.allow_click = true;
            gate.allow_set_text = false;
            gate.allow_back = prefers_back_navigation;
            return gate;
        }

        if (!editable_ids.empty() && (step_mentions_input_like(phase_step) || prompt_requests_text_edit(context.task))) {
            gate.phase_reason = "phase_set_text";
            gate.allow_click = false;
            gate.allow_set_text = true;
            gate.allow_back = prefers_back_navigation;
            return gate;
        }
    }

    gate.phase_reason = "generic";
    gate.allow_click = true;
    gate.allow_set_text = false;
    gate.allow_back = prefers_back_navigation;
    gate.direct_click_ids = task_direct_click_ids;
    return gate;
}

static std::string build_item_label_for_prompt(const json & item) {
    const auto text = normalized_field(item, "text");
    const auto content_desc = normalized_field(item, "contentDesc");
    if (text.has_value() && content_desc.has_value()) {
        if (*text == *content_desc) {
            return normalize_compact_label(*text, 64);
        }
        return normalize_compact_label(*text, 32) + ":" + normalize_compact_label(*content_desc, 32);
    }
    if (text.has_value()) {
        return normalize_compact_label(*text, 64);
    }
    if (content_desc.has_value()) {
        return normalize_compact_label(*content_desc, 64);
    }
    return "";
}

static std::string build_item_metadata_for_prompt(const json & item) {
    std::vector<std::string> parts;
    if (const auto role = normalized_field(item, "role")) {
        parts.push_back("role=" + normalize_compact_label(*role, 32));
    }
    if (const auto state = normalized_field(item, "state")) {
        parts.push_back("state=" + normalize_compact_label(*state, 32));
    }
    if (const auto input_type = normalized_field(item, "inputType")) {
        parts.push_back("type=" + normalize_compact_label(*input_type, 24));
    }
    if (const auto error = normalized_field(item, "error")) {
        parts.push_back("error=" + normalize_compact_label(*error, 32));
    }
    return join_compact(parts, ";", 4);
}

static void collect_visible_static_lines_recursive(
    const json & node,
    std::vector<std::string> & out,
    std::unordered_set<std::string> & seen,
    size_t max_items
) {
    if (out.size() >= max_items || !node.is_object()) {
        return;
    }

    const bool actionable = node.contains("attrs") && node.at("attrs").is_array() && !node.at("attrs").empty();
    if (!actionable) {
        const auto label = build_item_label_for_prompt(node);
        if (!label.empty()) {
            const auto lowered = to_lower_basic_multilang(label);
            if (seen.insert(lowered).second) {
                out.push_back(label);
            }
        }
    }

    const auto children_it = node.find("children");
    if (children_it == node.end() || !children_it->is_array()) {
        return;
    }
    for (const auto & child : *children_it) {
        collect_visible_static_lines_recursive(child, out, seen, max_items);
        if (out.size() >= max_items) {
            return;
        }
    }
}

static std::string build_interactive_toon(const json & grouped) {
    std::string out = "interactive:\n";
    out += "  id | label | resId | hint | meta | attrs\n";

    const auto item_refs = collect_unique_item_refs(grouped);
    std::vector<const json *> sorted = item_refs;
    std::sort(sorted.begin(), sorted.end(), [](const json * lhs, const json * rhs) {
        const auto lhs_id = lhs->at("id").get<int32_t>();
        const auto rhs_id = rhs->at("id").get<int32_t>();
        return lhs_id < rhs_id;
    });

    for (const auto * item_ptr : sorted) {
        const auto & item = *item_ptr;
        const int32_t id = item.at("id").get<int32_t>();
        std::vector<std::string> attrs;
        for (auto it = grouped.begin(); it != grouped.end(); ++it) {
            if (!it.value().is_array()) {
                continue;
            }
            for (const auto & group_item : it.value()) {
                const auto id_it = group_item.find("id");
                if (id_it != group_item.end() && id_it->is_number_integer() && id_it->get<int32_t>() == id) {
                    attrs.push_back(it.key());
                    break;
                }
            }
        }
        std::sort(attrs.begin(), attrs.end());
        attrs.erase(std::unique(attrs.begin(), attrs.end()), attrs.end());
        out += "  ";
        out += std::to_string(id);
        out += " | ";
        out += build_item_label_for_prompt(item);
        out += " | ";
        if (const auto res_id = normalized_field(item, "resId")) {
            out += *res_id;
        }
        out += " | ";
        if (item.find("text") == item.end()) {
            if (const auto hint = normalized_field(item, "hint")) {
                out += *hint;
            }
        }
        out += " | ";
        out += build_item_metadata_for_prompt(item);
        out += " | ";
        out += join_compact(attrs, ",", 6);
        out += "\n";
    }
    return out;
}

static std::string build_runtime_toon(
    const json & root,
    const json & grouped,
    std::vector<std::string> * static_lines_out = nullptr
) {
    std::vector<std::string> static_lines;
    std::unordered_set<std::string> seen;
    collect_visible_static_lines_recursive(root, static_lines, seen, 10);
    if (static_lines_out != nullptr) {
        *static_lines_out = static_lines;
    }

    std::string out = "visible_static:\n";
    if (static_lines.empty()) {
        out += "  -\n";
    } else {
        for (const auto & line : static_lines) {
            out += "  - ";
            out += line;
            out += "\n";
        }
    }
    out += build_interactive_toon(grouped);
    return out;
}

std::string build_runtime_toon_for_test(const json & root, const json & grouped) {
    return build_runtime_toon(root, grouped, nullptr);
}

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

static bool prompt_requests_back_navigation(const std::string & user_prompt) {
    const auto lowered_ascii = to_lower_ascii(user_prompt);
    const auto lowered_multilang = to_lower_basic_multilang(user_prompt);
    const std::vector<std::string> english_keywords = {
        "go back", "back", "go out", "exit", "leave", "return", "not in ", "wrong app", "wrong screen"
    };
    const std::vector<std::string> russian_keywords = {
        u8"назад", u8"вернись", u8"вернуться", u8"выйди", u8"выйти", u8"закрой",
        u8"не в ", u8"не там", u8"не тот экран", u8"выйти из", u8"вернуться из"
    };

    return contains_any_substring(lowered_ascii, english_keywords) ||
        contains_any_substring(lowered_multilang, russian_keywords);
}

enum class desired_state_t {
    unknown = 0,
    enabled = 1,
    disabled = 2,
};

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

static desired_state_t detect_desired_state(const std::string & user_prompt) {
    const bool wants_enabled = prompt_wants_enabled_state(user_prompt);
    const bool wants_disabled = prompt_wants_disabled_state(user_prompt);
    if (wants_enabled && !wants_disabled) {
        return desired_state_t::enabled;
    }
    if (wants_disabled && !wants_enabled) {
        return desired_state_t::disabled;
    }
    return desired_state_t::unknown;
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
    static const std::vector<std::string> preferred_columns = {
        "id", "resId", "role", "state", "error", "inputType", "hint", "text", "contentDesc"
    };

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

        std::unordered_map<std::string, size_t> encounter_order;
        for (size_t index = 0; index < columns.size(); ++index) {
            encounter_order.emplace(columns[index], index);
        }
        std::stable_sort(columns.begin(), columns.end(), [&](const std::string & lhs, const std::string & rhs) {
            const auto lhs_it = std::find(preferred_columns.begin(), preferred_columns.end(), lhs);
            const auto rhs_it = std::find(preferred_columns.begin(), preferred_columns.end(), rhs);
            const auto lhs_rank = lhs_it == preferred_columns.end()
                ? preferred_columns.size() + encounter_order.at(lhs)
                : static_cast<size_t>(std::distance(preferred_columns.begin(), lhs_it));
            const auto rhs_rank = rhs_it == preferred_columns.end()
                ? preferred_columns.size() + encounter_order.at(rhs)
                : static_cast<size_t>(std::distance(preferred_columns.begin(), rhs_it));
            return lhs_rank < rhs_rank;
        });

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

static const json * find_grouped_item_by_id(const json & grouped, int32_t selected_id) {
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        if (!it.value().is_array()) {
            continue;
        }
        for (const auto & item : it.value()) {
            const auto id_it = item.find("id");
            if (id_it != item.end() &&
                id_it->is_number_integer() &&
                id_it->get<int32_t>() == selected_id) {
                return &item;
            }
        }
    }
    return nullptr;
}

static std::string grouped_item_label(const json & item) {
    const auto text = normalized_field(item, "text");
    const auto content_desc = normalized_field(item, "contentDesc");
    if (text.has_value() && content_desc.has_value() && *text != *content_desc) {
        return *text + ":" + *content_desc;
    }
    if (text.has_value()) {
        return *text;
    }
    if (content_desc.has_value()) {
        return *content_desc;
    }
    if (const auto class_name = normalized_field(item, "class")) {
        return *class_name;
    }
    return "";
}

static bool text_matches_placeholder(const std::string & text_value, const json & item) {
    const auto normalized_text = to_lower_basic_multilang(trim_copy(text_value));
    if (normalized_text.empty()) {
        return false;
    }
    const auto label = to_lower_basic_multilang(grouped_item_label(item));
    return !label.empty() && normalized_text == label;
}

static std::string build_candidate_signature(
    const model_action_response & response,
    const json & grouped,
    const std::string & screen_name
) {
    if (response.done) {
        return "d#-1@" + shorten_app_name(screen_name);
    }
    char action_code = 'a';
    if (response.action_type == "click") {
        action_code = 'c';
    } else if (response.action_type == "set_text") {
        action_code = 't';
    } else if (response.action_type == "back") {
        action_code = 'b';
    }

    std::optional<int32_t> id;
    std::optional<std::string> label;
    if (response.selected_id >= 0) {
        id = response.selected_id;
        if (const auto * item = find_grouped_item_by_id(grouped, response.selected_id)) {
            const auto item_label = grouped_item_label(*item);
            if (!item_label.empty()) {
                label = item_label;
            }
        }
    } else if (response.action_type == "back") {
        label = "back";
    }
    return build_history_signature(action_code, id, label, shorten_app_name(screen_name));
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

static std::vector<int32_t> collect_state_action_ids(
    const json & grouped,
    desired_state_t desired_state,
    const std::vector<std::string> & task_terms
) {
    std::vector<int32_t> ids;
    std::unordered_map<int32_t, int> best_score_by_id;

    const auto consider_attr = [&](const char * attr_name) {
        const auto attr_it = grouped.find(attr_name);
        if (attr_it == grouped.end() || !attr_it->is_array()) {
            return;
        }
        for (const auto & item : *attr_it) {
            const auto id_it = item.find("id");
            if (id_it == item.end() || !id_it->is_number_integer()) {
                continue;
            }
            const int32_t id = id_it->get<int32_t>();
            const int score = score_item_for_terms(item, task_terms).score;
            auto pos = best_score_by_id.find(id);
            if (pos == best_score_by_id.end()) {
                best_score_by_id[id] = score;
            } else {
                pos->second = std::max(pos->second, score);
            }
        }
    };

    if (desired_state == desired_state_t::enabled) {
        consider_attr("unchecked");
    } else if (desired_state == desired_state_t::disabled) {
        consider_attr("checked");
    } else {
        consider_attr("checked");
        consider_attr("unchecked");
    }

    int best_positive = 0;
    for (const auto & [id, score] : best_score_by_id) {
        if (score > best_positive) {
            best_positive = score;
        }
    }

    for (const auto & [id, score] : best_score_by_id) {
        if (best_positive > 0) {
            if (score == best_positive) {
                ids.push_back(id);
            }
        } else {
            ids.push_back(id);
        }
    }

    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

static done_validation_result validate_done_response(
    const json & grouped,
    const std::string & task,
    const prompt_context & context,
    const std::optional<extracted_steps_plan> & plan
) {
    done_validation_result result;
    const size_t min_history = required_done_history(plan);

    if (context.history_tokens.size() < min_history) {
        result.reason =
            "history below floor (required=" + std::to_string(min_history) +
            " current=" + std::to_string(context.history_tokens.size()) + ")";
        return result;
    }

    const auto task_terms = extract_task_match_terms(task);
    const bool state_task = prompt_requests_state_change(task);
    const bool repeat_id_loop = context.repeated_tail_same_id >= 2 || context.repeated_tail_same_signature >= 2;
    const bool has_progress_history = !context.history_ids.empty();

    if (repeat_id_loop) {
        result.reason = "repeat-id loop";
        return result;
    }

    if (state_task) {
        const auto desired_state = detect_desired_state(task);
        const int checked_best = best_match_score_for_attr(grouped, "checked", task_terms);
        const int unchecked_best = best_match_score_for_attr(grouped, "unchecked", task_terms);
        const int min_state_score = 5;
        const int min_state_margin = 2;

        if (!has_progress_history) {
            result.reason = "state task without progress history";
            return result;
        }

        if (desired_state == desired_state_t::enabled &&
            checked_best >= min_state_score &&
            checked_best >= unchecked_best + min_state_margin) {
            result.accepted = true;
            result.reason = "state target is checked (required=" + std::to_string(min_history) + ")";
            return result;
        }
        if (desired_state == desired_state_t::disabled &&
            unchecked_best >= min_state_score &&
            unchecked_best >= checked_best + min_state_margin) {
            result.accepted = true;
            result.reason = "state target is unchecked (required=" + std::to_string(min_history) + ")";
            return result;
        }

        if (desired_state == desired_state_t::unknown &&
            has_progress_history &&
            std::max(checked_best, unchecked_best) >= 7 &&
            std::abs(checked_best - unchecked_best) >= 3) {
            result.accepted = true;
            result.reason = "state strongly resolved (required=" + std::to_string(min_history) + ")";
            return result;
        }

        if (desired_state == desired_state_t::enabled) {
            result.reason = "state conflict: target not confidently checked";
        } else if (desired_state == desired_state_t::disabled) {
            result.reason = "state conflict: target not confidently unchecked";
        } else {
            result.reason = "state target not yet reached";
        }
        return result;
    }

    const bool lookup_task = prompt_requests_lookup_input(task);
    if (!lookup_task || task_terms.empty()) {
        result.reason = "no strong completion trigger";
        return result;
    }
    if (!has_progress_history) {
        result.reason = "lookup task without progress history";
        return result;
    }

    const auto item_refs = collect_unique_item_refs(grouped);
    for (const json * item_ptr : item_refs) {
        const auto stats = score_item_for_terms(*item_ptr, task_terms);
        if (stats.score < 7 || stats.matched_terms == 0 || stats.longest_term < 4) {
            continue;
        }

        std::string searchable;
        if (const auto text = normalized_field(*item_ptr, "text")) {
            searchable += *text;
            searchable.push_back(' ');
        }
        if (const auto content_desc = normalized_field(*item_ptr, "contentDesc")) {
            searchable += *content_desc;
            searchable.push_back(' ');
        }
        const auto searchable_lower = to_lower_basic_multilang(searchable);
        const bool search_like =
            searchable_lower.find("search") != std::string::npos ||
            searchable_lower.find(u8"поиск") != std::string::npos ||
            searchable_lower.find(u8"найти") != std::string::npos;
        if (search_like) {
            continue;
        }

        if (item_is_upper_region(*item_ptr)) {
            result.accepted = true;
            result.reason = "strong target match in upper region (required=" + std::to_string(min_history) + ")";
            return result;
        }
    }

    result.reason = "no strong completion signal";
    return result;
}

done_validation_result validate_done_response_for_test(
    const json & grouped,
    const std::string & task,
    const prompt_context & context,
    const std::optional<extracted_steps_plan> & plan
) {
    return validate_done_response(grouped, task, context, plan);
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
    size_t text_candidate_count,
    bool allow_click,
    bool allow_back,
    bool allow_done
) {
    const std::string click_id_rule = build_id_rule(ids);
    const std::string editable_id_rule = build_id_rule(editable_ids);
    std::string grammar;
    grammar += "root ::= ws (";
    if (allow_done) {
        grammar += "done-response | ";
    }
    grammar += "no-match-response";
    if (allow_back) {
        grammar += " | back-response";
    }
    if (allow_click && !ids.empty()) {
        grammar += " | click-response";
    }
    if (!editable_ids.empty() && text_candidate_count > 0) {
        grammar += " | set-text-response";
    }
    grammar += ") ws\n";
    if (allow_done) {
        grammar += "done-response ::= \"6\"\n";
    }
    grammar += "no-match-response ::= \"0\"\n";
    if (allow_back) {
        grammar += "back-response ::= \"5\"\n";
    }
    if (allow_click && !ids.empty()) {
        grammar += "click-response ::= \"1\" \":\" click-id-value\n";
        grammar += "click-id-value ::= ";
        grammar += click_id_rule;
        grammar += "\n";
    }
    if (!editable_ids.empty() && text_candidate_count > 0) {
        grammar += "set-text-response ::= \"2\" \":\" set-text-id-value \":\" text-index-value\n";
        grammar += "set-text-id-value ::= ";
        grammar += editable_id_rule;
        grammar += "\n";
        grammar += "text-index-value ::= ";
        for (size_t i = 0; i < text_candidate_count; ++i) {
            if (i != 0) {
                grammar += " | ";
            }
            grammar += "\"";
            grammar += std::to_string(i);
            grammar += "\"";
        }
        grammar += "\n";
    }
    grammar += "ws ::= | \" \" ws | \"\\n\" ws | \"\\r\" ws | \"\\t\" ws\n";
    return grammar;
}

model_action_response extract_action_response_from_chat_response(const std::string & response_body) {
    return extract_action_response_from_chat_response_with_content(response_body, nullptr);
}

void validate_action_response_for_grouped(const json & grouped, const model_action_response & response) {
    if (response.done) {
        return;
    }

    if (response.action_type.empty()) {
        if (response.selected_id < 0) {
            return;
        }
        throw std::runtime_error("model response missing action_type for selected id");
    }

    if (response.action_type == "back") {
        if (response.selected_id != -1) {
            throw std::runtime_error("model response back action must use selected_id=-1");
        }
        if (response.text.has_value() && !trim_copy(*response.text).empty()) {
            throw std::runtime_error("model response back action must not include text");
        }
        return;
    }

    if (response.selected_id < 0) {
        throw std::runtime_error("model response selected id must be non-negative for click/set_text");
    }

    if (response.action_type == "click") {
        return;
    }

    if (response.action_type != "set_text") {
        throw std::runtime_error("model response action must be click, set_text, or back");
    }

    if ((!response.text.has_value() || trim_copy(*response.text).empty()) && response.text_index < 0) {
        throw std::runtime_error("model response set_text action requires non-empty text or text_index");
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
    bool done = false,
    bool path_is_null = false
) {
    auto * result = static_cast<loki_action_result_t *>(std::calloc(1, sizeof(loki_action_result_t)));
    if (result == nullptr) {
        return nullptr;
    }
    result->status = status;
    result->selected_id = selected_id;
    result->path_json = path_is_null ? nullptr : duplicate_c_string_global(path_json);
    result->error_message = duplicate_c_string_global(error_message);
    result->action_type = action_type ? duplicate_c_string_global(*action_type) : nullptr;
    result->text = text ? duplicate_c_string_global(*text) : nullptr;
    result->done = done;
    if ((!path_is_null && result->path_json == nullptr && !path_json.empty()) ||
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
    const loki_action::prompt_context & context,
    const std::string & screen_name,
    const std::string & toon,
    const std::string & grammar,
    const char * system_prompt,
    const std::optional<loki_action::extracted_steps_plan> & extracted_plan,
    const std::string & screen_mode,
    const std::string & phase_goal
) {
    return loki_action::json{
        {"model", "default"},
        {"messages", loki_action::json::array({
            loki_action::json{
                {"role", "system"},
                {"content", system_prompt},
            },
            loki_action::json{
                {"role", "user"},
                {"content", loki_action::build_user_content_from_context(
                    context,
                    screen_name,
                    toon,
                    extracted_plan,
                    screen_mode,
                    phase_goal
                )}
            },
        })},
        {"grammar", grammar},
        {"cache_prompt", true},
        {"max_tokens", 24},
        {"stream", false},
        {"temperature", 0.0},
        {"top_k", 1},
        {"top_p", 1.0},
        {"min_p", 0.0},
        {"repeat_penalty", 1.0},
    };
}

std::string build_done_gate_grammar_global() {
    return "root ::= ws (\"0\" | \"1\") ws\nws ::= | \" \" ws | \"\\n\" ws | \"\\r\" ws | \"\\t\" ws\n";
}

loki_action::json build_done_gate_payload_global(
    const loki_action::prompt_context & context,
    const std::string & screen_name,
    const std::vector<std::string> & static_lines,
    const loki_action::extracted_steps_plan & plan,
    const std::string & phase_goal,
    const std::string & screen_mode
) {
    std::string user_content;
    user_content += "Task: ";
    user_content += context.task;
    user_content += "\nApp: ";
    user_content += screen_name;
    if (!screen_mode.empty()) {
        user_content += "\nScreenMode: ";
        user_content += screen_mode;
    }
    user_content += "\nGoal: ";
    user_content += plan.goal;
    if (!phase_goal.empty()) {
        user_content += "\nPhaseGoal: ";
        user_content += phase_goal;
    }
    if (!plan.done_when.empty()) {
        user_content += "\nDoneWhen: ";
        user_content += plan.done_when;
    }
    if (!static_lines.empty()) {
        user_content += "\nVisible:";
        for (const auto & line : static_lines) {
            user_content += "\n- ";
            user_content += line;
        }
    }
    if (!context.history_entries.empty()) {
        user_content += "\nRecent:";
        const size_t begin = context.history_entries.size() > 3 ? context.history_entries.size() - 3 : 0;
        for (size_t i = begin; i < context.history_entries.size(); ++i) {
            user_content += "\n- ";
            user_content += context.history_entries[i];
        }
    }

    return loki_action::json{
        {"model", "default"},
        {"messages", loki_action::json::array({
            loki_action::json{{"role", "system"}, {"content", loki_action::DONE_CHECK_PROMPT}},
            loki_action::json{{"role", "user"}, {"content", user_content}},
        })},
        {"grammar", build_done_gate_grammar_global()},
        {"cache_prompt", true},
        {"max_tokens", 4},
        {"stream", false},
        {"temperature", 0.0},
        {"top_k", 1},
        {"top_p", 1.0},
        {"min_p", 0.0},
        {"repeat_penalty", 1.0},
    };
}

bool parse_done_gate_content_global(const std::string & content) {
    const auto trimmed = loki_action::trim_copy(content);
    if (trimmed == "1") {
        return true;
    }
    if (trimmed == "0") {
        return false;
    }
    throw std::runtime_error("invalid done gate content");
}

loki_action::json build_step_extractor_payload_global(
    const loki_action::prompt_context & context,
    const std::string & screen_name,
    const std::string & current_app_short,
    const std::string & screen_mode,
    const std::vector<std::string> & static_lines,
    const std::vector<std::string> & top_interactive
) {
    return loki_action::json{
        {"model", "default"},
        {"messages", loki_action::json::array({
            loki_action::json{
                {"role", "system"},
                {"content", loki_action::STEP_EXTRACTOR_PROMPT},
            },
            loki_action::json{
                {"role", "user"},
                {"content", loki_action::build_steps_extractor_user_content(
                    context,
                    screen_name,
                    current_app_short,
                    screen_mode,
                    static_lines,
                    top_interactive
                )}
            },
        })},
        {"grammar", loki_action::build_steps_extractor_grammar()},
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

std::optional<loki_action::extracted_steps_plan> run_steps_extractor_global(
    const loki_action::prompt_context & context,
    const std::string & screen_name,
    const std::string & current_app_short,
    const std::string & screen_mode,
    const std::vector<std::string> & static_lines,
    const std::vector<std::string> & top_interactive,
    const std::string & host_copy,
    int32_t resolved_port,
    std::string & stage,
    std::string & last_model_response
) {
    if (loki_action::trim_copy(context.task).empty() || context.step_number > 3) {
        return std::nullopt;
    }

    try {
        stage = "prepare_http:steps-extractor";
        const auto payload = build_step_extractor_payload_global(
            context,
            screen_name,
            current_app_short,
            screen_mode,
            static_lines,
            top_interactive
        );
        const auto payload_body = payload.dump();

        stage = "http_post:steps-extractor";
        const auto http_result = loki_action::post_json_via_socket(
            host_copy,
            resolved_port,
            "/v1/chat/completions",
            payload_body,
            5,
            120,
            5
        );
        if (!http_result.ok || http_result.status != 200) {
            LOKI_LOGI(
                "STEP %d PLAN: extractor skipped, status=%d ok=%s",
                context.step_number,
                http_result.status,
                http_result.ok ? "true" : "false"
            );
            return std::nullopt;
        }

        stage = "parse_model_response:steps-extractor";
        last_model_response = http_result.body;
        const auto root = loki_action::json::parse(http_result.body);
        const auto content = loki_action::extract_message_content(root);
        const auto parsed = loki_action::parse_steps_extractor_content(content);
        if (!parsed.has_value()) {
            LOKI_LOGI("STEP %d PLAN: extractor returned no usable plan", context.step_number);
            return std::nullopt;
        }

        const auto apps_line = loki_action::join_compact(parsed->apps, ",", 3);
        const auto steps_line = loki_action::join_compact(parsed->steps, " | ", 4);
        const auto phase_line = loki_action::join_compact(parsed->phase_hints, " | ", 4);
        LOKI_LOGI(
            "STEP %d PLAN-RAW: goal=\"%s\" apps=\"%s\" steps=\"%s\" phase=\"%s\"",
            context.step_number,
            loki_action::truncate_for_log(loki_action::escape_for_log(parsed->goal), 120).c_str(),
            loki_action::truncate_for_log(loki_action::escape_for_log(apps_line), 120).c_str(),
            loki_action::truncate_for_log(loki_action::escape_for_log(steps_line), 220).c_str(),
            loki_action::truncate_for_log(loki_action::escape_for_log(phase_line), 220).c_str()
        );
        return parsed;
    } catch (const std::exception & e) {
        LOKI_LOGI("STEP %d PLAN: extractor failed: %s", context.step_number, e.what());
        return std::nullopt;
    }
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
        const auto screen_name_it = screen.find("screen");
        const std::string screen_name =
            screen_name_it != screen.end() && screen_name_it->is_string()
                ? screen_name_it->get<std::string>()
                : "unknown";
        const std::string current_app_short = loki_action::shorten_app_name(screen_name);

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
        std::vector<std::string> static_lines;
        const auto full_toon = loki_action::build_runtime_toon(*root_it, grouped, &static_lines);
        const auto top_interactive = loki_action::build_top_interactive_summary(grouped);
        loki_action::log_multiline_toon(full_toon, prompt_context.step_number);
        const auto bootstrap_expected_apps = loki_action::expand_expected_apps(
            loki_action::infer_apps_from_task(prompt_context.task)
        );
        const auto bootstrap_screen_mode = loki_action::detect_screen_mode(
            screen_name,
            bootstrap_expected_apps,
            static_lines,
            top_interactive
        );
        LOKI_LOGI(
            "STEP %d SCREEN: app=%s mode=%s static=\"%s\" top=\"%s\"",
            prompt_context.step_number,
            current_app_short.c_str(),
            loki_action::screen_mode_name(bootstrap_screen_mode),
            loki_action::truncate_for_log(loki_action::escape_for_log(loki_action::join_compact(static_lines, " | ", 5)), 180).c_str(),
            loki_action::truncate_for_log(loki_action::escape_for_log(loki_action::join_compact(top_interactive, " | ", 6)), 180).c_str()
        );
        const auto candidate_ids = loki_action::collect_candidate_ids(grouped);
        const auto editable_candidate_ids = loki_action::collect_candidate_ids_for_attr(grouped, "editable");
        const auto state_candidate_ids = loki_action::collect_state_candidate_ids(grouped);
        const auto desired_state = loki_action::detect_desired_state(prompt_context.task);
        const auto task_match_terms = loki_action::extract_task_match_terms(prompt_context.task);
        const auto state_action_candidate_ids = loki_action::collect_state_action_ids(
            grouped,
            desired_state,
            task_match_terms
        );
        const auto direct_click_candidate_ids = loki_action::collect_direct_click_match_ids(
            grouped,
            prompt_context.task
        );
        const bool has_direct_click_match = !direct_click_candidate_ids.empty();
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

        const auto raw_extracted_plan = run_steps_extractor_global(
            prompt_context,
            screen_name,
            current_app_short,
            loki_action::screen_mode_name(bootstrap_screen_mode),
            static_lines,
            top_interactive,
            host_copy,
            resolved_port,
            stage,
            last_model_response
        );
        const bool prefers_text_edit_initial = loki_action::prompt_requests_text_edit(prompt_context.task);
        const bool prefers_lookup_input_initial = loki_action::prompt_requests_lookup_input(prompt_context.task);
        const bool prefers_back_navigation_initial = loki_action::prompt_requests_back_navigation(prompt_context.task);
        const bool prefers_state_action_initial =
            loki_action::prompt_requests_state_change(prompt_context.task) && !state_candidate_ids.empty();
        const bool prefers_editable_input_initial =
            !editable_candidate_ids.empty() &&
            (prefers_text_edit_initial || prefers_lookup_input_initial) &&
            !has_direct_click_match &&
            !prefers_state_action_initial;
        const bool raw_plan_app_match = loki_action::app_matches_expected(
            screen_name,
            loki_action::expand_expected_apps(
                raw_extracted_plan.has_value() ? raw_extracted_plan->apps : bootstrap_expected_apps
            )
        );
        const auto done_plan = loki_action::normalize_plan_for_done(raw_extracted_plan, raw_plan_app_match);
        const auto enriched_plan = loki_action::enrich_extracted_plan(
            prompt_context,
            raw_extracted_plan,
            raw_plan_app_match,
            has_direct_click_match,
            prefers_editable_input_initial
        );
        const auto expected_apps = loki_action::expand_expected_apps(enriched_plan.apps);
        const auto final_screen_mode = loki_action::detect_screen_mode(
            screen_name,
            expected_apps,
            static_lines,
            top_interactive
        );
        const auto phase_gate = loki_action::build_phase_gate_result(
            prompt_context,
            enriched_plan,
            screen_name,
            final_screen_mode,
            grouped,
            direct_click_candidate_ids,
            editable_candidate_ids,
            prefers_back_navigation_initial
        );
        const bool allow_done_for_app = final_screen_mode == loki_action::screen_mode_t::target_app;
        LOKI_LOGI(
            "STEP %d PLAN: goal=\"%s\" phase=\"%s\" mode=%s apps=\"%s\" steps=\"%s\"",
            prompt_context.step_number,
            loki_action::truncate_for_log(loki_action::escape_for_log(enriched_plan.goal), 120).c_str(),
            loki_action::truncate_for_log(loki_action::escape_for_log(phase_gate.phase_goal), 120).c_str(),
            loki_action::screen_mode_name(final_screen_mode),
            loki_action::truncate_for_log(loki_action::escape_for_log(loki_action::join_compact(expected_apps, ",", 4)), 80).c_str(),
            loki_action::truncate_for_log(loki_action::escape_for_log(loki_action::join_compact(enriched_plan.steps, " | ", 4)), 220).c_str()
        );

        auto run_model_pass = [&](const char * pass_name,
                                  const char * system_prompt,
                                  const std::vector<int32_t> & pass_ids,
                                  const std::vector<int32_t> & pass_editable_ids,
                                  size_t text_candidate_count,
                                  bool allow_click,
                                  bool allow_back = false,
                                  bool allow_empty_candidates = false,
                                  bool allow_done = false) -> std::optional<loki_action::model_action_response> {
            if (pass_ids.empty() && !allow_empty_candidates) {
                LOKI_LOGI("STEP %d MODEL PASS: %s skipped (no candidates)", prompt_context.step_number, pass_name);
                return std::nullopt;
            }

            const std::string grammar = loki_action::build_action_response_grammar(
                pass_ids,
                pass_editable_ids,
                text_candidate_count,
                allow_click,
                allow_back,
                allow_done
            );

            LOKI_LOGI(
                "STEP %d PASS[%s]: ids=%zu editable=%zu text_choices=%zu click=%s back=%s done=%s",
                prompt_context.step_number,
                pass_name,
                pass_ids.size(),
                pass_editable_ids.size(),
                text_candidate_count,
                allow_click ? "true" : "false",
                allow_back ? "true" : "false",
                allow_done ? "true" : "false"
            );

            stage = std::string("prepare_http:") + pass_name;
            const auto payload = build_chat_request_payload_global(
                prompt_context,
                screen_name,
                full_toon,
                grammar,
                system_prompt,
                enriched_plan,
                loki_action::screen_mode_name(final_screen_mode),
                phase_gate.phase_goal
            );
            const std::string payload_body = payload.dump();

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
                "STEP %d RESP[%s]: status=%d bytes=%zu",
                prompt_context.step_number,
                pass_name,
                http_result.status,
                response_body.size()
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
                "STEP %d MODEL[%s]: \"%s\"",
                prompt_context.step_number,
                pass_name,
                loki_action::truncate_for_log(loki_action::escape_for_log(normalized_content), 160).c_str()
            );
            LOKI_LOGI(
                "STEP %d PARSED[%s]: id=%d action=%s text_index=%d text=\"%s\" done=%s",
                prompt_context.step_number,
                pass_name,
                action_response.selected_id,
                action_response.action_type.c_str(),
                action_response.text_index,
                action_response.text
                    ? loki_action::truncate_for_log(loki_action::escape_for_log(*action_response.text), 120).c_str()
                    : "",
                action_response.done ? "true" : "false"
            );
            return action_response;
        };

        loki_action::model_action_response action_response;
        bool has_action_response = false;
        const bool prefers_text_edit = prefers_text_edit_initial;
        const bool prefers_back_navigation = prefers_back_navigation_initial;
        const bool prefers_state_action = prefers_state_action_initial;
        const bool prefers_editable_input = phase_gate.allow_set_text && !editable_candidate_ids.empty();
        const bool phase_has_direct_click_match = !phase_gate.direct_click_ids.empty();
        LOKI_LOGI(
            "STEP %d PHASE: reason=%s allow_click=%s allow_set_text=%s allow_back=%s force_back=%s direct_ids=%zu",
            prompt_context.step_number,
            phase_gate.phase_reason.c_str(),
            phase_gate.allow_click ? "true" : "false",
            phase_gate.allow_set_text ? "true" : "false",
            phase_gate.allow_back ? "true" : "false",
            phase_gate.force_back ? "true" : "false",
            phase_gate.direct_click_ids.size()
        );
        LOKI_LOGI(
            "STEP %d GATE: app=%s expected=%s app_match=%s text_edit=%s lookup=%s back=%s state=%s editable=%s direct=%s text_choices=%zu total=%zu history=%zu repeat_id=%d repeat_sig=%d",
            prompt_context.step_number,
            loki_action::shorten_app_name(screen_name).c_str(),
            loki_action::join_compact(expected_apps, ",", 4).c_str(),
            allow_done_for_app ? "true" : "false",
            prefers_text_edit ? "true" : "false",
            prefers_lookup_input_initial ? "true" : "false",
            prefers_back_navigation ? "true" : "false",
            prefers_state_action ? "true" : "false",
            prefers_editable_input ? "true" : "false",
            phase_has_direct_click_match ? "true" : "false",
            enriched_plan.target_text_candidates.size(),
            candidate_ids.size(),
            prompt_context.history_tokens.size(),
            prompt_context.repeated_tail_same_id,
            prompt_context.repeated_tail_same_signature
        );

        auto try_accept_model_response = [&](const loki_action::model_action_response & candidate,
                                             const char * pass_name) -> bool {
            (void) pass_name;
            if (candidate.done) {
                const size_t required_history = loki_action::required_done_history(done_plan);
                (void) required_history;
                const auto done_check = loki_action::validate_done_response(
                    grouped, prompt_context.task, prompt_context, done_plan
                );
                if (!done_check.accepted) {
                    LOKI_LOGI(
                        "STEP %d DONE REJECTED: pass=%s reason=%s required_history=%zu history=%zu",
                        prompt_context.step_number,
                        pass_name,
                        done_check.reason.c_str(),
                        required_history,
                        prompt_context.history_tokens.size()
                    );
                    return false;
                }
                LOKI_LOGI(
                    "STEP %d DONE ACCEPTED: pass=%s reason=%s required_history=%zu history=%zu",
                    prompt_context.step_number,
                    pass_name,
                    done_check.reason.c_str(),
                    required_history,
                    prompt_context.history_tokens.size()
                );
            }

            if (!candidate.done && !prompt_context.history_signatures.empty()) {
                const auto candidate_signature = loki_action::build_candidate_signature(candidate, grouped, screen_name);
                const size_t check_depth = std::min<size_t>(3, prompt_context.history_signatures.size());
                for (size_t i = 0; i < check_depth; ++i) {
                    const auto & past_sig = prompt_context.history_signatures[
                        prompt_context.history_signatures.size() - 1 - i
                    ];
                    if (candidate_signature == past_sig) {
                        LOKI_LOGI(
                            "STEP %d ACTION REJECTED: pass=%s reason=repeat-signature-in-last-%zu sig=%s",
                            prompt_context.step_number,
                            pass_name,
                            i + 1,
                            candidate_signature.c_str()
                        );
                        return false;
                    }
                }
            }

            action_response = candidate;
            has_action_response = true;
            return true;
        };

        try {
            if (!has_action_response && phase_gate.force_back) {
                action_response.action_type = "back";
                action_response.selected_id = -1;
                has_action_response = true;
                LOKI_LOGI(
                    "STEP %d DECISION: force_back reason=%s",
                    prompt_context.step_number,
                    phase_gate.phase_reason.c_str()
                );
            }

            bool done_gate_open = false;
            if (!has_action_response && allow_done_for_app) {
                stage = "prepare_http:done-gate";
                const auto payload = build_done_gate_payload_global(
                    prompt_context,
                    screen_name,
                    static_lines,
                    enriched_plan,
                    phase_gate.phase_goal,
                    loki_action::screen_mode_name(final_screen_mode)
                );
                const auto http_result = loki_action::post_json_via_socket(
                    host_copy,
                    resolved_port,
                    "/v1/chat/completions",
                    payload.dump(),
                    5,
                    120,
                    5
                );
                if (http_result.ok && http_result.status == 200) {
                    last_model_response = http_result.body;
                    const auto root = loki_action::json::parse(http_result.body);
                    const auto content = loki_action::extract_message_content(root);
                    done_gate_open = parse_done_gate_content_global(content);
                    LOKI_LOGI(
                        "STEP %d DONE-GATE: app_match=true model=%s decision=%s",
                        prompt_context.step_number,
                        loki_action::truncate_for_log(loki_action::escape_for_log(content), 32).c_str(),
                        done_gate_open ? "open" : "closed"
                    );
                } else {
                    LOKI_LOGI(
                        "STEP %d DONE-GATE: skipped status=%d ok=%s",
                        prompt_context.step_number,
                        http_result.status,
                        http_result.ok ? "true" : "false"
                    );
                }
            }

            if (!has_action_response && prefers_state_action) {
                const auto & state_ids_for_pass =
                    state_action_candidate_ids.empty() ? state_candidate_ids : state_action_candidate_ids;
                const auto state_response = run_model_pass(
                    "state-priority",
                    loki_action::STATE_PRIORITY_PROMPT,
                    state_ids_for_pass,
                    {},
                    0,
                    true,
                    prefers_back_navigation
                );
                if (state_response.has_value() &&
                    (state_response->done || state_response->selected_id >= 0)) {
                    (void) try_accept_model_response(*state_response, "state-priority");
                }
            }

            if (!has_action_response && !prefers_text_edit && !prefers_state_action && phase_has_direct_click_match) {
                const auto direct_click_response = run_model_pass(
                    "direct-click-priority",
                    loki_action::DIRECT_CLICK_PRIORITY_PROMPT,
                    phase_gate.direct_click_ids,
                    {},
                    0,
                    true,
                    phase_gate.allow_back
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
                    enriched_plan.target_text_candidates.size(),
                    false,
                    false
                );
                if (editable_response.has_value() && editable_response->selected_id >= 0) {
                    (void) try_accept_model_response(*editable_response, "editable-priority");
                }
            }

            if (!has_action_response && phase_gate.allow_click && !phase_has_direct_click_match) {
                const bool fallback_to_non_editable =
                    prefers_editable_input || (phase_has_direct_click_match && !prefers_text_edit);
                const auto & fallback_ids = fallback_to_non_editable ? non_editable_candidate_ids : candidate_ids;
                std::vector<int32_t> fallback_editable_ids;
                if (!fallback_to_non_editable) {
                    fallback_editable_ids = phase_gate.allow_set_text ? editable_candidate_ids : std::vector<int32_t>{};
                }
                const auto fallback_response = run_model_pass(
                    fallback_to_non_editable ? "non-editable-fallback" : "default",
                    fallback_to_non_editable ? loki_action::CLICK_FALLBACK_PROMPT : loki_action::SYSTEM_PROMPT,
                    fallback_ids,
                    fallback_editable_ids,
                    fallback_to_non_editable || !phase_gate.allow_set_text ? 0 : enriched_plan.target_text_candidates.size(),
                    true,
                    phase_gate.allow_back,
                    false,
                    done_gate_open
                );
                if (fallback_response.has_value()) {
                    (void) try_accept_model_response(
                        *fallback_response,
                        fallback_to_non_editable ? "non-editable-fallback" : "default"
                    );
                }
            }

            if (!has_action_response && phase_gate.allow_back && !phase_gate.allow_click && !prefers_editable_input) {
                const auto back_response = run_model_pass(
                    "back-priority",
                    loki_action::CLICK_FALLBACK_PROMPT,
                    {},
                    {},
                    0,
                    false,
                    true,
                    true,
                    false
                );
                if (back_response.has_value() && back_response->action_type == "back") {
                    (void) try_accept_model_response(*back_response, "back-priority");
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

        if (action_response.action_type == "set_text" && !action_response.text.has_value()) {
            if (action_response.text_index < 0 ||
                static_cast<size_t>(action_response.text_index) >= enriched_plan.target_text_candidates.size()) {
                return make_result_global(
                    LOKI_ACTION_STATUS_INVALID_RESPONSE,
                    action_response.selected_id,
                    "[]",
                    "set_text text_index is out of range"
                );
            }
            action_response.text = enriched_plan.target_text_candidates[static_cast<size_t>(action_response.text_index)];
        }

        if (action_response.action_type == "set_text" && action_response.selected_id >= 0) {
            if (const auto * item = loki_action::find_grouped_item_by_id(grouped, action_response.selected_id)) {
                if (action_response.text.has_value() &&
                    loki_action::text_matches_placeholder(*action_response.text, *item)) {
                    return make_result_global(
                        LOKI_ACTION_STATUS_INVALID_RESPONSE,
                        action_response.selected_id,
                        "[]",
                        "set_text resolved to placeholder label"
                    );
                }
            }
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

        if (action_response.action_type == "back") {
            return make_result_global(
                LOKI_ACTION_STATUS_OK,
                -1,
                "",
                "",
                action_response.action_type,
                std::nullopt,
                false,
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
    return "1.6.1";
}

} // extern "C"

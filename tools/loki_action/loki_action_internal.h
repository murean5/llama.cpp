#ifndef LOKI_ACTION_INTERNAL_H
#define LOKI_ACTION_INTERNAL_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace loki_action {

using json = nlohmann::ordered_json;

json group_by_attrs_textual(const json & tree);
json prepare_for_toon(const json & grouped);
std::string json_to_toon(const json & prepared);
std::string find_path_json_by_id(const json & grouped, int32_t selected_id);
int32_t extract_selected_id_from_chat_response(const std::string & response_body);

} // namespace loki_action

#endif

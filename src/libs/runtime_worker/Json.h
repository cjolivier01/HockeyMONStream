#pragma once

#include <json-glib/json-glib.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace hm::runtime_worker {
class Json {
 public:
  Json() : node_(json_node_new(JSON_NODE_OBJECT)) {
    json_node_take_object(node_, json_object_new());
  }
  explicit Json(JsonNode* node) : node_(json_node_copy(node)) {}
  Json(const Json& other) : Json(other.node_) {}
  Json(Json&& other) noexcept : node_(other.node_) {
    other.node_ = nullptr;
  }
  Json& operator=(Json other) noexcept {
    std::swap(node_, other.node_);
    return *this;
  }
  ~Json() {
    if (node_)
      json_node_unref(node_);
  }
  static Json Parse(const std::string& text) {
    if (text.find('\0') != std::string::npos || text.find("\\u0000") != std::string::npos)
      throw std::runtime_error("NUL is not permitted in protocol documents");
    JsonParser* parser = json_parser_new();
    GError* error = nullptr;
    if (!json_parser_load_from_data(parser, text.data(), text.size(), &error)) {
      std::string message = error ? error->message : "Invalid JSON";
      g_clear_error(&error);
      g_object_unref(parser);
      throw std::runtime_error(message);
    }
    Json result(json_parser_get_root(parser));
    g_object_unref(parser);
    return result;
  }
  static Json Array() {
    Json result;
    json_node_unref(result.node_);
    result.node_ = json_node_new(JSON_NODE_ARRAY);
    json_node_take_array(result.node_, json_array_new());
    return result;
  }
  static Json String(const std::string& value) {
    Json result;
    json_node_unref(result.node_);
    result.node_ = json_node_new(JSON_NODE_VALUE);
    json_node_set_string(result.node_, value.c_str());
    return result;
  }
  bool Has(const char* key) const {
    return json_object_has_member(Object(), key);
  }
  Json Get(const char* key) const {
    if (!Has(key))
      throw std::runtime_error(std::string("Missing field: ") + key);
    return Json(json_object_get_member(Object(), key));
  }
  std::string Text() const {
    if (!JSON_NODE_HOLDS_VALUE(node_) || json_node_get_value_type(node_) != G_TYPE_STRING)
      throw std::runtime_error("Expected string");
    return json_node_get_string(node_);
  }
  std::string Text(const char* key) const {
    return Get(key).Text();
  }
  int64_t Integer() const {
    if (!JSON_NODE_HOLDS_VALUE(node_) || json_node_get_value_type(node_) != G_TYPE_INT64)
      throw std::runtime_error("Expected integer");
    return json_node_get_int(node_);
  }
  int64_t Integer(const char* key, int64_t low, int64_t high) const {
    const auto value = Get(key).Integer();
    if (value < low || value > high)
      throw std::runtime_error(std::string("Out of range: ") + key);
    return value;
  }
  bool Boolean(const char* key) const {
    const auto value = Get(key);
    if (!JSON_NODE_HOLDS_VALUE(value.node_) || json_node_get_value_type(value.node_) != G_TYPE_BOOLEAN)
      throw std::runtime_error("Expected boolean");
    return json_node_get_boolean(value.node_);
  }
  std::vector<Json> Items() const {
    if (!JSON_NODE_HOLDS_ARRAY(node_))
      throw std::runtime_error("Expected array");
    std::vector<Json> result;
    auto* array = json_node_get_array(node_);
    for (guint i = 0; i < json_array_get_length(array); ++i)
      result.emplace_back(json_array_get_element(array, i));
    return result;
  }
  std::vector<std::string> Keys() const {
    std::vector<std::string> result;
    GList* keys = json_object_get_members(Object());
    for (auto* key = keys; key; key = key->next)
      result.emplace_back(static_cast<const char*>(key->data));
    g_list_free(keys);
    return result;
  }
  void Set(const char* key, const Json& value) {
    json_object_set_member(Object(), key, json_node_copy(value.node_));
  }
  void Set(const char* key, const std::string& value) {
    json_object_set_string_member(Object(), key, value.c_str());
  }
  void Set(const char* key, const char* value) {
    json_object_set_string_member(Object(), key, value);
  }
  void Set(const char* key, int64_t value) {
    json_object_set_int_member(Object(), key, value);
  }
  void Set(const char* key, bool value) {
    json_object_set_boolean_member(Object(), key, value);
  }
  void Null(const char* key) {
    json_object_set_null_member(Object(), key);
  }
  void Append(const Json& value) {
    json_array_add_element(json_node_get_array(node_), json_node_copy(value.node_));
  }
  std::string Dump() const {
    gchar* value = json_to_string(node_, FALSE);
    std::string result(value);
    g_free(value);
    return result;
  }

 private:
  JsonObject* Object() const {
    if (!JSON_NODE_HOLDS_OBJECT(node_))
      throw std::runtime_error("Expected object");
    return json_node_get_object(node_);
  }
  JsonNode* node_;
};
} // namespace hm::runtime_worker

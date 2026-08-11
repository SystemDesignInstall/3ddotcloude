#pragma once

// Shared test-only JSON-Schema conformance checker (ADR-016, tests only).
// Self-contained subset of JSON-Schema keywords used by the platform schemas
// (image.schema.json, feature.schema.json): required, properties, type, const,
// enum, pattern, minimum, and array items. `format` (uuid / date-time) is
// intentionally not enforced beyond the string `type` — structure, values and
// patterns are. Review debt #7 (image-import.md §16.8) requires the *produced*
// artifact documents to validate against the ratified schema, not just the
// schema files to be well-formed; feature.schema.json payloads carry the same
// requirement (RFC-0007 §2).

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

inline void CheckNode(const nlohmann::json& schema,
                      const nlohmann::json& doc, const std::string& path,
                      std::vector<std::string>* violations) {
  if (schema.contains("type")) {
    const std::string t = schema["type"];
    const bool ok = (t == "string" && doc.is_string()) ||
                    (t == "number" && doc.is_number()) ||
                    (t == "integer" && doc.is_number_integer()) ||
                    (t == "object" && doc.is_object()) ||
                    (t == "array" && doc.is_array());
    if (!ok) {
      violations->push_back(path + ": expected " + t + ", got " +
                            doc.type_name());
    }
  }
  if (schema.is_object() && schema.contains("required")) {
    for (const auto& key : schema["required"]) {
      if (!doc.is_object() || !doc.contains(key)) {
        violations->push_back(path + ": missing required '" +
                              key.get<std::string>() + "'");
      }
    }
  }
  if (schema.is_object() && schema.contains("properties")) {
    for (auto it = schema["properties"].begin();
         it != schema["properties"].end(); ++it) {
      const std::string key = it.key();
      const std::string child = path + "/" + key;
      if (doc.is_object() && doc.contains(key)) {
        CheckNode(it.value(), doc[key], child, violations);
      }
    }
  }
  if (schema.contains("const") && doc != schema["const"]) {
    violations->push_back(path + ": const mismatch");
  }
  if (schema.contains("enum")) {
    if (std::find(schema["enum"].begin(), schema["enum"].end(), doc) ==
        schema["enum"].end()) {
      violations->push_back(path + ": not in enum");
    }
  }
  if (schema.contains("pattern") && doc.is_string()) {
    const std::regex re(schema["pattern"].get<std::string>());
    if (!std::regex_match(doc.get<std::string>(), re)) {
      violations->push_back(path + ": pattern mismatch");
    }
  }
  if (schema.contains("minimum") && doc.is_number_integer()) {
    if (doc.get<std::int64_t>() < schema["minimum"].get<std::int64_t>()) {
      violations->push_back(path + ": below minimum");
    }
  }
  if (schema.contains("items") && doc.is_array()) {
    const auto& items = schema["items"];
    for (std::size_t i = 0; i < doc.size(); ++i) {
      CheckNode(items, doc[i], path + "/" + std::to_string(i), violations);
    }
  }
}

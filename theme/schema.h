#pragma once
// Проверка документа по JSON Schema (подмножество draft 2020-12, которого хватает
// для assets/theme-schema.json): type, enum, const, minimum, maximum, minItems,
// maxItems, maxLength, pattern, required, properties, additionalProperties, items,
// $ref на #/$defs/..., oneOf, allOf.

#include <string>
#include <vector>

#include "core/json.h"

namespace theme {

// true - документ подходит. errors (если задан) получает пути и причины ошибок.
bool ValidateSchema(const json::Value& schema, const json::Value& doc, std::vector<std::string>* errors = nullptr);

}  // namespace theme

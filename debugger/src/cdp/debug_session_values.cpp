#include "debug_session.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \r\n\t"), last = value.find_last_not_of(" \r\n\t");
    return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}
bool identifier_start(unsigned char c) { return std::isalpha(c) != 0 || c == '_' || c == '$'; }
bool identifier_part(unsigned char c) { return identifier_start(c) || std::isdigit(c) != 0; }
std::vector<std::string> data_path(const std::string& expression) {
    if (expression.empty() || expression.size() > 1024) throw std::runtime_error("Data path exceeds budget");
    std::vector<std::string> result;
    size_t pos = 0;
    auto identifier = [&] {
        const size_t first = pos;
        if (pos >= expression.size() || !identifier_start(static_cast<unsigned char>(expression[pos]))) throw std::runtime_error("Only static data paths are supported");
        while (pos < expression.size() && identifier_part(static_cast<unsigned char>(expression[pos]))) ++pos;
        return expression.substr(first, pos - first);
    };
    result.push_back(identifier());
    while (pos < expression.size()) {
        if (result.size() >= 32) throw std::runtime_error("Data path segment budget exceeded");
        if (expression[pos] == '.') { ++pos; result.push_back(identifier()); }
        else if (expression[pos] == '[') {
            const size_t first = ++pos;
            while (pos < expression.size() && std::isdigit(static_cast<unsigned char>(expression[pos]))) ++pos;
            if (pos == first || pos >= expression.size() || expression[pos] != ']') throw std::runtime_error("Only nonnegative literal array indices are supported");
            const auto index = expression.substr(first, pos - first);
            if (index.size() > 10 || (index.size() > 1 && index[0] == '0')) throw std::runtime_error("Invalid array index");
            result.push_back(index); ++pos;
        } else throw std::runtime_error("Only static data paths are supported");
    }
    return result;
}
void free_descriptor(JSContext* ctx, JSPropertyDescriptor& desc) {
    JS_FreeValue(ctx, desc.value); JS_FreeValue(ctx, desc.getter); JS_FreeValue(ctx, desc.setter);
}
}

bool DebugSession::parse_literal(const std::string& text, json::Value& argument) {
    const auto literal = trim(text);
    if (literal.empty() || literal.size() > 16384) return false;
    argument = json::Value::object();
    for (const char* special : {"undefined", "NaN", "Infinity", "-Infinity", "-0"}) {
        if (literal == special) { argument.set("unserializableValue", special); return true; }
    }
    // The bounded strict JSON parser rejects operators, comments and trailing code.
    try {
        auto value = json::Value::parse(literal);
        if (value.is_null() || value.is_bool() || value.is_string() || (value.is_number() && std::isfinite(value.get_number()))) {
            argument.set("value", value); return true;
        }
    } catch (...) {}
    return false;
}
bool DebugSession::primitive(JSContext* ctx, const json::Value& argument, JSValue& result) {
    if (!argument.is_object() || argument.has("objectId") || (argument.has("value") && argument.has("unserializableValue"))) return false;
    if (argument.has("unserializableValue")) {
        if (!argument["unserializableValue"].is_string()) return false;
        const auto& value = argument["unserializableValue"].get_string();
        if (value == "undefined") result = JS_UNDEFINED;
        else if (value == "NaN") result = JS_NewFloat64(ctx, std::numeric_limits<double>::quiet_NaN());
        else if (value == "Infinity") result = JS_NewFloat64(ctx, std::numeric_limits<double>::infinity());
        else if (value == "-Infinity") result = JS_NewFloat64(ctx, -std::numeric_limits<double>::infinity());
        else if (value == "-0") result = JS_NewFloat64(ctx, -0.0);
        else return false;
    } else if (!argument.has("value")) result = JS_UNDEFINED;
    else {
        const auto& value = argument["value"];
        if (value.is_null()) result = JS_NULL;
        else if (value.is_bool()) result = JS_NewBool(ctx, value.get_bool());
        else if (value.is_number() && std::isfinite(value.get_number())) result = JS_NewFloat64(ctx, value.get_number());
        else if (value.is_string() && value.get_string().size() <= 16384) result = JS_NewStringLen(ctx, value.get_string().data(), value.get_string().size());
        else return false;
    }
    if (JS_IsException(result)) { JS_FreeValue(ctx, JS_GetException(ctx)); return false; }
    return true;
}
bool DebugSession::ordinary(JSContext*, JSValueConst value) const {
    if (!JS_IsObject(value) || JS_IsProxy(value)) return false;
    const auto id = JS_GetClassID(value);
    return id != 0 && (id == object_class_ || id == array_class_);
}
json::Value DebugSession::remote(JSContext* ctx, JSValueConst value) {
    auto result = json::Value::object();
    if (JS_IsUndefined(value)) result.set("type", "undefined");
    else if (JS_IsNull(value)) result.set("type", "object").set("subtype", "null").set("value", nullptr);
    else if (JS_IsBool(value)) result.set("type", "boolean").set("value", JS_ToBool(ctx, value) != 0);
    else if (JS_IsNumber(value)) {
        double number = 0; JS_ToFloat64(ctx, &number, value); result.set("type", "number");
        if (std::isnan(number)) result.set("unserializableValue", "NaN");
        else if (std::isinf(number)) result.set("unserializableValue", number < 0 ? "-Infinity" : "Infinity");
        else if (number == 0 && std::signbit(number)) result.set("unserializableValue", "-0");
        else result.set("value", number);
    } else if (JS_IsString(value)) {
        size_t length = 0; const char* text = JS_ToCStringLen(ctx, &length, value);
        if (!text) { JS_FreeValue(ctx, JS_GetException(ctx)); throw std::runtime_error("String preview allocation failed"); }
        size_t bytes = std::min({length, size_t(16384), size_t(524288) - std::min(preview_bytes_, size_t(524288))});
        // Preserve complete UTF-8 codepoints at the truncation boundary.
        while (bytes < length && bytes > 0 && (static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80) --bytes;
        result.set("type", "string").set("value", std::string(text, bytes));
        if (bytes != length) result.set("description", "[truncated string]");
        preview_bytes_ += bytes; JS_FreeCString(ctx, text);
    } else if (JS_IsObject(value)) {
        result.set("type", "object");
        if (JS_IsProxy(value)) return result.set("description", "[Proxy: inspection unsupported]");
        if (JS_IsFunction(ctx, value)) return result.set("type", "function").set("description", "[Function]");
        if (!ordinary(ctx, value)) return result.set("description", "[Opaque/exotic object: inspection unsupported]");
        const bool array = JS_GetClassID(value) == array_class_;
        result.set("className", array ? "Array" : "Object").set("description", array ? "Array" : "Object");
        if (array) result.set("subtype", "array");
        const auto pointer = JS_VALUE_GET_PTR(value);
        auto existing = object_ids_.find(pointer);
        if (existing != object_ids_.end()) return result.set("objectId", existing->second);
        if (handle_count_ >= 2048) return result.set("description", "[Object handle budget exceeded]");
        const auto id = "object:" + std::to_string(generation_) + ":" + std::to_string(next_object_++);
        objects_[id].value = JS_DupValue(ctx, value); object_ids_[pointer] = id; ++handle_count_;
        result.set("objectId", id);
    } else result.set("type", "undefined").set("description", "[Unsupported value]");
    return result;
}
int DebugSession::find_frame(const std::string& id) const {
    for (size_t i = 0; i < frames_.size(); ++i) if (frames_[i].id == id) return static_cast<int>(i);
    return -1;
}
bool DebugSession::read_own(JSContext* ctx, JSValueConst object, const std::string& field, JSValue& result) {
    if (!ordinary(ctx, object) || field.size() > 1024) return false;
    const auto atom = JS_NewAtomLen(ctx, field.data(), field.size());
    if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, JS_GetException(ctx)); return false; }
    JSPropertyDescriptor desc{};
    const int found = JS_GetOwnProperty(ctx, &desc, object, atom); JS_FreeAtom(ctx, atom);
    if (found <= 0) { if (found < 0) JS_FreeValue(ctx, JS_GetException(ctx)); return false; }
    const bool data = (desc.flags & JS_PROP_TMASK) != JS_PROP_GETSET;
    if (data) result = JS_DupValue(ctx, desc.value);
    free_descriptor(ctx, desc); return data;
}
json::Value DebugSession::properties_vm(JSContext* ctx, const std::string& id) {
    auto properties = json::Value::array();
    for (const auto& scope : scopes_) if (scope.id == id) {
        for (const auto& binding : scope.bindings) {
            if (properties.size() >= 256) break;
            auto property = json::Value::object();
            property.set("name", binding.name).set("writable", binding.writable && binding.initialized)
                .set("configurable", false).set("enumerable", true).set("isOwn", true);
            if (binding.initialized) property.set("value", remote(ctx, binding.value));
            else { auto marker = json::Value::object(); marker.set("type", "undefined").set("description", "[Uninitialized]"); property.set("value", marker); }
            properties.push(property);
        }
        auto result = json::Value::object(); result.set("result", properties); return result;
    }
    const auto object = objects_.find(id);
    if (object == objects_.end()) return error("Stale or unsupported object handle");
    if (!ordinary(ctx, object->second.value)) return error("Object inspection is unsupported");
    JSPropertyEnum* names = nullptr; uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &names, &count, object->second.value, JS_GPN_STRING_MASK) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx)); return error("Cannot enumerate stored properties");
    }
    try {
        for (uint32_t i = 0; i < count && i < 256; ++i) {
            size_t key_size = 0;
            JSValue key_value = JS_AtomToValue(ctx, names[i].atom);
            const char* text = JS_ToCStringLen(ctx, &key_size, key_value);
            JS_FreeValue(ctx, key_value);
            if (!text) throw std::runtime_error("Property name allocation failed");
            std::string key;
            if (key_size <= 1024) key.assign(text, key_size);
            JS_FreeCString(ctx, text);
            if (key_size > 1024) continue;
            JSPropertyDescriptor desc{};
            if (JS_GetOwnProperty(ctx, &desc, object->second.value, names[i].atom) <= 0) continue;
            try {
                auto property = json::Value::object();
                property.set("name", key).set("configurable", (desc.flags & JS_PROP_CONFIGURABLE) != 0)
                    .set("enumerable", (desc.flags & JS_PROP_ENUMERABLE) != 0).set("isOwn", true);
                if ((desc.flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                    auto marker = json::Value::object(); marker.set("type", "function").set("description", "[Accessor: not invoked]");
                    if (!JS_IsUndefined(desc.getter)) property.set("get", marker);
                    if (!JS_IsUndefined(desc.setter)) property.set("set", marker);
                } else property.set("value", remote(ctx, desc.value)).set("writable", (desc.flags & JS_PROP_WRITABLE) != 0);
                properties.push(property);
            } catch (...) { free_descriptor(ctx, desc); throw; }
            free_descriptor(ctx, desc);
        }
    } catch (...) { JS_FreePropertyEnum(ctx, names, count); throw; }
    JS_FreePropertyEnum(ctx, names, count);
    if (count > 256) {
        auto marker = json::Value::object(), value = json::Value::object();
        value.set("type", "string").set("value", "[Property budget: first 256 only]");
        marker.set("name", "[truncated]").set("value", value); properties.push(marker);
    }
    auto result = json::Value::object(); result.set("result", properties); return result;
}
json::Value DebugSession::get_properties(const std::string& id) {
    if (id.size() > 128) return error("Invalid object handle");
    return dispatch([this, id](JSContext* ctx) { return properties_vm(ctx, id); });
}
json::Value DebugSession::evaluate_vm(JSContext* ctx, const std::string& frame_id, const std::string& expression) {
    const auto frame = find_frame(frame_id);
    if (frame < 0) return error("Stale frame handle");
    json::Value argument; JSValue current = JS_UNDEFINED;
    if (parse_literal(expression, argument)) {
        if (!primitive(ctx, argument, current)) return error("Unsupported literal");
    } else {
        const auto path = data_path(trim(expression));
        const DebugBinding* found = nullptr;
        for (size_t index : frames_[frame].scopes) {
            for (const auto& binding : scopes_[index].bindings) if (binding.name == path.front()) {
                // The VM snapshot exposes lexical identities, not active-scope PC ranges.
                // Never guess which same-name lexical binding a textual path means.
                if (found) return error("Ambiguous lexical binding; inspect/edit the explicit scope instead");
                found = &binding;
            }
        }
        if (!found || !found->initialized) return error("Unknown or uninitialized binding");
        current = JS_DupValue(ctx, found->value);
        for (size_t i = 1; i < path.size(); ++i) {
            JSValue next = JS_UNDEFINED;
            if (!read_own(ctx, current, path[i], next)) {
                JS_FreeValue(ctx, current); return error("Only ordinary own data fields may be read; accessor/Proxy/exotic rejected");
            }
            JS_FreeValue(ctx, current); current = next;
        }
    }
    try { auto result = json::Value::object(); result.set("result", remote(ctx, current)); JS_FreeValue(ctx, current); return result; }
    catch (...) { JS_FreeValue(ctx, current); throw; }
}
json::Value DebugSession::evaluate_on_call_frame(const std::string& frame, const std::string& expression) {
    if (frame.size() > 128 || expression.size() > 16384) return error("Inspection input budget exceeded");
    return dispatch([this, frame, expression](JSContext* ctx) { return evaluate_vm(ctx, frame, expression); });
}
json::Value DebugSession::evaluate_literal(const std::string& expression) {
    json::Value argument;
    if (!parse_literal(expression, argument)) return error("Only primitive literals are supported; code execution is disabled");
    // No VM is needed to describe a literal: this is safe even outside a pause.
    auto value = json::Value::object(), result = json::Value::object();
    if (argument.has("unserializableValue")) {
        const auto& special = argument["unserializableValue"].get_string();
        value.set("type", special == "undefined" ? "undefined" : "number");
        if (special != "undefined") value.set("unserializableValue", special);
    } else {
        const auto& raw = argument["value"];
        value.set("type", raw.is_null() ? "object" : raw.is_bool() ? "boolean" : raw.is_string() ? "string" : "number").set("value", raw);
        if (raw.is_null()) value.set("subtype", "null");
    }
    result.set("result", value); return result;
}
json::Value DebugSession::set_variable_value(int scope_index, const std::string& name,
        const json::Value& argument, const std::string& frame_id) {
    if (name.size() > 1024 || frame_id.size() > 128) return error("Binding request budget exceeded");
    return dispatch([this, scope_index, name, argument, frame_id](JSContext* ctx) {
        const int frame = find_frame(frame_id);
        if (frame < 0 || scope_index < 0 || static_cast<size_t>(scope_index) >= frames_[frame].scopes.size()) return error("Stale frame or scope");
        auto& scope = scopes_[frames_[frame].scopes[scope_index]];
        DebugBinding* target = nullptr;
        for (auto& binding : scope.bindings) if (binding.name == name) {
            if (target) return error("Ambiguous binding"); target = &binding;
        }
        if (!target || !target->initialized || !target->writable) return error("Binding is absent, uninitialized or readonly");
        JSValue next = JS_UNDEFINED;
        if (!primitive(ctx, argument, next)) return error("Only typed primitive replacement values are supported");
        const int status = JS_SetVariableAtLevelByIndex(ctx, scope.frame, target->slot, next);
        if (status != 0) { JS_FreeValue(ctx, next); return error("VM refused binding write"); }
        JS_FreeValue(ctx, target->value); target->value = next;
        refresh_bindings(ctx);
        return json::Value::object();
    });
}
void DebugSession::refresh_bindings(JSContext* ctx) {
    for (const auto& frame : frames_) {
        if (frame.scopes.empty()) continue;
        const int level = scopes_[frame.scopes.front()].frame;
        JSDebugLocalVar* variables = nullptr; int count = 0;
        if (JS_GetLocalVariablesAtLevel(ctx, level, &variables, &count) < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx)); continue;
        }
        for (size_t index : frame.scopes) for (auto& binding : scopes_[index].bindings) {
            for (int i = 0; i < count; ++i) if (variables[i].binding_index == binding.slot) {
                JS_FreeValue(ctx, binding.value); binding.value = JS_DupValue(ctx, variables[i].value);
                binding.initialized = variables[i].is_initialized; binding.writable = variables[i].is_writable; break;
            }
        }
        if (variables) JS_FreeLocalVariables(ctx, variables, count);
    }
}
json::Value DebugSession::set_object_value(const std::string& id, const std::string& field, const json::Value& argument) {
    if (id.size() > 128 || field.size() > 1024) return error("Data field request budget exceeded");
    return dispatch([this, id, field, argument](JSContext* ctx) {
        auto target = objects_.find(id);
        if (target == objects_.end()) return error("Stale object handle");
        const JSValue object = target->second.value;
        if (!ordinary(ctx, object) || (JS_GetClassID(object) == array_class_ && field == "length")) return error("Unsupported object or array length write");
        JSValue next = JS_UNDEFINED;
        if (!primitive(ctx, argument, next)) return error("Only typed primitive replacement values are supported");
        const auto atom = JS_NewAtomLen(ctx, field.data(), field.size());
        if (atom == JS_ATOM_NULL) { JS_FreeValue(ctx, next); JS_FreeValue(ctx, JS_GetException(ctx)); return error("Field allocation failed"); }
        JSPropertyDescriptor desc{};
        const int found = JS_GetOwnProperty(ctx, &desc, object, atom);
        if (found <= 0) {
            JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, next);
            if (found < 0) JS_FreeValue(ctx, JS_GetException(ctx));
            return error("Only existing own fields may be modified");
        }
        const bool writable = (desc.flags & JS_PROP_TMASK) != JS_PROP_GETSET && (desc.flags & JS_PROP_WRITABLE) != 0;
        free_descriptor(ctx, desc);
        if (!writable) { JS_FreeAtom(ctx, atom); JS_FreeValue(ctx, next); return error("Accessor or readonly field write refused"); }
        const int status = JS_DefineProperty(ctx, object, atom, next, JS_UNDEFINED, JS_UNDEFINED, JS_PROP_HAS_VALUE);
        JS_FreeAtom(ctx, atom);
        if (status <= 0) { JS_FreeValue(ctx, next); JS_FreeValue(ctx, JS_GetException(ctx)); return error("VM refused data field write"); }
        auto result = json::Value::object(); result.set("result", remote(ctx, next)); JS_FreeValue(ctx, next); return result;
    });
}

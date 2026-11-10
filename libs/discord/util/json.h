#pragma once

// Small read helpers over yyjson. Discord sends snowflakes as strings and a few
// numbers (permissions) as strings too, so the getters accept either.

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "third_party/yyjson.h"

namespace J {

using Doc = std::shared_ptr<yyjson_doc>;

inline Doc Parse(const char* data, size_t len) {
    yyjson_doc* d = yyjson_read(data, len, 0);
    if (!d) return nullptr;
    return Doc(d, yyjson_doc_free);
}
inline Doc Parse(const std::string& s) { return Parse(s.data(), s.size()); }
inline yyjson_val* Root(const Doc& d) { return d ? yyjson_doc_get_root(d.get()) : nullptr; }

inline yyjson_val* Get(yyjson_val* o, const char* k) { return yyjson_is_obj(o) ? yyjson_obj_get(o, k) : nullptr; }

inline std::string Str(yyjson_val* v, const char* def = "") {
    return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(def);
}
inline std::string Str(yyjson_val* o, const char* k, const char* def) { return Str(Get(o, k), def); }
inline std::string S(yyjson_val* o, const char* k) { return Str(Get(o, k), ""); }

inline int64_t Int(yyjson_val* v, int64_t def = 0) {
    if (yyjson_is_sint(v)) return yyjson_get_sint(v);
    if (yyjson_is_uint(v)) return (int64_t)yyjson_get_uint(v);
    if (yyjson_is_real(v)) return (int64_t)yyjson_get_real(v);
    if (yyjson_is_str(v)) return strtoll(yyjson_get_str(v), nullptr, 10);
    if (yyjson_is_bool(v)) return yyjson_get_bool(v) ? 1 : 0;
    return def;
}
inline int64_t I(yyjson_val* o, const char* k, int64_t def = 0) { return Int(Get(o, k), def); }

inline uint64_t U64(yyjson_val* v) {
    if (yyjson_is_str(v)) return strtoull(yyjson_get_str(v), nullptr, 10);
    if (yyjson_is_uint(v)) return yyjson_get_uint(v);
    if (yyjson_is_sint(v)) return (uint64_t)yyjson_get_sint(v);
    if (yyjson_is_real(v)) return (uint64_t)yyjson_get_real(v);
    return 0;
}
inline uint64_t Id(yyjson_val* o, const char* k) { return U64(Get(o, k)); }

inline bool Bool(yyjson_val* o, const char* k, bool def = false) {
    yyjson_val* v = Get(o, k);
    return yyjson_is_bool(v) ? yyjson_get_bool(v) : def;
}

inline bool IsNull(yyjson_val* o, const char* k) {
    yyjson_val* v = Get(o, k);
    return !v || yyjson_is_null(v);
}

// for (auto* v : J::Arr(x)) ...
struct ArrRange {
    yyjson_val* arr;
    struct It {
        yyjson_arr_iter it;
        yyjson_val* cur;
        bool operator!=(const It& o) const { return cur != o.cur; }
        void operator++() { cur = yyjson_arr_iter_next(&it); }
        yyjson_val* operator*() const { return cur; }
    };
    It begin() const {
        It i{};
        if (yyjson_is_arr(arr)) {
            yyjson_arr_iter_init(arr, &i.it);
            i.cur = yyjson_arr_iter_next(&i.it);
        } else {
            i.cur = nullptr;
        }
        return i;
    }
    It end() const { return It{{}, nullptr}; }
};
inline ArrRange Arr(yyjson_val* v) { return ArrRange{v}; }
inline ArrRange Arr(yyjson_val* o, const char* k) { return ArrRange{Get(o, k)}; }
inline size_t Len(yyjson_val* v) { return yyjson_is_arr(v) ? yyjson_arr_size(v) : 0; }

}  // namespace J

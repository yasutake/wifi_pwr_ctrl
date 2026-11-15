#include "common.h"
#include <limits.h>
#include <stdio.h>

json_object *field(json_object *obj, const char *key, enum json_type type)
{
    json_object *value;
    if (!obj || !json_object_is_type(obj, json_type_object) ||
        !json_object_object_get_ex(obj, key, &value) ||
        !json_object_is_type(value, type)) {
        fprintf(stderr, "Invalid device response: missing or invalid %s\n", key);
        return NULL;
    }
    return value;
}

int device_ok(json_object *obj, const char *context, const char *key)
{
    json_object *code = field(obj, key, json_type_int);
    if (!code) return 0;
    if (json_object_get_int64(code) != 0) {
        fprintf(stderr, "%s failed (device error %lld)\n", context,
                (long long)json_object_get_int64(code));
        return 0;
    }
    return 1;
}

json_object *parse_json(const unsigned char *data, size_t len)
{
    json_tokener *tok = json_tokener_new();
    json_object *obj = NULL;
    if (tok && len <= INT_MAX) {
        obj = json_tokener_parse_ex(tok, (const char *)data, (int)len);
        if (json_tokener_get_error(tok) != json_tokener_success ||
            json_tokener_get_parse_end(tok) != len) {
            json_object_put(obj);
            obj = NULL;
        }
    }
    json_tokener_free(tok);
    if (!obj) fprintf(stderr, "Invalid JSON response from device\n");
    return obj;
}

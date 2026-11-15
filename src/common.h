#ifndef COMMON_H
#define COMMON_H

#include <json-c/json.h>
#include <stddef.h>
#include <stdint.h>

struct klap {
    char base[128];
    char cookie[256];
    char terminal_uuid[25];
    unsigned char key[16], iv[12], sig[28];
    uint32_t seq;
    void *curl;
};

int klap_open(struct klap *s, const char *ip, int port,
              const char *username, const char *password);
void klap_close(struct klap *s);
json_object *klap_query(struct klap *s, const char *method, json_object *params);
json_object *legacy_query(const char *ip, int port, const char *request);
json_object *field(json_object *obj, const char *key, enum json_type type);
int device_ok(json_object *obj, const char *context, const char *key);
json_object *parse_json(const unsigned char *data, size_t len);

#endif

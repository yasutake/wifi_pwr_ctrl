#include "common.h"
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "Usage: wifi-pwr-ctrl --model P110M|P105|P300|HS105 --ip IPv4\n"
                 "       [--port PORT] [--child DEVICE_ID] status|on|off|energy\n"
                 "Tapo credentials: TAPO_USERNAME and TAPO_PASSWORD environment variables.\n"
                 "P300 on/off requires --child; status lists outlet IDs.\n");
}

static json_object *smart_result(struct klap *s, const char *method, json_object *params)
{
    json_object *reply = klap_query(s, method, params), *result;
    if (!reply) return NULL;
    result = field(reply, "result", json_type_object);
    if (result) json_object_get(result);
    json_object_put(reply);
    return result;
}

static int smart_set(struct klap *s, const char *child, int on)
{
    json_object *params = json_object_new_object(), *reply, *result, *nested;
    int ok = 0;
    json_object_object_add(params, "device_on", json_object_new_boolean(on));
    if (child) {
        json_object *inner = json_object_new_object(), *wrapper = json_object_new_object();
        json_object_object_add(inner, "method", json_object_new_string("set_device_info"));
        json_object_object_add(inner, "params", params);
        json_object_object_add(wrapper, "device_id", json_object_new_string(child));
        json_object_object_add(wrapper, "requestData", inner);
        reply = klap_query(s, "control_child", wrapper);
        json_object_put(wrapper);
        if (!reply) return 0;
        result = field(reply, "result", json_type_object);
        nested = field(result, "responseData", json_type_object);
        ok = nested && device_ok(nested, "control_child", "error_code");
    } else {
        reply = klap_query(s, "set_device_info", params);
        json_object_put(params);
        if (!reply) return 0;
        ok = 1;
    }
    json_object_put(reply);
    return ok;
}

static json_object *strip_status(struct klap *s, const char *child)
{
    json_object *out = json_object_new_array();
    int index = 0;
    for (;;) {
        json_object *params = json_object_new_object();
        json_object_object_add(params, "start_index", json_object_new_int(index));
        json_object *page = smart_result(s, "get_child_device_list", params);
        json_object_put(params);
        if (!page) goto fail;
        json_object *list = field(page, "child_device_list", json_type_array);
        json_object *sum = field(page, "sum", json_type_int);
        json_object *start = field(page, "start_index", json_type_int);
        if (!list || !sum || !start || json_object_get_int(start) != index ||
            json_object_get_int(sum) < index) {
            fprintf(stderr, "Invalid P300 child list pagination\n");
            json_object_put(page); goto fail;
        }
        size_t count = json_object_array_length(list);
        if (count > (size_t)(json_object_get_int(sum) - index) ||
            (!count && index < json_object_get_int(sum))) {
            fprintf(stderr, "Incomplete P300 child list\n");
            json_object_put(page); goto fail;
        }
        for (size_t i = 0; i < count; i++) {
            json_object *entry = json_object_array_get_idx(list, i);
            json_object *id = field(entry, "device_id", json_type_string);
            json_object *on = field(entry, "device_on", json_type_boolean);
            if (!id || !on) { json_object_put(page); goto fail; }
            if (!child || !strcmp(child, json_object_get_string(id))) {
                json_object *item = json_object_new_object(), *position;
                json_object_object_add(item, "device_id", json_object_get(id));
                json_object_object_add(item, "device_on", json_object_get(on));
                if (json_object_object_get_ex(entry, "position", &position) &&
                    json_object_is_type(position, json_type_int))
                    json_object_object_add(item, "position", json_object_get(position));
                json_object_array_add(out, item);
            }
        }
        index += (int)count;
        int done = index == json_object_get_int(sum);
        json_object_put(page);
        if (done) break;
    }
    if (child && !json_object_array_length(out)) {
        fprintf(stderr, "P300 child ID not found\n"); goto fail;
    }
    return out;
fail:
    json_object_put(out);
    return NULL;
}

static int kasa(const char *ip, int port, const char *command)
{
    int set = strcmp(command, "status") != 0;
    const char *request = !set ? "{\"system\":{\"get_sysinfo\":{}}}" :
        !strcmp(command, "on") ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}" :
                                 "{\"system\":{\"set_relay_state\":{\"state\":0}}}";
    json_object *reply = legacy_query(ip, port, request), *system, *result;
    int ok = 0;
    if (!reply) return 0;
    system = field(reply, "system", json_type_object);
    result = field(system, set ? "set_relay_state" : "get_sysinfo", json_type_object);
    if (!result || !device_ok(result, command, "err_code")) goto done;
    if (!set) {
        json_object *state = field(result, "relay_state", json_type_int);
        if (!state || (json_object_get_int(state) != 0 && json_object_get_int(state) != 1)) goto done;
        printf("{\"device_on\":%s}\n", json_object_get_int(state) ? "true" : "false");
    } else puts("{\"ok\":true}");
    ok = 1;
done:
    json_object_put(reply);
    return ok;
}

int main(int argc, char **argv)
{
    const char *model = NULL, *ip = NULL, *child = NULL, *command;
    int port = 0, opt;
    struct in_addr address;
    static const struct option options[] = {
        {"model", required_argument, NULL, 'm'}, {"ip", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'}, {"child", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    while ((opt = getopt_long(argc, argv, "m:i:p:c:h", options, NULL)) != -1) {
        switch (opt) {
        case 'm': model = optarg; break;
        case 'i': ip = optarg; break;
        case 'c': child = optarg; break;
        case 'p': {
            char *end;
            errno = 0;
            long value = strtol(optarg, &end, 10);
            if (errno || !*optarg || *end || value < 1 || value > 65535) {
                fprintf(stderr, "Invalid port\n"); return 2;
            }
            port = (int)value; break;
        }
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (!model || !ip || inet_pton(AF_INET, ip, &address) != 1 || optind + 1 != argc) {
        usage(stderr); return 2;
    }
    command = argv[optind];
    int hs = !strcmp(model, "HS105"), strip = !strcmp(model, "P300");
    int energy = !strcmp(command, "energy");
    if ((!hs && !strip && strcmp(model, "P105") && strcmp(model, "P110M")) ||
        (strcmp(command, "status") && strcmp(command, "on") &&
         strcmp(command, "off") && !energy) ||
        (child && (!strip || !*child)) ||
        (strip && !child && (!strcmp(command, "on") || !strcmp(command, "off")))) {
        usage(stderr); return 2;
    }
    if (energy && strcmp(model, "P110M")) {
        fprintf(stderr, "Energy monitoring is not available on %s\n", model); return 2;
    }
    if (hs) return kasa(ip, port ? port : 9999, command) ? 0 : 1;
    const char *username = getenv("TAPO_USERNAME"), *password = getenv("TAPO_PASSWORD");
    if (!username || !password) {
        fprintf(stderr, "Set TAPO_USERNAME and TAPO_PASSWORD\n"); return 2;
    }
    struct klap session;
    if (!klap_open(&session, ip, port ? port : 80, username, password)) return 1;
    int ok = 0;
    if (!strcmp(command, "on") || !strcmp(command, "off")) {
        ok = smart_set(&session, child, !strcmp(command, "on"));
        if (ok) puts("{\"ok\":true}");
    } else {
        json_object *result = strip ? strip_status(&session, child) :
            smart_result(&session, energy ? "get_energy_usage" : "get_device_info", NULL);
        if (result) {
            if (strip) {
                printf("%s\n", json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN));
                ok = 1;
            } else if (energy) {
                json_object *power = field(result, "current_power", json_type_int);
                json_object *today = field(result, "today_energy", json_type_int);
                json_object *month = field(result, "month_energy", json_type_int);
                if (power && today && month) {
                    printf("{\"power_w\":%.3f,\"today_kwh\":%.3f,\"month_kwh\":%.3f}\n",
                           json_object_get_double(power) / 1000, json_object_get_double(today) / 1000,
                           json_object_get_double(month) / 1000);
                    ok = 1;
                }
            } else {
                json_object *on = field(result, "device_on", json_type_boolean);
                if (on) { printf("{\"device_on\":%s}\n", json_object_get_boolean(on) ? "true" : "false"); ok = 1; }
            }
            json_object_put(result);
        }
    }
    klap_close(&session);
    return ok ? 0 : 1;
}

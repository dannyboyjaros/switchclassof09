#ifndef JSON_LITE_H
#define JSON_LITE_H

#include <stddef.h>

typedef enum {
    JL_NULL,
    JL_BOOL,
    JL_NUMBER,
    JL_STRING,
    JL_ARRAY,
    JL_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

struct JsonValue {
    JsonType type;
    union {
        int boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            size_t count;
        } array;
        struct {
            char **keys;
            JsonValue **values;
            size_t count;
        } object;
    } u;
};

JsonValue *json_parse(const char *text);
void json_free(JsonValue *value);
JsonValue *json_object_get(JsonValue *object, const char *key);
const char *json_as_string(JsonValue *value);
double json_as_number(JsonValue *value);
int json_as_bool(JsonValue *value);

#endif

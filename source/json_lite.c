#include "json_lite.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
} Parser;

static JsonValue *parse_value(Parser *parser);
static void skip_ws(Parser *parser) {
    while (*parser->p && isspace((unsigned char)*parser->p)) {
        parser->p++;
    }
}

static JsonValue *make_value(JsonType type) {
    JsonValue *value = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (value) {
        value->type = type;
    }
    return value;
}

static char *parse_string_raw(Parser *parser) {
    if (*parser->p != '"') {
        return NULL;
    }
    parser->p++;
    size_t cap = 32;
    size_t len = 0;
    char *out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }
    while (*parser->p && *parser->p != '"') {
        char ch = *parser->p++;
        if (ch == '\\' && *parser->p) {
            char esc = *parser->p++;
            switch (esc) {
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case '"':
            case '\\':
            case '/':
                ch = esc;
                break;
            case 'u': {
                unsigned code = 0;
                for (int i = 0; i < 4 && isxdigit((unsigned char)*parser->p); i++) {
                    char hex = *parser->p++;
                    code <<= 4;
                    if (hex >= '0' && hex <= '9') {
                        code += (unsigned)(hex - '0');
                    } else if (hex >= 'a' && hex <= 'f') {
                        code += (unsigned)(hex - 'a' + 10);
                    } else {
                        code += (unsigned)(hex - 'A' + 10);
                    }
                }
                if (len + 4 >= cap) {
                    cap *= 2;
                    char *grown = (char *)realloc(out, cap);
                    if (!grown) {
                        free(out);
                        return NULL;
                    }
                    out = grown;
                }
                if (code < 0x80) {
                    out[len++] = (char)code;
                } else if (code < 0x800) {
                    out[len++] = (char)(0xC0 | (code >> 6));
                    out[len++] = (char)(0x80 | (code & 0x3F));
                } else {
                    out[len++] = (char)(0xE0 | (code >> 12));
                    out[len++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    out[len++] = (char)(0x80 | (code & 0x3F));
                }
                continue;
            }
            default:
                ch = esc;
                break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *grown = (char *)realloc(out, cap);
            if (!grown) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        out[len++] = ch;
    }
    if (*parser->p == '"') {
        parser->p++;
    }
    out[len] = 0;
    return out;
}

static JsonValue *parse_string(Parser *parser) {
    JsonValue *value = make_value(JL_STRING);
    if (!value) {
        return NULL;
    }
    value->u.string = parse_string_raw(parser);
    return value;
}

static JsonValue *parse_number(Parser *parser) {
    JsonValue *value = make_value(JL_NUMBER);
    if (!value) {
        return NULL;
    }
    char *end = NULL;
    value->u.number = strtod(parser->p, &end);
    parser->p = end;
    return value;
}

static JsonValue *parse_array(Parser *parser) {
    JsonValue *value = make_value(JL_ARRAY);
    if (!value) {
        return NULL;
    }
    parser->p++;
    skip_ws(parser);
    size_t cap = 8;
    value->u.array.items = (JsonValue **)calloc(cap, sizeof(JsonValue *));
    if (*parser->p == ']') {
        parser->p++;
        return value;
    }
    while (*parser->p) {
        skip_ws(parser);
        if (value->u.array.count == cap) {
            cap *= 2;
            value->u.array.items = (JsonValue **)realloc(value->u.array.items, cap * sizeof(JsonValue *));
        }
        value->u.array.items[value->u.array.count++] = parse_value(parser);
        skip_ws(parser);
        if (*parser->p == ',') {
            parser->p++;
            continue;
        }
        if (*parser->p == ']') {
            parser->p++;
            break;
        }
        break;
    }
    return value;
}

static JsonValue *parse_object(Parser *parser) {
    JsonValue *value = make_value(JL_OBJECT);
    if (!value) {
        return NULL;
    }
    parser->p++;
    skip_ws(parser);
    size_t cap = 8;
    value->u.object.keys = (char **)calloc(cap, sizeof(char *));
    value->u.object.values = (JsonValue **)calloc(cap, sizeof(JsonValue *));
    if (*parser->p == '}') {
        parser->p++;
        return value;
    }
    while (*parser->p) {
        skip_ws(parser);
        if (value->u.object.count == cap) {
            cap *= 2;
            value->u.object.keys = (char **)realloc(value->u.object.keys, cap * sizeof(char *));
            value->u.object.values = (JsonValue **)realloc(value->u.object.values, cap * sizeof(JsonValue *));
        }
        value->u.object.keys[value->u.object.count] = parse_string_raw(parser);
        skip_ws(parser);
        if (*parser->p == ':') {
            parser->p++;
        }
        skip_ws(parser);
        value->u.object.values[value->u.object.count++] = parse_value(parser);
        skip_ws(parser);
        if (*parser->p == ',') {
            parser->p++;
            continue;
        }
        if (*parser->p == '}') {
            parser->p++;
            break;
        }
        break;
    }
    return value;
}

static JsonValue *parse_value(Parser *parser) {
    skip_ws(parser);
    if (*parser->p == '"') {
        return parse_string(parser);
    }
    if (*parser->p == '{') {
        return parse_object(parser);
    }
    if (*parser->p == '[') {
        return parse_array(parser);
    }
    if (!strncmp(parser->p, "null", 4)) {
        parser->p += 4;
        return make_value(JL_NULL);
    }
    if (!strncmp(parser->p, "true", 4)) {
        parser->p += 4;
        JsonValue *value = make_value(JL_BOOL);
        value->u.boolean = 1;
        return value;
    }
    if (!strncmp(parser->p, "false", 5)) {
        parser->p += 5;
        JsonValue *value = make_value(JL_BOOL);
        value->u.boolean = 0;
        return value;
    }
    return parse_number(parser);
}

JsonValue *json_parse(const char *text) {
    Parser parser;
    parser.p = text;
    return parse_value(&parser);
}

void json_free(JsonValue *value) {
    if (!value) {
        return;
    }
    if (value->type == JL_STRING) {
        free(value->u.string);
    } else if (value->type == JL_ARRAY) {
        for (size_t i = 0; i < value->u.array.count; i++) {
            json_free(value->u.array.items[i]);
        }
        free(value->u.array.items);
    } else if (value->type == JL_OBJECT) {
        for (size_t i = 0; i < value->u.object.count; i++) {
            free(value->u.object.keys[i]);
            json_free(value->u.object.values[i]);
        }
        free(value->u.object.keys);
        free(value->u.object.values);
    }
    free(value);
}

JsonValue *json_object_get(JsonValue *object, const char *key) {
    if (!object || object->type != JL_OBJECT) {
        return NULL;
    }
    for (size_t i = 0; i < object->u.object.count; i++) {
        if (object->u.object.keys[i] && strcmp(object->u.object.keys[i], key) == 0) {
            return object->u.object.values[i];
        }
    }
    return NULL;
}

const char *json_as_string(JsonValue *value) {
    if (!value || value->type != JL_STRING || !value->u.string) {
        return "";
    }
    return value->u.string;
}

double json_as_number(JsonValue *value) {
    if (!value) {
        return 0;
    }
    if (value->type == JL_NUMBER) {
        return value->u.number;
    }
    if (value->type == JL_STRING) {
        return strtod(value->u.string ? value->u.string : "0", NULL);
    }
    return 0;
}

int json_as_bool(JsonValue *value) {
    if (!value) {
        return 0;
    }
    if (value->type == JL_BOOL) {
        return value->u.boolean;
    }
    if (value->type == JL_NUMBER) {
        return value->u.number != 0;
    }
    return 0;
}

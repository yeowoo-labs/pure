#ifndef PURE_MESSAGE_JSON_H
#define PURE_MESSAGE_JSON_H
#include "module.h"

/* A message's fields from a JSON object read in place (docs/research/082-json-in-place.md):
 * a u32 or a number for a fixed field (put into the prefix), a string for
 * bytes or text (left where it is: an escaped one is unescaped over itself,
 * which never makes it longer), a group's fields as an object inside. */
typedef struct {
    const PureModule *module;
    const PureInputShape *shape;
    unsigned char *payload; /* the prefix; a string's start is counted from here */
    unsigned char *table;   /* for each field, its start and size (u32le each) */
    const bool *given;      /* fields given elsewhere (a request's query), or NULL */
    bool lenient;           /* a member naming no field is skipped, value and all (an API's answer) */
    bool seen[PURE_MODULE_FIELDS];
    /* A rows field's array of objects is encoded in the stored form of its
     * list into rows (grown with malloc: the caller frees it), which the
     * caller places from rows_base on in the payload, as the table says. */
    unsigned char *rows;
    size_t rows_size, rows_capacity, rows_base;
} PureJsonMessage;

/* The object at json[*at] (its opening brace), *at moved past it: NULL, or
 * what is wrong ("json", "json_field": unknown or given twice, "json_type",
 * "limit": rows past what a payload holds, "memory"). */
const char *pure_json_message(PureJsonMessage *, unsigned char *json, size_t size, size_t *at);
size_t pure_json_space(const unsigned char *json, size_t at, size_t size);
/* A fixed field's value from its text (a query value or a JSON number) into
 * the prefix: a u32 in range, or a finite number. */
bool pure_json_fixed(const PureField *field, const unsigned char *text, size_t size, unsigned char *prefix);
#endif

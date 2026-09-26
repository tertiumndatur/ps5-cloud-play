// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUD_JSON_H
#define CHIAKI_CLOUD_JSON_H

#include <json-c/json_object.h>
#include <stdbool.h>

const char *cloud_json_string(struct json_object *object, const char *key);
int cloud_json_integer(struct json_object *object, const char *key);
bool cloud_json_boolean(struct json_object *object, const char *key);
struct json_object *cloud_json_object(struct json_object *object, const char *key);
struct json_object *cloud_json_array(struct json_object *object, const char *key);
struct json_object *cloud_json_copy(struct json_object *value);
void cloud_json_put_string(struct json_object *object, const char *key, const char *value);
void cloud_json_put_boolean(struct json_object *object, const char *key, bool value);
bool cloud_string_equal_ci(const char *left, const char *right);
bool cloud_string_ends(const char *value, const char *suffix);

#endif

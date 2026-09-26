// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloud_json.h"

#include <json-c/json_tokener.h>
#include <string.h>
#include <strings.h>

static struct json_object *cloud_json_member(struct json_object *object, const char *key)
{
	struct json_object *value = NULL;
	if(!object || !key || json_object_get_type(object) != json_type_object
		|| !json_object_object_get_ex(object, key, &value) || !value)
		return NULL;
	return value;
}

const char *cloud_json_string(struct json_object *object, const char *key)
{
	struct json_object *value = cloud_json_member(object, key);
	return value && json_object_get_type(value) == json_type_string ? json_object_get_string(value) : "";
}

int cloud_json_integer(struct json_object *object, const char *key)
{
	struct json_object *value = cloud_json_member(object, key);
	return value ? json_object_get_int(value) : 0;
}

bool cloud_json_boolean(struct json_object *object, const char *key)
{
	struct json_object *value = cloud_json_member(object, key);
	return value ? json_object_get_boolean(value) != 0 : false;
}

struct json_object *cloud_json_object(struct json_object *object, const char *key)
{
	struct json_object *value = cloud_json_member(object, key);
	return value && json_object_get_type(value) == json_type_object ? value : NULL;
}

struct json_object *cloud_json_array(struct json_object *object, const char *key)
{
	struct json_object *value = cloud_json_member(object, key);
	return value && json_object_get_type(value) == json_type_array ? value : NULL;
}

struct json_object *cloud_json_copy(struct json_object *value)
{
	if(!value)
		return NULL;
	struct json_object *copy = NULL;
	return json_object_deep_copy(value, &copy, NULL) == 0 ? copy : NULL;
}

void cloud_json_put_string(struct json_object *object, const char *key, const char *value)
{
	if(object && key)
		json_object_object_add(object, key, json_object_new_string(value ? value : ""));
}

void cloud_json_put_boolean(struct json_object *object, const char *key, bool value)
{
	if(object && key)
		json_object_object_add(object, key, json_object_new_boolean(value));
}

bool cloud_string_equal_ci(const char *left, const char *right)
{
	return left && right && strcasecmp(left, right) == 0;
}

bool cloud_string_ends(const char *value, const char *suffix)
{
	if(!value || !suffix)
		return false;
	size_t value_size = strlen(value);
	size_t suffix_size = strlen(suffix);
	return suffix_size <= value_size && memcmp(value + value_size - suffix_size, suffix, suffix_size) == 0;
}

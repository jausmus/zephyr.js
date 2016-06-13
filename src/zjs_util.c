// Copyright (c) 2016, Intel Corporation.

#include <string.h>

#include <zephyr.h>

#include "zjs_util.h"

#if defined(CONFIG_STDOUT_CONSOLE)
#include <stdio.h>
#define PRINT           printf
#else
#include <misc/printk.h>
#define PRINT           printk
#endif

// fifo of pointers to zjs_callback objects representing JS callbacks
struct nano_fifo zjs_callbacks_fifo;

void zjs_queue_init()
{
    nano_fifo_init(&zjs_callbacks_fifo);
}

void zjs_queue_callback(struct zjs_callback *cb) {
    // requires: cb is a callback structure containing a pointer to a JS
    //             callback object and a wrapper function that knows how to
    //             call the callback; this structure may contain additional
    //             fields used by that wrapper; JS objects and values within
    //             the structure should already be ref-counted so they won't
    //             be lost, and you deref them from call_function
    //  effects: adds this callback info to a fifo queue and will call the
    //             wrapper with this structure later, in a safe way, within
    //             the task context for proper serialization
    nano_fifo_put(&zjs_callbacks_fifo, cb);
}

int count = 0;

void zjs_run_pending_callbacks()
{
    // requires: call only from task context
    //  effects: calls all the callbacks in the queue
    struct zjs_callback *cb;
    while (1) {
        cb = nano_task_fifo_get(&zjs_callbacks_fifo, TICKS_NONE);
        if (!cb) {
            count += 1;
            break;
        }
        count = 0;

        if (unlikely(!cb->call_function)) {
            PRINT("error: no JS callback found\n");
            continue;
        }

        cb->call_function(cb);
    }
}

void zjs_obj_add_boolean(jerry_object_t *obj, bool bval, const char *name)
{
    // requires: obj is an existing JS object
    //  effects: creates a new field in parent named name, set to value
    jerry_value_t value = jerry_create_boolean_value(bval);
    jerry_set_object_field_value(obj, (const jerry_char_t *)name,
                                     &value);
}

void zjs_obj_add_function(jerry_object_t *obj, void *function,
                          const char *name)
{
    // requires: obj is an existing JS object, function is a native C function
    //  effects: creates a new field in object named name, that will be a JS
    //             JS function that calls the given C function
    jerry_object_t *func = jerry_create_external_function(function);
    jerry_value_t value;
    value.type = JERRY_DATA_TYPE_OBJECT;
    value.u.v_object = func;
    jerry_set_object_field_value(obj, (const jerry_char_t *)name,
                                     &value);
}

void zjs_obj_add_object(jerry_object_t *parent, jerry_object_t *child,
                        const char *name)
{
    // requires: parent and child are existing JS objects
    //  effects: creates a new field in parent named name, that refers to child
    jerry_value_t value;
    value.type = JERRY_DATA_TYPE_OBJECT;
    value.u.v_object = child;
    jerry_set_object_field_value(parent, (const jerry_char_t *)name,
                                     &value);
}

void zjs_obj_add_string(jerry_object_t *obj, const char *sval,
                        const char *name)
{
    // requires: obj is an existing JS object
    //  effects: creates a new field in parent named name, set to value
    jerry_string_t *str = jerry_create_string(sval);
    jerry_value_t value = jerry_create_string_value(str);
    jerry_set_object_field_value(obj, name, &value);
}

void zjs_obj_add_uint32(jerry_object_t *obj, uint32_t ival,
                        const char *name)
{
    // requires: obj is an existing JS object
    //  effects: creates a new field in parent named name, set to value
    jerry_value_t value;
    value.type = JERRY_DATA_TYPE_UINT32;
    value.u.v_uint32 = ival;
    jerry_set_object_field_value(obj, name, &value);
}

bool zjs_obj_get_boolean(jerry_object_t *obj, const char *name,
                         bool *flag)
{
    // requires: obj is an existing JS object, value name should exist as
    //             boolean
    //  effects: retrieves field specified by name as a boolean
    jerry_value_t value;
    if (!jerry_get_object_field_value(obj, name, &value))
        return false;

    if (value.type != JERRY_DATA_TYPE_BOOLEAN)
        return false;

    *flag = jerry_get_boolean_value(&value);
    jerry_release_value(&value);
    return true;
}

bool zjs_obj_get_string(jerry_object_t *obj, const char *name,
                        char *buffer, int len)
{
    // requires: obj is an existing JS object, value name should exist as
    //             string, buffer can receive the string, len is its size
    //  effects: retrieves field specified by name; if it exists, and is a
    //             string, copies at most len - 1 bytes plus a null terminator
    //             into buffer and returns true; otherwise, returns false
    jerry_value_t value;
    if (!jerry_get_object_field_value(obj, name, &value))
        return false;

    if (value.type != JERRY_DATA_TYPE_STRING)
        return false;

    jerry_string_t *str = jerry_get_string_value(&value);
    jerry_size_t jlen = jerry_get_string_size(str);
    if (jlen + 1 < len)
        len = jlen + 1;

    int wlen = jerry_string_to_char_buffer(str, buffer, len);
    buffer[wlen] = '\0';
    jerry_release_value(&value);
    return true;
}

bool zjs_obj_get_uint32(jerry_object_t *obj, const char *name,
                        uint32_t *num)
{
    // requires: obj is an existing JS object, value name should exist as number
    //  effects: retrieves field specified by name as a uint32
    jerry_value_t value;
    if (!jerry_get_object_field_value(obj, name, &value))
        return false;

    // work around bug in the above API, it never returns false
    if (value.type == JERRY_DATA_TYPE_UNDEFINED)
        return false;

    *num = (uint32_t)jerry_get_number_value(&value);
    jerry_release_value(&value);
    return true;
}

bool zjs_is_number(jerry_value_t value)
{
    if (value.type == JERRY_DATA_TYPE_UINT32 ||
        value.type == JERRY_DATA_TYPE_FLOAT32 ||
        value.type == JERRY_DATA_TYPE_FLOAT64)
        return true;
    return false;
}

// FIXME: NOT BEING USED CURRENTLY
// zjs_obj_get_string + strcmp suffices for my current needs, although I had
// already debugged this function so I want to at least check it in and see if
// it becomes useful
bool zjs_strequal(const jerry_string_t *jstr, const char *str)
{
    // requires: str is null-terminated and should be small so we don't use up
    //             too much stack space
    //  effects: returns the results of strcmp between the string underlying
    //             jstr and str
    int len = strlen(str);
    jerry_size_t jlen = jerry_get_string_size(jstr);
    if (len != jlen)
        return false;

    char buffer[jlen];
    int wlen = jerry_string_to_char_buffer(jstr, buffer, jlen);
    buffer[wlen] = '\0';
    for (int i=0; i<len; i++) {
        if (buffer[i] != str[i])
            return false;
    }
    return true;
}

/**
 * Initialize Jerry value with specified object
 */
void zjs_init_value_object(jerry_value_t *out_value_p, jerry_object_t *v)
{
    // requires: out_value_p to recieve the object value v
    //  effects: put the object into out_value_p with appropriate encoding.
    jerry_acquire_object (v);

    out_value_p->type = JERRY_DATA_TYPE_OBJECT;
    out_value_p->u.v_object = v;
}

/**
 * Initialize Jerry value with specified string
 */
void zjs_init_value_string(jerry_value_t *out_value_p, const char *v)
{
    // requires: out_value_p to recieve the string value v
    //  effects: put the string into out_value_p with appropriate encoding.

    out_value_p->type = JERRY_DATA_TYPE_STRING;
    out_value_p->u.v_string = jerry_create_string((jerry_char_t *) v);
}

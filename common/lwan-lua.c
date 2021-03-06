/*
 * lwan - simple web server
 * Copyright (c) 2014 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "lwan-private.h"

#include "lwan-array.h"
#include "lwan-cache.h"
#include "lwan-config.h"
#include "lwan-lua.h"

static const char *request_metatable_name = "Lwan.Request";

struct lwan_lua_priv {
    char *default_type;
    char *script_file;
    char *script;
    pthread_key_t cache_key;
    unsigned cache_period;
};

struct lwan_lua_state {
    struct cache_entry base;
    lua_State *L;
};

static ALWAYS_INLINE struct lwan_request *userdata_as_request(lua_State *L, int n)
{
    return *((struct lwan_request **)luaL_checkudata(L, n, request_metatable_name));
}

static int req_say_cb(lua_State *L)
{
    struct lwan_request *request = userdata_as_request(L, 1);
    size_t response_str_len;
    const char *response_str = lua_tolstring(L, -1, &response_str_len);

    strbuf_set_static(request->response.buffer, response_str, response_str_len);
    lwan_response_send_chunk(request);

    return 0;
}

static int req_send_event_cb(lua_State *L)
{
    struct lwan_request *request = userdata_as_request(L, 1);
    size_t event_str_len;
    const char *event_str = lua_tolstring(L, -1, &event_str_len);
    const char *event_name = lua_tostring(L, -2);

    strbuf_set_static(request->response.buffer, event_str, event_str_len);
    lwan_response_send_event(request, event_name);

    return 0;
}

static int req_set_response_cb(lua_State *L)
{
    struct lwan_request *request = userdata_as_request(L, 1);
    size_t response_str_len;
    const char *response_str = lua_tolstring(L, -1, &response_str_len);

    strbuf_set(request->response.buffer, response_str, response_str_len);

    return 0;
}

static int request_param_getter(lua_State *L,
        const char *(*getter)(struct lwan_request *req, const char *key))
{
    struct lwan_request *request = userdata_as_request(L, 1);
    const char *key_str = lua_tostring(L, -1);

    const char *value = getter(request, key_str);
    if (!value)
        lua_pushnil(L);
    else
        lua_pushstring(L, value);

    return 1;
}

static int req_query_param_cb(lua_State *L)
{
    return request_param_getter(L, lwan_request_get_query_param);
}

static int req_post_param_cb(lua_State *L)
{
    return request_param_getter(L, lwan_request_get_post_param);
}

static int req_cookie_cb(lua_State *L)
{
    return request_param_getter(L, lwan_request_get_cookie);
}

static bool append_key_value(lua_State *L, struct coro *coro,
    struct lwan_key_value_array *arr, char *key, int value_index)
{
    struct lwan_key_value *kv;

    kv = lwan_key_value_array_append(arr);
    if (!kv)
        return false;

    kv->key = key;
    kv->value = coro_strdup(coro, lua_tostring(L, value_index));

    return kv->value != NULL;
}

static int req_set_headers_cb(lua_State *L)
{
    const int table_index = 2;
    const int key_index = 1 + table_index;
    const int value_index = 2 + table_index;
    const int nested_value_index = value_index * 2 - table_index;
    struct lwan_key_value_array *headers;
    struct lwan_request *request = userdata_as_request(L, 1);
    struct coro *coro = request->conn->coro;
    struct lwan_key_value *kv;

    if (request->flags & RESPONSE_SENT_HEADERS)
        goto out;

    if (!lua_istable(L, table_index))
        goto out;

    headers = coro_lwan_key_value_array_new(request->conn->coro);
    if (!headers)
        goto out;

    lua_pushnil(L);
    while (lua_next(L, table_index) != 0) {
        char *key;

        if (!lua_isstring(L, key_index)) {
            lua_pop(L, 1);
            continue;
        }

        key = coro_strdup(request->conn->coro, lua_tostring(L, key_index));
        if (!key)
            goto out;

        if (lua_isstring(L, value_index)) {
            if (!append_key_value(L, coro, headers, key, value_index))
                goto out;
        } else if (lua_istable(L, value_index)) {
            lua_pushnil(L);

            for (; lua_next(L, value_index) != 0; lua_pop(L, 1)) {
                if (lua_isstring(L, nested_value_index))
                    continue;
                if (!append_key_value(L, coro, headers, key, nested_value_index))
                    goto out;
            }
        }

        lua_pop(L, 1);
    }

    kv = lwan_key_value_array_append(headers);
    if (!kv)
        goto out;
    kv->key = kv->value = NULL;

    request->response.headers = headers->base.base;
    lua_pushinteger(L, (lua_Integer)((struct lwan_array *)headers->base.elements));
    return 1;

out:
    lua_pushnil(L);
    return 1;
}

static const struct luaL_reg lwan_request_meta_regs[] = {
    { "query_param", req_query_param_cb },
    { "post_param", req_post_param_cb },
    { "set_response", req_set_response_cb },
    { "say", req_say_cb },
    { "send_event", req_send_event_cb },
    { "cookie", req_cookie_cb },
    { "set_headers", req_set_headers_cb },
    { NULL, NULL }
};

const char *lwan_lua_state_last_error(lua_State *L)
{
    return lua_tostring(L, -1);
}

lua_State *lwan_lua_create_state(const char *script_file, const char *script)
{
    lua_State *L;

    L = luaL_newstate();
    if (UNLIKELY(!L))
        return NULL;

    luaL_openlibs(L);

    luaL_newmetatable(L, request_metatable_name);
    luaL_register(L, NULL, lwan_request_meta_regs);
    lua_setfield(L, -1, "__index");

    if (script_file) {
        if (UNLIKELY(luaL_dofile(L, script_file) != 0)) {
            lwan_status_error("Error opening Lua script %s: %s",
                script_file, lua_tostring(L, -1));
            goto close_lua_state;
        }
    } else if (UNLIKELY(luaL_dostring(L, script) != 0)) {
        lwan_status_error("Error evaluating Lua script %s", lua_tostring(L, -1));
        goto close_lua_state;
    }

    return L;

close_lua_state:
    lua_close(L);
    return NULL;
}

static struct cache_entry *state_create(const char *key __attribute__((unused)),
        void *context)
{
    struct lwan_lua_priv *priv = context;
    struct lwan_lua_state *state = malloc(sizeof(*state));

    if (UNLIKELY(!state))
        return NULL;

    state->L = lwan_lua_create_state(priv->script_file, priv->script);
    if (LIKELY(state->L))
        return (struct cache_entry *)state;

    free(state);
    return NULL;
}

static void state_destroy(struct cache_entry *entry,
        void *context __attribute__((unused)))
{
    struct lwan_lua_state *state = (struct lwan_lua_state *)entry;

    lua_close(state->L);
    free(state);
}

static struct cache *get_or_create_cache(struct lwan_lua_priv *priv)
{
    struct cache *cache = pthread_getspecific(priv->cache_key);
    if (UNLIKELY(!cache)) {
        lwan_status_debug("Creating cache for this thread");
        cache = cache_create(state_create, state_destroy, priv, priv->cache_period);
        if (UNLIKELY(!cache))
            lwan_status_error("Could not create cache");
        /* FIXME: This cache instance leaks: store it somewhere and
         * free it on module shutdown */
        pthread_setspecific(priv->cache_key, cache);
    }
    return cache;
}

static void unref_thread(void *data1, void *data2)
{
    lua_State *L = data1;
    int thread_ref = (int)(intptr_t)data2;
    luaL_unref(L, LUA_REGISTRYINDEX, thread_ref);
}

static ALWAYS_INLINE const char *get_handle_prefix(struct lwan_request *request, size_t *len)
{
    if (request->flags & REQUEST_METHOD_GET) {
        *len = sizeof("handle_get_");
        return "handle_get_";
    }
    if (request->flags & REQUEST_METHOD_POST) {
        *len = sizeof("handle_post_");
        return "handle_post_";
    }
    if (request->flags & REQUEST_METHOD_HEAD) {
        *len = sizeof("handle_head_");
        return "handle_head_";
    }

    return NULL;
}

static bool get_handler_function(lua_State *L, struct lwan_request *request)
{
    char handler_name[128];
    size_t handle_prefix_len;
    const char *handle_prefix = get_handle_prefix(request, &handle_prefix_len);

    if (UNLIKELY(!handle_prefix))
        return false;
    if (UNLIKELY(request->url.len >= sizeof(handler_name) - handle_prefix_len))
        return false;

    char *url;
    size_t url_len;
    if (request->url.len) {
        url = strndupa(request->url.value, request->url.len);
        for (char *c = url; *c; c++) {
            if (*c == '/') {
                *c = '\0';
                break;
            }
            if (UNLIKELY(!isalnum(*c) && *c != '_'))
                return false;
        }
        url_len = strlen(url);
    } else {
        url = "root";
        url_len = 4;
    }

    if (UNLIKELY((handle_prefix_len + url_len + 1) > sizeof(handler_name)))
        return false;

    char *method_name = mempcpy(handler_name, handle_prefix, handle_prefix_len);
    memcpy(method_name - 1, url, url_len + 1);

    lua_getglobal(L, handler_name);
    return lua_isfunction(L, -1);
}

void lwan_lua_state_push_request(lua_State *L, struct lwan_request *request)
{
    struct lwan_request **userdata = lua_newuserdata(L, sizeof(struct lwan_request *));
    *userdata = request;
    luaL_getmetatable(L, request_metatable_name);
    lua_setmetatable(L, -2);
}

static lua_State *push_newthread(lua_State *L, struct coro *coro)
{
    lua_State *L1 = lua_newthread(L);
    if (UNLIKELY(!L1))
        return NULL;

    int thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    coro_defer2(coro, CORO_DEFER2(unref_thread), L, (void *)(intptr_t)thread_ref);

    return L1;
}

static enum lwan_http_status
lua_handle_cb(struct lwan_request *request,
              struct lwan_response *response,
              void *data)
{
    struct lwan_lua_priv *priv = data;

    if (UNLIKELY(!priv))
        return HTTP_INTERNAL_ERROR;

    struct cache *cache = get_or_create_cache(priv);
    if (UNLIKELY(!cache))
        return HTTP_INTERNAL_ERROR;

    struct lwan_lua_state *state = (struct lwan_lua_state *)cache_coro_get_and_ref_entry(
            cache, request->conn->coro, "");
    if (UNLIKELY(!state))
        return HTTP_NOT_FOUND;

    lua_State *L = push_newthread(state->L, request->conn->coro);
    if (UNLIKELY(!L))
        return HTTP_INTERNAL_ERROR;

    if (UNLIKELY(!get_handler_function(L, request)))
        return HTTP_NOT_FOUND;

    int n_arguments = 1;
    lwan_lua_state_push_request(L, request);
    response->mime_type = priv->default_type;
    while (true) {
        switch (lua_resume(L, n_arguments)) {
        case LUA_YIELD:
            coro_yield(request->conn->coro, CONN_CORO_MAY_RESUME);
            n_arguments = 0;
            break;
        case 0:
            return HTTP_OK;
        default:
            lwan_status_error("Error from Lua script: %s", lua_tostring(L, -1));
            return HTTP_INTERNAL_ERROR;
        }
    }
}

static void *lua_init(const char *prefix __attribute__((unused)), void *data)
{
    struct lwan_lua_settings *settings = data;
    struct lwan_lua_priv *priv;

    priv = calloc(1, sizeof(*priv));
    if (!priv) {
        lwan_status_error("Could not allocate memory for private Lua struct");
        return NULL;
    }

    priv->default_type = strdup(
        settings->default_type ? settings->default_type : "text/plain");
    if (!priv->default_type) {
        lwan_status_perror("strdup");
        goto error;
    }

    if (settings->script) {
        priv->script = strdup(settings->script);
        if (!priv->script) {
            lwan_status_perror("strdup");
            goto error;
        }
    } else if (settings->script_file) {
        priv->script_file = strdup(settings->script_file);
        if (!priv->script_file) {
            lwan_status_perror("strdup");
            goto error;
        }
    } else {
        lwan_status_error("No Lua script_file or script provided");
        goto error;
    }

    if (pthread_key_create(&priv->cache_key, NULL)) {
        lwan_status_perror("pthread_key_create");
        goto error;
    }

    priv->cache_period = settings->cache_period;

    return priv;

error:
    free(priv->script_file);
    free(priv->default_type);
    free(priv->script);
    free(priv);
    return NULL;
}

static void lua_shutdown(void *data)
{
    struct lwan_lua_priv *priv = data;
    if (priv) {
        pthread_key_delete(priv->cache_key);
        free(priv->default_type);
        free(priv->script_file);
        free(priv->script);
        free(priv);
    }
}

static void *lua_init_from_hash(const char *prefix, const struct hash *hash)
{
    struct lwan_lua_settings settings = {
        .default_type = hash_find(hash, "default_type"),
        .script_file = hash_find(hash, "script_file"),
        .cache_period = parse_time_period(hash_find(hash, "cache_period"), 15),
        .script = hash_find(hash, "script")
    };
    return lua_init(prefix, &settings);
}

const struct lwan_module *lwan_module_lua(void)
{
    static const struct lwan_module lua_module = {
        .init = lua_init,
        .init_from_hash = lua_init_from_hash,
        .shutdown = lua_shutdown,
        .handle = lua_handle_cb,
        .flags = HANDLER_PARSE_QUERY_STRING
            | HANDLER_REMOVE_LEADING_SLASH
            | HANDLER_PARSE_COOKIES
    };

    return &lua_module;
}

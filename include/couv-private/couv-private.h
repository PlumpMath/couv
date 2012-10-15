#ifndef _COUV_PRIVATE_H
#define _COUV_PRIVATE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <lauxlib.h>
#include <uv.h>

#include "couv.h"
#include "ngx-queue.h"

typedef void *(*couv_alloc_t)(lua_State *L, size_t nbytes);
typedef void (*couv_free_t)(lua_State *L, void *ptr);

typedef struct couv_buf_mem_s couv_buf_mem_t;
struct couv_buf_mem_s {
  int ref_cnt;
  couv_free_t free;
  char mem[1];
};

void *couv_buf_mem_alloc(lua_State *L, size_t nbytes);
void couv_buf_mem_retain(lua_State *L, void *ptr);
void couv_buf_mem_release(lua_State *L, void *ptr);

typedef struct couv_buf_s {
  void *orig;
  uv_buf_t buf;
} couv_buf_t;

typedef struct couv_udp_input_s {
  ngx_queue_t *prev;
  ngx_queue_t *next;
  ssize_t nread;
  couv_buf_t w_buf;
  union {
    struct sockaddr_storage storage;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
  } addr;
} couv_udp_input_t;

typedef struct couv_udp_s {
  uv_udp_t handle;
  int is_yielded_for_recv;
  ngx_queue_t input_queue;
} couv_udp_t;

typedef struct couv_stream_input_s {
  ngx_queue_t *prev;
  ngx_queue_t *next;
  ssize_t nread;
  couv_buf_t w_buf;
} couv_stream_input_t;

typedef struct couv_stream_s {
  uv_stream_t handle;
  int is_yielded_for_read;
  ngx_queue_t input_queue;
} couv_stream_t;

typedef struct couv_tcp_s {
  uv_tcp_t handle;
  int is_yielded_for_read;
  ngx_queue_t input_queue;
} couv_tcp_t;


/*
 * buffer
 */
int luaopen_couv_buffer(lua_State *L);

#define COUV_BUFFER_MTBL_NAME "couv.Buffer"
#define couv_checkbuf(L, index) \
    luaL_checkudata(L, index, COUV_BUFFER_MTBL_NAME)
uv_buf_t couv_tobuforstr(lua_State *L, int index);
uv_buf_t couv_checkbuforstr(lua_State *L, int index);

/* NOTE: you must free the result buffers array with couv_free. */
uv_buf_t *couv_checkbuforstrtable(lua_State *L, int index, size_t *buffers_cnt);

void couv_dbg_print_bufs(const char *header, uv_buf_t *bufs, size_t bufcnt);

#define couv_argcheckindex(L, arg_index, index, min, max) \
  luaL_argcheck(L, (int)min <= index && index <= (int)max, arg_index, \
      "index out of range");


/*
 * loop
 */
int luaopen_couv_loop(lua_State *L);
uv_loop_t *couv_loop(lua_State *L);

#define COUV_LOOP_REGISTRY_KEY "couv.loop"

/*
 * ipaddr
 */
#define COUV_IP4ADDR_MTBL_NAME "couv.Ip4addr"
#define couv_checkip4addr(L, index) \
  (struct sockaddr_in *)luaL_checkudata(L, index, COUV_IP4ADDR_MTBL_NAME)

#define COUV_IP6ADDR_MTBL_NAME "couv.Ip6addr"
#define couv_checkip6addr(L, index) \
  (struct sockaddr_in6 *)luaL_checkudata(L, index, COUV_IP6ADDR_MTBL_NAME)

int luaopen_couv_ipaddr(lua_State *L);

int couv_dbg_print_ip4addr(const char *header, struct sockaddr_in *addr);

/*
 * fs
 */
int luaopen_couv_fs(lua_State *L);

/*
 * tcp
 */
int luaopen_couv_tcp(lua_State *L);

/*
 * udp
 */
int luaopen_couv_udp(lua_State *L);

/*
 * auxlib
 */
#if LUA_VERSION_NUM == 502
#define couv_resume(L, from, nargs) lua_resume(L, from, nargs)
#elif LUA_VERSION_NUM == 501
#define couv_resume(L, from, nargs) lua_resume(L, nargs)
#else
#error
#endif

void *couv_alloc(lua_State *L, size_t size);
void couv_free(lua_State *L, void *ptr);

int couvL_is_in_mainthread(lua_State *L);
int couvL_hasmetatablename(lua_State *L, int index, const char *tname);
const char *couvL_uv_errname(int uv_errcode);

int couv_registry_set_for_ptr(lua_State *L, void *ptr, int index);
int couv_registry_get_for_ptr(lua_State *L, void *ptr);
int couv_registry_delete_for_ptr(lua_State *L, void *ptr);

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#define container_of(ptr, type, member) \
  ((type *) ((char *) (ptr) - offsetof(type, member)))

#define couvL_SET_FIELD(L, name, type, val) \
  lua_push##type(L, val);                   \
  lua_setfield(L, -2, #name)

#ifdef __cplusplus
}
#endif
#endif /* _COUV_PRIVATE_H */

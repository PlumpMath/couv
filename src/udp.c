#include "couv-private.h"

typedef struct couv_udp_send_s {
  uv_buf_t *bufs;
  uv_udp_send_t req;
} couv_udp_send_t;

static couv_udp_send_t *couv_alloc_udp_send(lua_State *L, uv_buf_t *bufs) {
  couv_udp_send_t *req;

  req = couv_alloc(L, sizeof(couv_udp_send_t));
  if (!req)
    return NULL;

  req->bufs = bufs;
  return req;
}


static uv_udp_t *couv_alloc_udp_handle(lua_State *L) {
  couv_udp_t *w_handle;

  w_handle = couv_alloc(L, sizeof(couv_udp_t));
  if (!w_handle)
    return NULL;

  if (couvL_is_mainthread(L)) {
    luaL_error(L, "udp handle must be created in coroutine, not in main thread.");
    return NULL;
  } else
    w_handle->threadref = luaL_ref(L, LUA_REGISTRYINDEX);
  return &w_handle->handle;
}

void couv_free_udp_handle(lua_State *L, uv_udp_t *handle) {
  couv_udp_t *w_handle;

  w_handle = container_of(handle, couv_udp_t, handle);
  luaL_unref(L, LUA_REGISTRYINDEX, w_handle->threadref);
  couv_free(L, w_handle);
}

static int udp_create(lua_State *L) {
  uv_loop_t *loop;
  uv_udp_t *handle;
  couv_udp_t *w_handle;
  int r;

  handle = couv_alloc_udp_handle(L);
  if (!handle)
    return 0;

  loop = couv_loop(L);
  r = uv_udp_init(loop, handle);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(loop).code));
  }

  handle->data = L;
  w_handle = container_of(handle, couv_udp_t, handle);
  w_handle->is_yielded_for_recv = 0;
  ngx_queue_init(&w_handle->input_queue);

  lua_pushlightuserdata(L, handle);
  return 1;
}

static int udp_open(lua_State *L) {
  uv_udp_t *handle;
  couv_udp_t *w_handle;
  uv_os_sock_t sock;
  int r;

  handle = lua_touserdata(L, 1);
  sock = (uv_os_sock_t)luaL_checkinteger(L, 2);
  r = uv_udp_open(handle, sock);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  w_handle = container_of(handle, couv_udp_t, handle);
  w_handle->is_yielded_for_recv = 0;
  return 0;
}

static int udp_bind(lua_State *L) {
  uv_udp_t *handle;
  struct sockaddr_in *ip4addr;
  struct sockaddr_in6 *ip6addr;
  unsigned flags;
  int r;

  handle = lua_touserdata(L, 1);
  flags = luaL_optint(L, 3, 0);
  if ((ip4addr = couvL_testip4addr(L, 2)) != NULL)
    r = uv_udp_bind(handle, *ip4addr, flags);
  else if ((ip6addr = couvL_testip6addr(L, 2)) != NULL)
    r = uv_udp_bind6(handle, *ip6addr, flags);
  else
    return luaL_error(L, "must be ip4addr or ip6addr");
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return 0;
}

static void udp_send_cb(uv_udp_send_t* req, int status) {
  couv_udp_send_t *holder;
  lua_State *L;
  int nresults;

  holder = container_of(req, couv_udp_send_t, req);
  L = req->handle->data;
  if (status < 0) {
    lua_pushstring(L, couvL_uv_errname(uv_last_error(req->handle->loop).code));
    nresults = 1;
  } else
    nresults = 0;
  couv_free(L, holder->bufs);
  couv_free(L, holder);
  couv_resume(L, L, nresults);
}

static int udp_send(lua_State *L) {
  uv_udp_t *handle;
  struct sockaddr_in *ip4addr;
  struct sockaddr_in6 *ip6addr;
  uv_buf_t *bufs;
  size_t bufcnt;
  couv_udp_send_t *holder;
  uv_udp_send_t *req;
  int r;

  handle = lua_touserdata(L, 1);
  bufs = couv_checkbuforstrtable(L, 2, &bufcnt);
  holder = couv_alloc_udp_send(L, bufs);
  req = &holder->req;
  if ((ip4addr = couvL_testip4addr(L, 3)) != NULL)
    r = uv_udp_send(req, handle, bufs, (int)bufcnt, *ip4addr, udp_send_cb);
  else if ((ip6addr = couvL_testip6addr(L, 3)) != NULL)
    r = uv_udp_send6(req, handle, bufs, (int)bufcnt, *ip6addr, udp_send_cb);
  else
    return luaL_error(L, "must be ip4addr or ip6addr");
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return lua_yield(L, 0);
}

static void udp_recv_cb(uv_udp_t *handle, ssize_t nread, uv_buf_t buf,
    struct sockaddr* addr, unsigned flags) {
  couv_udp_t *w_handle;
  lua_State *L;
  couv_udp_input_t *input;

  w_handle = container_of(handle, couv_udp_t, handle);
  L = handle->data;

  input = couv_alloc(L, sizeof(couv_udp_input_t));
  if (!input)
    return;

  input->nread = nread;
  input->w_buf.orig = buf.base;
  input->w_buf.buf = buf;
  if (addr)
    input->addr.v4 = *(struct sockaddr_in *)addr;
  ngx_queue_insert_tail(&w_handle->input_queue, (ngx_queue_t *)input);

  if (lua_status(L) == LUA_YIELD && w_handle->is_yielded_for_recv) {
    w_handle->is_yielded_for_recv = 0;
    couv_resume(L, L, 0);
  }
}

static int udp_recv_start(lua_State *L) {
  uv_udp_t *handle;
  int r;

  handle = lua_touserdata(L, 1);
  r = uv_udp_recv_start(handle, couv_buf_alloc_cb, udp_recv_cb);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return 0;
}

static int udp_recv_stop(lua_State *L) {
  uv_udp_t *handle;
  int r;

  handle = lua_touserdata(L, 1);
  r = uv_udp_recv_stop(handle);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return 0;
}

static int udp_prim_recv(lua_State *L) {
  uv_udp_t *handle;
  couv_udp_t *w_handle;
  couv_udp_input_t *input;
  couv_buf_t *w_buf;
  struct sockaddr_in *ip4addr;

  handle = lua_touserdata(L, 1);
  w_handle = container_of(handle, couv_udp_t, handle);
  if (ngx_queue_empty(&w_handle->input_queue)) {
    w_handle->is_yielded_for_recv = 1;
    return lua_yield(L, 0);
  }
  input = (couv_udp_input_t *)ngx_queue_head(&w_handle->input_queue);
  ngx_queue_remove(input);

  lua_pushnumber(L, input->nread);

  w_buf = lua_newuserdata(L, sizeof(couv_buf_t));
  luaL_getmetatable(L, COUV_BUFFER_MTBL_NAME);
  lua_setmetatable(L, -2);
  *w_buf = input->w_buf;

  ip4addr = lua_newuserdata(L, sizeof(struct sockaddr_in));
  luaL_getmetatable(L, COUV_IP4ADDR_MTBL_NAME);
  lua_setmetatable(L, -2);
  *ip4addr = input->addr.v4;

  return 3;
}

static int udp_getsockname(lua_State *L) {
  uv_udp_t *handle;
  struct sockaddr_storage name;
  int namelen;
  int r;

  handle = lua_touserdata(L, 1);
  namelen = sizeof(name);
  r = uv_udp_getsockname(handle, (struct sockaddr *)&name, &namelen);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return couv_push_ipaddr_raw(L, (struct sockaddr *)&name);
}

static int udp_set_membership(lua_State *L) {
  uv_udp_t *handle;
  const char *multicast_addr;
  int interface_addr_type;
  const char *interface_addr;
  uv_membership membership;
  int r;

  handle = lua_touserdata(L, 1);
  multicast_addr = luaL_checkstring(L, 2);
  interface_addr_type = lua_type(L, 3);
  if (interface_addr_type == LUA_TSTRING)
    interface_addr = lua_tostring(L, 3);
  if (interface_addr_type == LUA_TNIL)
    interface_addr = NULL;
  else
    return luaL_argerror(L, 3, "must be string or nil");
  membership = luaL_checkint(L, 4);
  r = uv_udp_set_membership(handle, multicast_addr, interface_addr, membership);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return 0;
}

static int udp_set_multicast_loop(lua_State *L) {
  uv_udp_t *handle;
  int on;
  int r;

  handle = lua_touserdata(L, 1);
  on = lua_toboolean(L, 2);
  r = uv_udp_set_multicast_loop(handle, on);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return 0;
}

static int udp_set_multicast_ttl(lua_State *L) {
  uv_udp_t *handle;
  int ttl;
  int r;

  handle = lua_touserdata(L, 1);
  ttl = luaL_checkint(L, 2);
  r = uv_udp_set_multicast_ttl(handle, ttl);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return 0;
}

static int udp_set_broadcast(lua_State *L) {
  uv_udp_t *handle;
  int on;
  int r;

  handle = lua_touserdata(L, 1);
  on = lua_toboolean(L, 2);
  r = uv_udp_set_broadcast(handle, on);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return 0;
}

static int udp_set_ttl(lua_State *L) {
  uv_udp_t *handle;
  int ttl;
  int r;

  handle = lua_touserdata(L, 1);
  ttl = luaL_checkint(L, 2);
  r = uv_udp_set_ttl(handle, ttl);
  if (r < 0) {
    return luaL_error(L, couvL_uv_errname(uv_last_error(couv_loop(L)).code));
  }
  return 0;
}

static const struct luaL_Reg udp_functions[] = {
  { "udp_bind", udp_bind },
  { "udp_create", udp_create },
  { "udp_getsockname", udp_getsockname },
  { "udp_open", udp_open },
  { "udp_prim_recv", udp_prim_recv },
  { "udp_recv_start", udp_recv_start },
  { "udp_recv_stop", udp_recv_stop },
  { "udp_send", udp_send },
  { "udp_set_broadcast", udp_set_broadcast },
  { "udp_set_membership", udp_set_membership },
  { "udp_set_multicast_loop", udp_set_multicast_loop },
  { "udp_set_multicast_ttl", udp_set_multicast_ttl },
  { "udp_set_ttl", udp_set_ttl },
  { NULL, NULL }
};

int luaopen_couv_udp(lua_State *L) {
  couvL_SET_FIELD(L, UDP_IPV6ONLY, number, UV_UDP_IPV6ONLY);

  couvL_SET_FIELD(L, JOIN_GROUP, number, UV_JOIN_GROUP);
  couvL_SET_FIELD(L, LEAVE_GROUP, number, UV_LEAVE_GROUP);

  couvL_setfuncs(L, udp_functions, 0);

  return 1;
}

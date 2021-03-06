#include "couv-private.h"

#define couv_get_udp_handle_data(h) (&((couv_udp_t *)h)->hdata)

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


static uv_udp_t *couv_new_udp_handle(lua_State *L) {
  couv_udp_t *w_handle;
  uv_udp_t *handle;

  w_handle = lua_newuserdata(L, sizeof(couv_udp_t));
  if (!w_handle)
    return NULL;

  lua_getfield(L, LUA_REGISTRYINDEX, COUV_UDP_MTBL_NAME);
  lua_setmetatable(L, -2);

  handle = &w_handle->handle;

  if (couvL_is_mainthread(L)) {
    luaL_error(L, "udp handle must be created in coroutine, not in main thread.");
    return NULL;
  } else
    couv_rawsetp(L, LUA_REGISTRYINDEX, COUV_THREAD_REG_KEY(handle));
  return handle;
}

void couv_clean_udp_handle(lua_State *L, uv_udp_t *handle) {
  lua_pushnil(L);
  couv_rawsetp(L, LUA_REGISTRYINDEX, COUV_USERDATA_REG_KEY(handle));

  lua_pushnil(L);
  couv_rawsetp(L, LUA_REGISTRYINDEX, COUV_THREAD_REG_KEY(handle));
}

static int udp_new(lua_State *L) {
  uv_loop_t *loop;
  uv_udp_t *handle;
  couv_udp_handle_data_t *hdata;
  int r;

  handle = couv_new_udp_handle(L);
  if (!handle)
    return 0;

  loop = couv_loop(L);
  r = uv_udp_init(loop, handle);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(loop));
  }

  handle->data = L;
  hdata = couv_get_udp_handle_data(handle);
  ngx_queue_init(&hdata->input_queue);

  lua_pushvalue(L, -1);
  couv_rawsetp(L, LUA_REGISTRYINDEX, COUV_USERDATA_REG_KEY(handle));
  return 1;
}

static int udp_open(lua_State *L) {
  uv_udp_t *handle;
  uv_os_sock_t sock;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  sock = (uv_os_sock_t)luaL_checkinteger(L, 2);
  r = uv_udp_open(handle, sock);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return 0;
}

static int udp_bind(lua_State *L) {
  uv_udp_t *handle;
  struct sockaddr *addr;
  unsigned flags;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  addr = couvL_checkudataclass(L, 2, COUV_SOCK_ADDR_MTBL_NAME);
  flags = luaL_optint(L, 3, 0);
  if (addr->sa_family == AF_INET)
    r = uv_udp_bind(handle, *(struct sockaddr_in *)addr, flags);
  else
    r = uv_udp_bind6(handle, *(struct sockaddr_in6 *)addr, flags);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
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
    lua_pushstring(L, couvL_uv_lasterrname(req->handle->loop));
    nresults = 1;
  } else
    nresults = 0;
  couv_free(L, holder->bufs);
  couv_free(L, holder);
  couv_resume(L, L, nresults);
}

static int udp_send(lua_State *L) {
  uv_udp_t *handle;
  struct sockaddr *addr;
  uv_buf_t *bufs;
  size_t bufcnt;
  couv_udp_send_t *holder;
  uv_udp_send_t *req;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  bufs = couv_checkbuforstrtable(L, 2, &bufcnt);
  addr = couvL_checkudataclass(L, 3, COUV_SOCK_ADDR_MTBL_NAME);
  holder = couv_alloc_udp_send(L, bufs);
  req = &holder->req;
  if (addr->sa_family == AF_INET) {
    r = uv_udp_send(req, handle, bufs, (int)bufcnt,
        *(struct sockaddr_in *)addr, udp_send_cb);
  } else {
    r = uv_udp_send6(req, handle, bufs, (int)bufcnt,
        *(struct sockaddr_in6 *)addr, udp_send_cb);
  }
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return lua_yield(L, 0);
}

static void udp_recv_cb(uv_udp_t *handle, ssize_t nread, uv_buf_t buf,
    struct sockaddr* addr, unsigned flags) {
  lua_State *L;
  couv_udp_input_t *input;
  couv_udp_handle_data_t *hdata;

  L = handle->data;

  input = couv_alloc(L, sizeof(couv_udp_input_t));
  if (!input)
    return;

  input->nread = nread;
  input->w_buf.orig = buf.base;
  input->w_buf.buf = buf;
  if (addr)
    input->addr.v4 = *(struct sockaddr_in *)addr;

  hdata = couv_get_udp_handle_data(handle);
  ngx_queue_insert_tail(&hdata->input_queue, (ngx_queue_t *)input);

  if (lua_status(L) == LUA_YIELD)
    couv_resume(L, L, 0);
}

static int udp_recv_start(lua_State *L) {
  uv_udp_t *handle;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  r = uv_udp_recv_start(handle, couv_buf_alloc_cb, udp_recv_cb);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return 0;
}

static int udp_recv_stop(lua_State *L) {
  uv_udp_t *handle;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  r = uv_udp_recv_stop(handle);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return 0;
}

static int udp_prim_recv(lua_State *L) {
  uv_udp_t *handle;
  couv_udp_input_t *input;
  couv_buf_t *w_buf;
  struct sockaddr_in *ip4addr;
  couv_udp_handle_data_t *hdata;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  hdata = couv_get_udp_handle_data(handle);
  if (ngx_queue_empty(&hdata->input_queue))
    return lua_yield(L, 0);
  input = (couv_udp_input_t *)ngx_queue_head(&hdata->input_queue);
  ngx_queue_remove(input);

  lua_pushnumber(L, input->nread);

  w_buf = lua_newuserdata(L, sizeof(couv_buf_t));
  luaL_getmetatable(L, COUV_BUFFER_MTBL_NAME);
  lua_setmetatable(L, -2);
  *w_buf = input->w_buf;

  ip4addr = lua_newuserdata(L, sizeof(struct sockaddr_in));
  luaL_getmetatable(L, COUV_SOCK_ADDR_V4_MTBL_NAME);
  lua_setmetatable(L, -2);
  *ip4addr = input->addr.v4;

  return 3;
}

static int udp_getsockname(lua_State *L) {
  uv_udp_t *handle;
  struct sockaddr_storage name;
  int namelen;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  namelen = sizeof(name);
  r = uv_udp_getsockname(handle, (struct sockaddr *)&name, &namelen);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return couvL_pushsockaddr(L, (struct sockaddr *)&name);
}

static int udp_set_membership(lua_State *L) {
  uv_udp_t *handle;
  const char *multicast_addr;
  int interface_addr_type;
  const char *interface_addr;
  uv_membership membership;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
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
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return 0;
}

static int udp_set_multicast_loop(lua_State *L) {
  uv_udp_t *handle;
  int on;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  on = lua_toboolean(L, 2);
  r = uv_udp_set_multicast_loop(handle, on);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return 0;
}

static int udp_set_multicast_ttl(lua_State *L) {
  uv_udp_t *handle;
  int ttl;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  ttl = luaL_checkint(L, 2);
  r = uv_udp_set_multicast_ttl(handle, ttl);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return 0;
}

static int udp_set_broadcast(lua_State *L) {
  uv_udp_t *handle;
  int on;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  on = lua_toboolean(L, 2);
  r = uv_udp_set_broadcast(handle, on);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return 0;
}

static int udp_set_ttl(lua_State *L) {
  uv_udp_t *handle;
  int ttl;
  int r;

  handle = couvL_checkudataclass(L, 1, COUV_UDP_MTBL_NAME);
  ttl = luaL_checkint(L, 2);
  r = uv_udp_set_ttl(handle, ttl);
  if (r < 0) {
    return luaL_error(L, couvL_uv_lasterrname(couv_loop(L)));
  }
  return 0;
}

static const struct luaL_Reg udp_methods[] = {
  { "bind", udp_bind },
  { "getsockname", udp_getsockname },
  { "open", udp_open },
  { "_recv", udp_prim_recv },
  { "_send", udp_send },
  { "setBroadcast", udp_set_broadcast },
  { "setMembership", udp_set_membership },
  { "setMulticastLoop", udp_set_multicast_loop },
  { "setMulticastTtl", udp_set_multicast_ttl },
  { "setTtl", udp_set_ttl },
  { "startRecv", udp_recv_start },
  { "stopRecv", udp_recv_stop },
  { NULL, NULL }
};

static const struct luaL_Reg udp_functions[] = {
  { "new", udp_new },
  { NULL, NULL }
};

int luaopen_couv_udp(lua_State *L) {
  lua_newtable(L);
  couvL_setfuncs(L, udp_functions, 0);

  couvL_SET_FIELD(L, IPV6ONLY, number, UV_UDP_IPV6ONLY);

  couvL_SET_FIELD(L, JOIN_GROUP, number, UV_JOIN_GROUP);
  couvL_SET_FIELD(L, LEAVE_GROUP, number, UV_LEAVE_GROUP);

  lua_setfield(L, -2, "Udp");

  couv_newmetatable(L, COUV_UDP_MTBL_NAME, COUV_HANDLE_MTBL_NAME);
  couvL_setfuncs(L, udp_methods, 0);
  lua_setfield(L, -2, "_Udp");

  return 0;
}

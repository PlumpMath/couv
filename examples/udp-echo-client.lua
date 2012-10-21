local uv = require 'couv'

local TEST_PORT = 9123

coroutine.wrap(function()
  local addr = uv.ip4addr('127.0.0.1', TEST_PORT)
  local handle = uv.udp_create()

  uv.udp_recv_start(handle)

  uv.udp_send(handle, {"PING", "world"}, addr)
  print('udp_client sent {"PING", "world"}')

--[[
  local nread, buf, addr = uv.udp_recv(handle)
  print("udp_client recv nread=", nread)
  if nread and nread > 0 then
    print("udp_client recv msg=", buf:toString(1, nread), ", host=", addr:host(), ", port=", addr:port())
  end

  uv.udp_send(handle, {"hey"}, addr)
  print('udp_client sent {"hey"}')

  nread, buf, addr = uv.udp_recv(handle)
  print("udp_client recv nread=", nread)
  if nread and nread > 0 then
    print("udp_client recv msg=", buf:toString(1, nread), ", host=", addr:host(), ", port=", addr:port())
  end

  uv.udp_recv_stop(handle)
--]]
  uv.close(handle)
end)()

uv.run()

-- Rune clean-room RAAT high-level message layer.
-- Wire contract (fixed by the RAAT protocol itself): message type 1 carries
-- a request (i32be rid + JSON body); replies go out as type 2 with the same
-- rid plus a "final" flag byte; type 3 carries an artwork blob that
-- precedes its JSON description.

local rjson = require("rjson")

local rprotocol = {}

local TYPE_REQUEST  = 1
local TYPE_RESPONSE = 2
local TYPE_ARTWORK  = 3

rprotocol.onRequest = function(cx, req) end -- replaced by the protocol module
rprotocol.artworkBlob = nil

RequestContext = {}
RequestContext.__index = RequestContext

function RequestContext.new(rid, quiet)
  return setmetatable({ rid = rid, quiet = quiet, final = false },
                      RequestContext)
end

function RequestContext.reply(self, body, isFinal)
  if self.quiet then
    body.quiet = true
  end
  local text = rjson.encode(body)
  local buf = buffer.new(5 + #text)
  buf:set_int32_be(0, self.rid)
  buf:set_uint8(4, isFinal and 1 or 0)
  buf:set_string(5, text)
  if not self.quiet then
    raat.log.trace("reply [" .. tostring(self.rid) .. "] " .. text)
  end
  raat.session.send_message(TYPE_RESPONSE, buf)
end

function RequestContext.respond(self, body)
  self:reply(body, true)
end

function RequestContext.respond_nonfinal(self, body)
  self:reply(body, false)
end

local function safeRespond(cx, body)
  local ok, err = pcall(function() cx:respond(body) end)
  if not ok then
    raat.log.error("respond failed: " .. tostring(err))
  end
end

local function onIncoming(messageType, body)
  -- Everything below runs under pcall: on the device an unprotected Lua
  -- error here is 100% silent (the C dispatcher pops the error with no
  -- log), which looks exactly like "dispatch is dead". Surface it instead.
  local ok, err = pcall(function()
    if messageType ~= TYPE_REQUEST then
      if messageType == TYPE_ARTWORK then
        rprotocol.artworkBlob = body
      else
        raat.log.trace("unexpected message type " .. tostring(messageType))
      end
      return
    end
    local buf = buffer.new(body)
    local rid = buf:get_int32_be(0)
    local text = buf:get_string(4, buf:length() - 4)
    local okd, req = pcall(rjson.decode, text)
    if not okd then
      local preview = string.gsub(text, '"host"%s*:%s*"[^"]*"', '"host":"<redacted>"')
      safeRespond(RequestContext.new(rid, false), {
        status = "Error",
        message = "decode raised: " .. tostring(req) .. " near: "
                  .. string.sub(preview, 1, 160),
      })
      return
    end
    if req == nil then
      local preview = string.gsub(text, '"host"%s*:%s*"[^"]*"', '"host":"<redacted>"')
      raat.log.warning("undecodable request payload: "
        .. string.sub(preview, 1, 240))
      safeRespond(RequestContext.new(rid, false),
        { status = "InvalidRequest", message = "undecodable json" })
      return
    end
    if not req.quiet then
      raat.log.trace("request [" .. tostring(rid) .. "] " .. text)
    end
    local okr, rerr = pcall(rprotocol.onRequest,
      RequestContext.new(rid, req.quiet), req)
    if not okr then
      raat.log.error("request handler error: " .. tostring(rerr))
      safeRespond(RequestContext.new(rid, req.quiet),
        { status = "Error", message = "handler: " .. tostring(rerr) })
    end
  end)
  if not ok then
    raat.log.error("onIncoming raised: " .. tostring(err))
  end
end

table.insert(raat.session.message_handlers, onIncoming)

return rprotocol

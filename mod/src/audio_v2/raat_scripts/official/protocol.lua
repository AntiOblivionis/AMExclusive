local base     = require("base")
local json     = require("dkjson")

local protocol = { }

local REQUEST         = 0x00000001
local RESPONSE        = 0x00000002
local ARTWORK         = 0x00000003

protocol.on_request = function (cx, r) end

RequestContext = { }
RequestContext.__index = RequestContext

function RequestContext.new(rid,quiet)
    local self = setmetatable({}, RequestContext)
    self.rid = rid
    self.quiet = quiet
    self.did_final_response = false
    return self
end

function RequestContext.respond_base(self, response, isfinal)
    if self.quiet then
        response.quiet = true
    end
    local jsonstr = json.encode(response, { indent = false })
    local buf = buffer.new(5 + #jsonstr)
    buf:set_int32_be(0, self.rid)
    if isfinal then
        buf:set_uint8(4, 1)
    else
        buf:set_uint8(4, 0)
    end
    buf:set_string(5, jsonstr)
    if not self.quiet then 
        raat.log.trace("SENT [" .. tostring(self.rid) .. "] " .. jsonstr)
    end
    raat.session.send_message(RESPONSE, buf)
end

function RequestContext.respond(self, response)
    self:respond_base(response, true)
end

function RequestContext.respond_nonfinal(self, response)
    self:respond_base(response, false)
end

function on_message(message_type, message_body)
    if message_type == REQUEST then
        local buf = buffer.new(message_body)
        local rid = buf:get_int32_be(0)
        local jsonstr = buf:get_string(4, buf:length() - 4)
        local json = json.decode(jsonstr, 1, nil)
        if not json.quiet then
            raat.log.trace("GOT [" .. tostring(rid) .. "] " .. jsonstr)
        end
        protocol.on_request(RequestContext.new(rid, json.quiet), json)
    elseif message_type == ARTWORK then
        -- artwork is uploaded in binary form ahead of a JSON message. Stash the blob away here
        protocol.artwork_blob = message_body
    else
        raat.log.trace("received unexpected message of type", message_type)
    end
end

table.insert(raat.session.message_handlers, on_message)

return protocol


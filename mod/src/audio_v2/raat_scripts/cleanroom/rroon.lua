-- Rune clean-room implementation of the RAAT device-side session protocol.
-- Written from scratch against the publicly documented RAAT behaviour and
-- the C API surface that the RAAT server exposes to Lua; no Roon source was
-- consulted during authoring beyond behaviour/API facts (docs/CLEAN_ROOM.md).
--
-- Implements the JSON request handlers the control channel expects:
-- setup / stream / start / stop / end_stream / teardown / sync / info and
-- the audio TCP + clock UDP servers they drive.

local rbase = require("rbase")
local rjson = require("rjson")
local rprotocol = require("rprotocol")

local roon = {}

----------------------------------------------------------------------
-- session state
----------------------------------------------------------------------

local sess = nil -- active session, nil between teardown and next setup

local clockSock, clockName, audioSock, audioName
local probeSeq = 1
local probeWaiters = {}

local CLOCK_MAGIC_QUERY   = "RAATCLKQ"
local CLOCK_MAGIC_REPLY   = "RAATCLKR"
local CLOCK_MAGIC_ERROR   = "RAATCLKE"
local PROBE_TICK_MS       = 5
local PROBE_MAX_TICKS     = 200
local PROBE_REQUIRED_OK   = 5
local PROBE_RTT_LIMIT_US  = 30000000

local function decodeMaybe(value)
  if type(value) == "string" then
    return rjson.decode(value) or value
  end
  return value
end

local function failResponse(err)
  local text = tostring(err)
  if text:find("DEVICE_IN_USE") then
    return { status = "DeviceInUse", message = text }
  elseif text:find("DEVICE_INIT_FAILED") then
    return { status = "DeviceInitFailed", message = text }
  elseif text:find("DEVICE_OPEN_FAILED") then
    return { status = "DeviceOpenFailed", message = text }
  end
  return { status = "UnexpectedError", message = text }
end

----------------------------------------------------------------------
-- audio TCP receiver (frame layout is fixed by the RAAT wire protocol)
----------------------------------------------------------------------

local ST_CMD, ST_HDR, ST_BODY = 1, 2, 3

local CMD_DATA, CMD_CHMAP, CMD_STREAMID, CMD_STREAMSAMPLE = 0, 1, 2, 3
local CMD_NORMALIZATION, CMD_CODEC_DATA = 4, 5

local CMD_HDR_LEN = { [CMD_DATA] = 8, [CMD_CHMAP] = 4, [CMD_STREAMID] = 4,
                      [CMD_STREAMSAMPLE] = 8, [CMD_NORMALIZATION] = 16,
                      [CMD_CODEC_DATA] = 8 }

local cli, st, cmd
local hdrBuf, hdrBufSize, hdrNeed, hdrFilled
local bodyBuf, bodyBufSize, bodyLeft, nSamples
local curStreamId, curChmap, curSample, curGain, curPeak

local function resetReceiver(client)
  if cli then
    cli:close()
    cli = nil
    collectgarbage("collect")
  end
  cli = client
  st = ST_CMD
  cmd = -1
  hdrBuf, hdrBufSize = buffer.new(16), 16
  hdrNeed, hdrFilled = 0, 0
  bodyBuf, bodyBufSize = buffer.new(16), 16
  bodyLeft, bodyFilled, nSamples = 0, 0, 0
  curStreamId, curChmap = -1, 0
  curSample, curGain, curPeak = 0, 0.0, 0.0
end

local function consumeHeader(chunk, off, avail)
  local take = math.min(avail, hdrNeed)
  hdrBuf:set_string(hdrFilled, chunk, off, take)
  hdrFilled = hdrFilled + take
  hdrNeed = hdrNeed - take
  if hdrNeed > 0 then
    return off + take, avail - take
  end
  -- header complete; interpret
  if cmd == CMD_DATA or cmd == CMD_CODEC_DATA then
    nSamples = hdrBuf:get_int32_be(0)
    local nbytes = hdrBuf:get_int32_be(4)
    if bodyBufSize < nbytes then
      bodyBuf, bodyBufSize = buffer.new(nbytes), nbytes
    end
    bodyLeft = nbytes
    st = ST_BODY
  elseif cmd == CMD_CHMAP then
    curChmap = hdrBuf:get_int32_be(0)
    st = ST_CMD
  elseif cmd == CMD_STREAMID then
    curStreamId = hdrBuf:get_int32_be(0)
    st = ST_CMD
  elseif cmd == CMD_STREAMSAMPLE then
    curSample = hdrBuf:get_int64_be(0)
    st = ST_CMD
  elseif cmd == CMD_NORMALIZATION then
    curGain = hdrBuf:get_float64_be(0)
    curPeak = hdrBuf:get_float64_be(8)
    st = ST_CMD
  end
  return off + take, avail - take
end

local function emitAudio()
  if not (sess and sess.stream) then
    return
  end
  if curStreamId == sess.streamId then
    -- for DSD formats nsamples counts bits per channel; the stream object
    -- knows the encoding from the format it was created with
    if stream.has_write_chmap then
      sess.stream:write_chmap(curSample, curGain, curPeak, bodyBuf, nSamples,
                              curChmap)
    else
      sess.stream:write(curSample, curGain, curPeak, bodyBuf, nSamples)
    end
  else
    raat.log.trace("dropping data for stale stream id")
  end
  if sess.state == "buffering" then
    sess.gotSamples = sess.gotSamples + nSamples
    local limit = sess.bufferSize * sess.readyThreshold
    if sess.gotSamples > limit then
      sess.state = "ready"
      if sess.streamCx then
        sess.streamCx:respond_nonfinal({ status = "Ready" })
      end
    end
  end
  curSample = curSample + nSamples
end

local bodyFilled = 0

local function feed(chunk, off, avail)
  while avail > 0 do
    if st == ST_CMD then
      hdrBuf:set_string(0, chunk, off, 1)
      cmd = hdrBuf:get_uint8(0)
      local need = CMD_HDR_LEN[cmd]
      if need == nil then
        raat.log.warning("unknown audio command " .. tostring(cmd))
        return false
      end
      hdrNeed, hdrFilled = need, 0
      st = ST_HDR
      off, avail = off + 1, avail - 1
    elseif st == ST_HDR then
      local take = math.min(avail, hdrNeed)
      hdrBuf:set_string(hdrFilled, chunk, off, take)
      hdrFilled, hdrNeed = hdrFilled + take, hdrNeed - take
      off, avail = off + take, avail - take
      if hdrNeed == 0 then
        if cmd == CMD_DATA or cmd == CMD_CODEC_DATA then
          nSamples = hdrBuf:get_int32_be(0)
          local nbytes = hdrBuf:get_int32_be(4)
          if bodyBufSize < nbytes then
            bodyBuf, bodyBufSize = buffer.new(nbytes), nbytes
          end
          bodyLeft, bodyFilled = nbytes, 0
          st = ST_BODY
        elseif cmd == CMD_CHMAP then
          curChmap = hdrBuf:get_int32_be(0)
          st = ST_CMD
        elseif cmd == CMD_STREAMID then
          curStreamId = hdrBuf:get_int32_be(0)
          st = ST_CMD
        elseif cmd == CMD_STREAMSAMPLE then
          curSample = hdrBuf:get_int64_be(0)
          st = ST_CMD
        elseif cmd == CMD_NORMALIZATION then
          curGain = hdrBuf:get_float64_be(0)
          curPeak = hdrBuf:get_float64_be(8)
          st = ST_CMD
        end
      end
    elseif st == ST_BODY then
      local take = math.min(avail, bodyLeft)
      bodyBuf:set_string(bodyFilled, chunk, off, take)
      bodyFilled, bodyLeft = bodyFilled + take, bodyLeft - take
      off, avail = off + take, avail - take
      if bodyLeft == 0 then
        emitAudio()
        st = ST_CMD
      end
    else
      return false
    end
  end
  return true
end

local function onClockPacketAlloc(err, data, srcAddr)
  if err or not data then return end
  onClockPacketNoAlloc(err, data, #data, srcAddr)
end

local function onClientData(chunk, len)
  if not cli then
    return
  end
  if (not sess) or sess.state == nil or not sess.stream then
    resetReceiver(nil)
    return
  end
  if not feed(chunk, 0, len) then
    resetReceiver(nil)
  end
end

local function onAccept(err)
  if err then return end
  local client = uv.new_tcp()
  audioSock:accept(client)
  if cli then
    resetReceiver(client) -- last client wins; drop the previous connection
  else
    resetReceiver(client)
  end
  if uv.read_start_noalloc then
    client:read_start_noalloc(function(readErr, chunk, len)
      if readErr or not chunk then return end
      onClientData(chunk, len)
    end)
  else
    client:read_start(function(readErr, chunk)
      if readErr or not chunk then return end
      onClientData(chunk, #chunk)
    end)
  end
end

----------------------------------------------------------------------
-- clock UDP server + master-clock probing
----------------------------------------------------------------------

local clockRbuf = buffer.new(512)

local function replyClock(srcAddr, rid)
  if not (sess and sess.token) or sess.state == nil then
    clockRbuf:set_string(0, CLOCK_MAGIC_ERROR)
    clockRbuf:set_int32_be(8, rid)
    uv.udp_send(clockSock, clockRbuf:get_string(0, 12), srcAddr.ip, srcAddr.port,
                function() end)
    return
  end
  local delay = 0
  if raat.output.is_get_output_delay_supported then
    local ok, value = pcall(raat.output.get_output_delay, sess.token)
    if ok and value ~= nil then
      delay = value
    end
  end
  local ok, now = pcall(raat.output.get_local_time, sess.token)
  if not ok or now == nil then
    clockRbuf:set_string(0, CLOCK_MAGIC_ERROR)
    clockRbuf:set_int32_be(8, rid)
    uv.udp_send(clockSock, clockRbuf:get_string(0, 12), srcAddr.ip, srcAddr.port,
                function() end)
    return
  end
  clockRbuf:set_string(0, CLOCK_MAGIC_REPLY)
  clockRbuf:set_int32_be(8, rid)
  clockRbuf:set_int64_be(12, now)
  clockRbuf:set_int64_be(20, delay)
  uv.udp_send(clockSock, clockRbuf:get_string(0, 28), srcAddr.ip, srcAddr.port,
              function() end)
end

local function onClockPacketNoAlloc(err, data, ndata, srcAddr)
  if err or not data then return end
  clockRbuf:set_string(0, data, 0, math.min(512, ndata))
  local magic = clockRbuf:get_string(0, 8)
  if magic == CLOCK_MAGIC_QUERY then
    replyClock(srcAddr, clockRbuf:get_int32_be(8))
  elseif magic == CLOCK_MAGIC_REPLY then
    local rid = clockRbuf:get_int32_be(8)
    local remoteTime = clockRbuf:get_int64_be(12)
    local waiter = probeWaiters[rid]
    if waiter then
      probeWaiters[rid] = nil
      waiter(true, remoteTime)
    end
  elseif magic == CLOCK_MAGIC_ERROR then
    local rid = clockRbuf:get_int32_be(8)
    local waiter = probeWaiters[rid]
    if waiter then
      probeWaiters[rid] = nil
      waiter(false, 0)
    end
  end
end

----------------------------------------------------------------------
-- request handlers
----------------------------------------------------------------------

local function cleanupStream()
  if not sess then return end
  sess.stream = nil
  if sess.streamCx then
    sess.streamCx:respond({ status = "Ended" })
    sess.streamCx = nil
  end
end

local function reqEcho(cx, req)
  cx:respond({ status = "Success" })
end

local function compressFormats(formats)
  if type(formats) == "string" then
    return formats
  end
  local order, groups = {}, {}
  for _, f in ipairs(formats or {}) do
    local key = tostring(f.sample_rate) .. "/" .. tostring(f.bits_per_sample)
    if not groups[key] then
      groups[key] = {}
      order[#order + 1] = key
    end
    local found = false
    for _, c in ipairs(groups[key]) do
      if c == tostring(f.channels) then found = true break end
    end
    if not found then
      groups[key][#groups[key] + 1] = tostring(f.channels)
    end
  end
  local out = {}
  for _, key in ipairs(order) do
    out[#out + 1] = key .. "/" .. table.concat(groups[key], "|")
  end
  return table.concat(out, ",")
end

local function reqInfo(cx, req)
  cx:respond({
    status = "Success",
    info = raat.info,
    platform = raat.platform,
    output = {
      supported_formats = compressFormats(raat.output.supported_formats),
      info = decodeMaybe(raat.output.info),
    },
    transport = {
      is_supported = raat.transport.is_supported,
      is_update_status_supported = raat.transport.is_update_status_supported,
      is_update_artwork_supported = raat.transport.is_update_artwork_supported,
      info = decodeMaybe(raat.transport.info),
    },
    volume = {
      is_supported = raat.volume.is_supported,
      info = decodeMaybe(raat.volume.info),
    },
    source_selection = {
      is_supported = raat.source_selection.is_supported,
      info = decodeMaybe(raat.source_selection.info),
    },
    flags = { has_write_chmap = stream.has_write_chmap },
    codecs = raat.codecs,
  })
end

local subsControls, subsTransport, subsApp = {}, {}, {}

local function reqGetControls(cx, req)
  local controls = {}
  if raat.volume.is_supported then
    controls[#controls + 1] = { type = "volume" }
  end
  if #controls == 0 then
    cx:respond({ status = "NotSupported" })
  else
    cx:respond({ status = "Success", controls = controls })
  end
end

local function reqSubscribe(cx, req, registry)
  if not (raat.volume.is_supported or raat.transport.is_supported) then
    cx:respond({ status = "NotSupported" })
    return
  end
  rbase.subscribe(registry, req, cx)
  cx:respond({ status = "Success" })
end

local function reqUnsubscribe(cx, req, registry)
  rbase.unsubscribe(registry, req, cx)
end

local function reqSetup(cx, req)
  if not raat.output.is_supported then
    cx:respond({ status = "NotSupported" })
    return
  end
  local format = req.format
  if type(format) ~= "table" or not format.sample_rate then
    cx:respond({ status = "InvalidArgument", message = "missing format" })
    return
  end
  if sess then
    cleanupStream()
    if sess.token then
      pcall(raat.output.teardown, sess.token)
    end
    sess = nil
  end
  local mine = { state = nil, format = format, setupCx = cx }
  sess = mine
  -- watchdog: if the C side never calls on_setup, answer with a loud,
  -- diagnosable failure instead of an upstream timeout with zero clues
  local watchdog = uv.new_timer()
  if watchdog then
    watchdog:start(3000, 0, function()
      if watchdog then
        watchdog:stop()
        watchdog:close()
        watchdog = nil
      end
      if sess == mine and not mine.token then
        raat.log.warning("output setup never completed")
        cx:respond({ status = "Error",
                     message = "on_setup never invoked by output layer (3s watchdog)" })
        sess = nil
      end
    end)
  end
  local ok, err = pcall(raat.output.setup, format, {
    on_setup = function(success, tokenOrError)
      if watchdog then
        watchdog:stop()
        watchdog:close()
        watchdog = nil
      end
      if sess ~= mine then return end
      if success then
        mine.token = tokenOrError
        mine.state = "setup"
        cx:respond_nonfinal({
          status = "Success",
          clock_port = clockName and clockName.port or 0,
          audio_port = 0,
          audio_port_tcp = audioName and audioName.port or 0,
        })
      else
        raat.log.warning("output setup failed: " .. tostring(tokenOrError))
        cx:respond(failResponse(tokenOrError))
        if sess == mine then sess = nil end
      end
    end,
    on_output_lost = function(reason)
      cx:respond({ status = "Lost", reason = decodeMaybe(reason) })
      if sess == mine then sess = nil end
    end,
    -- some output layers expect all four callbacks to be present; a missing
    -- key must never be the reason setup stalls silently
    on_volume_change = function(volume, muted)
      if volume ~= nil and mine.volumeHooks then
        mine.volumeHooks(volume, muted)
      end
    end,
    on_mute_change = function(muted)
      if muted ~= nil and mine.muteHooks then
        mine.muteHooks(muted)
      end
    end,
  })
  if not ok then
    if watchdog then
      watchdog:stop()
      watchdog:close()
      watchdog = nil
    end
    raat.log.error("output.setup call raised: " .. tostring(err))
    cx:respond({ status = "Error",
                 message = "output.setup raised: " .. tostring(err) })
    if sess == mine then sess = nil end
  end
end

local function reqStream(cx, req)
  if not raat.output.is_supported then
    cx:respond({ status = "NotSupported" })
    return
  end
  -- Roon re-opens a stream on the SAME session after stop for track
  -- changes (real RAAT devices allow it), and reqStart below already
  -- accepts "stopped" for the same reason; end_stream keeps the stream
  -- cleared but leaves state "stopped" — accept both here.
  if not sess or (sess.state ~= "setup" and sess.state ~= "stopped") then
    cx:respond({ status = "InvalidState" })
    return
  end
  if not tonumber(req.first_seq) then
    cx:respond({ status = "InvalidArgument",
                 message = "missing or invalid 'first_seq' argument" })
    return
  end
  if not tonumber(req.stream_id) then
    cx:respond({ status = "InvalidArgument",
                 message = "missing or invalid 'stream_id' argument" })
    return
  end
  cleanupStream()
  local format = sess.format
  sess.bufferSize = (tonumber(req.buffer_size) or 1) * format.sample_rate
  sess.readyThreshold = tonumber(req.ready_threshold) or 0.5
  sess.streamId = tonumber(req.stream_id)
  sess.stream = stream.new(format, 2 * sess.bufferSize)
  sess.gotSamples = 0
  sess.state = "buffering"
  sess.streamCx = cx
  cx:respond_nonfinal({ status = "Buffering" })
end

local function reqStart(cx, req)
  if not raat.output.is_supported then
    cx:respond({ status = "NotSupported" })
    return
  end
  if not sess or (sess.state ~= "buffering" and sess.state ~= "ready"
                  and sess.state ~= "stopped") then
    cx:respond({ status = "InvalidState" })
    return
  end
  local sample = tonumber(req.stream_sample)
  if not sample then
    cx:respond({ status = "InvalidArgument",
                 message = "invalid or missing 'stream_sample' argument" })
    return
  end
  local when = tonumber(req.time)
  if not when then
    when = raat.output.get_local_time(sess.token)
           + (tonumber(req.min_offset) or 300000)
    raat.log.trace("ASAP start from device clock")
  end
  local ok, err = pcall(raat.output.start, sess.token, when, sample,
                        sess.stream)
  if ok then
    sess.state = "playing"
    if sess.streamCx then
      sess.streamCx:respond_nonfinal({ status = "Playing" })
    end
    cx:respond({ status = "Success", time = when })
  else
    cx:respond({ status = "UnexpectedError", message = tostring(err) })
  end
end

local function reqStop(cx, req)
  if not raat.output.is_supported then
    cx:respond({ status = "NotSupported" })
    return
  end
  if not sess or (sess.state ~= "playing" and sess.state ~= "buffering"
                  and sess.state ~= "ready") then
    cx:respond({ status = "InvalidState" })
    return
  end
  local ok, err = pcall(raat.output.stop, sess.token)
  if not ok then
    cx:respond({ status = "UnexpectedError", message = tostring(err) })
    return
  end
  sess.state = "stopped"
  if sess.streamCx then
    sess.streamCx:respond_nonfinal({ status = "Stopped" })
  end
  cx:respond({ status = "Success" })
end

local function reqEndStream(cx, req)
  cleanupStream()
  cx:respond({ status = "Success" })
end

local function reqTeardown(cx, req)
  if not raat.output.is_supported then
    cx:respond({ status = "NotSupported" })
    return
  end
  if not sess then
    cx:respond({ status = "InvalidState" })
    return
  end
  cleanupStream()
  if sess.token then
    local ok, err = pcall(raat.output.teardown, sess.token)
    if not ok then
      raat.log.warning("teardown failed: " .. tostring(err))
    end
  end
  if sess.setupCx then
    sess.setupCx:respond({ status = "Teardown" })
  end
  sess = nil
  cx:respond({ status = "Success" })
end

local function reqSync(cx, req)
  if not raat.output.is_supported then
    cx:respond({ status = "NotSupported" })
    return
  end
  if not sess or not sess.token then
    cx:respond({ status = "InvalidState" })
    return
  end
  local host, port = req.host, tonumber(req.port)
  if not host or not port then
    cx:respond({ status = "InvalidArgument",
                 message = "sync requires host and port" })
    return
  end
  local timer = uv.new_timer()
  local ticks = PROBE_MAX_TICKS
  local okCount, bestRtt, bestOffset = 0, nil, 0
  local done = false

  local function finish(body)
    if done then return end
    done = true
    timer:stop()
    timer:close()
    cx:respond(body)
  end

  timer:start(0, PROBE_TICK_MS, function()
    if done then return end
    if not (sess and sess.token) then
      finish({ status = "InvalidState" })
      return
    end
    ticks = ticks - 1
    if ticks <= 0 then
      finish({ status = "Timeout" })
      return
    end
    local rid = probeSeq
    probeSeq = probeSeq + 1
    local packet = buffer.new(12)
    packet:set_string(0, CLOCK_MAGIC_QUERY)
    packet:set_int32_be(8, rid)
    local sentAt = raat.output.get_local_time(sess.token)
    probeWaiters[rid] = function(good, remoteTime)
      if done then return end
      if not good then return end
      local receivedAt = raat.output.get_local_time(sess.token)
      local rtt = receivedAt - sentAt
      local offset = sentAt + math.floor(rtt / 2) - remoteTime
      if bestRtt == nil or rtt < bestRtt then
        bestRtt, bestOffset = rtt, offset
      end
      okCount = okCount + 1
      if okCount >= PROBE_REQUIRED_OK then
        if bestRtt < PROBE_RTT_LIMIT_US then
          raat.output.set_remote_time(sess.token, bestOffset,
                                      req.new_source == true)
          raat.log.trace(("synced: offset %.3fms rtt %.3fms")
            :format(bestOffset / 1e6, bestRtt / 1e6))
          finish({ status = "Success" })
        else
          raat.log.warning("clock probe RTT too high; not feeding endpoint")
          finish({ status = "Success" })
        end
      end
    end
    uv.udp_send(clockSock, packet:get_string(0, 12), host, port,
                function() end)
  end)
end

local function reqSetClientType(cx, req)
  raat.session.set_client_type(req.client_type)
  cx:respond({ status = "Success" })
end

local function reqRefreshFormats(cx, req)
  if raat.output.is_refresh_supported_formats_supported then
    raat.output.refresh_supported_formats()
  end
  cx:respond({ status = "Success" })
end

local handlers = {
  echo          = reqEcho,
  info          = reqInfo,
  get_controls  = reqGetControls,
  subscribe_controls    = function(cx, r) reqSubscribe(cx, r, subsControls) end,
  unsubscribe_controls  = function(cx, r) reqUnsubscribe(cx, r, subsControls) end,
  subscribe_transport   = function(cx, r) reqSubscribe(cx, r, subsTransport) end,
  unsubscribe_transport = function(cx, r) reqUnsubscribe(cx, r, subsTransport) end,
  subscribe_app_channel    = function(cx, r) reqSubscribe(cx, r, subsApp) end,
  unsubscribe_app_channel  = function(cx, r) reqUnsubscribe(cx, r, subsApp) end,
  setup         = reqSetup,
  stream        = reqStream,
  start         = reqStart,
  stop          = reqStop,
  end_stream    = reqEndStream,
  teardown      = reqTeardown,
  sync          = reqSync,
  set_client_type = reqSetClientType,
  refresh_supported_formats = reqRefreshFormats,
}

rprotocol.onRequest = function(cx, req)
  local handler = handlers[req.request]
  if handler then
    handler(cx, req)
  else
    cx:respond({ status = "NotSupported" })
  end
end

----------------------------------------------------------------------
-- startup
----------------------------------------------------------------------

local function bindInRange(sock, isUdp)
  if raat.port_range then
    local port = raat.port_range.start_port
    local last = raat.port_range.end_port
    while port < last do
      local rc = sock:bind("0.0.0.0", port)
      if rc == 0 then
        return true
      end
      port = port + 1
    end
  end
  return false
end

local function go()
  clockSock = uv.new_udp()
  audioSock = uv.new_tcp()

  if not bindInRange(clockSock, true) then
    clockSock:bind("0.0.0.0", 0)
  end
  clockName = uv.udp_getsockname(clockSock)

  if not bindInRange(audioSock, false) then
    audioSock:bind("0.0.0.0", 0)
  end
  audioName = uv.tcp_getsockname(audioSock)

  if uv.udp_recv_start_noalloc then
    uv.udp_recv_start_noalloc(clockSock, onClockPacketNoAlloc)
  else
    uv.udp_recv_start(clockSock, onClockPacketAlloc)
  end

  audioSock:listen(10, onAccept)

  raat.log.trace("rune roon protocol ready: clock udp "
    .. rbase.addrToString(clockName) .. ", audio tcp "
    .. rbase.addrToString(audioName))
end

table.insert(raat.session.shutdown_handlers, function()
  if clockSock then clockSock:close() end
  if audioSock then audioSock:close() end
  if cli then cli:close() end
end)

go()

return roon

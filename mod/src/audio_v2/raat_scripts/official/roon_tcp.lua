local base     = require("base")
local protocol = require("protocol")
local json     = require("dkjson")

local roon = { }

local clock_sock
local bus_sock  
local audio_sock_tcp

local controller_id     = nil
local owner_id          = nil -- which controller_id "owns" this device right now for the purpose of transport/frontpanel
local last_status       = nil -- last status msg for this controller
local last_artwork_blob = nil -- last artwork blob for this controller
local last_artwork_mime = nil -- last artwork mime for this controller


local clock_endpoint
local audio_endpoint_tcp 
local controls_subscriptions = { }
local transport_subscriptions = { }
local app_channel_subscriptions = { }

local IDLE      = "IDLE"
local SETUP     = "SETUP"
local BUFFERING = "BUFFERING"
local READY     = "READY"
local PLAYING   = "PLAYING"
local STOPPED   = "STOPPED"

local CLOCK_SYNC_ITERATIONS = 5

-- the current playback info
-- { 
--      stream             = stream
--      token              = opaque
--      setup_cx           = request context
--      stream_cx          = request context
--      stream_dropouts    = nsamples
--      format             = { ... }
--      state              = IDLE | SETUP | BUFFERING | PLAYING | STOPPED | READY
--      stream_id          = number
--      start_time         = number
--      start_stream_sample = number
--      stats_timer        = uv timer
--      got_samples        = number
-- }
--
local current                    = nil 
local pending_clocksync_requests = { next_rid = 1 }

function validate_format(format) 
    local sample_type = format.sample_type
    if sample_type ~= "pcm" and sample_type ~= "dsd" and sample_type ~= "pcm_float64" and sample_type ~= "pcm_float32" then 
        raat.log.warning("invalid sample_type: '" .. sample_type .. "'")
        return nil 
    end

    local sample_rate = tonumber(format.sample_rate)
    if not sample_rate then 
        raat.log.warning("invalid sample_rate")
        return nil 
    end

    local bits_per_sample = tonumber(format.bits_per_sample)
    if not bits_per_sample then 
        raat.log.warning("invalid bits_per_sample")
        return nil 
    end

    local channels = tonumber(format.channels)
    if not channels then 
        raat.log.warning("invalid channels")
        return nil 
    end

    return { 
        sample_type     = sample_type,
        sample_rate     = sample_rate,
        bits_per_sample = bits_per_sample, 
        channels        = channels, 
    }
end

function count(t)
    local ret = 0
    for k,v in pairs(t) do
        ret = ret + 1
    end
    return ret
end

local stats_table = {}
function on_stats_timer()
    if current and current.stream then
        local stats = current.stream:get_stats(stats_table)
        --raat.log.debug(json.encode(stats) .. " stream dropouts " .. tostring(current.stream_dropouts))
        if current.stream_dropouts ~= nil and stats.samples_dropped > current.stream_dropouts then
            local dropout = stats.samples_dropped - current.stream_dropouts
            current.stream_cx:respond_nonfinal({ status = "Dropout", samples = dropout})
        end
        current.stream_dropouts = stats.samples_dropped
    end
end

function check_ready() 
    local threshold = current.buffer_size * current.ready_threshold
    if  current.got_samples > threshold and current.state == BUFFERING then
        current.state = READY
        current.stream_cx:respond_nonfinal({ status = "Ready" })
    end
end

local clock_packet_builder = buffer.new(28)
function on_clock_query(src_addr, rid) 
    if not current then return end
        
    local time = nil
    local delay = nil
    status,err = pcall(function() 
        if raat.output.is_get_output_delay_supported and current and current.token ~= nil then
            delay = raat.output.get_output_delay(current.token)
        end

        if delay == nil then
            delay = 0
        end

        --raat.log.trace("[roon] got clock sync query from " .. addr_tostring(src_addr) .. " rid=" .. tostring(rid))
        if current == nil or current.state == IDLE then error("invalid state") end
        time = raat.output.get_local_time(current.token)
    end)

    if status then
        clock_packet_builder:set_string(0, "RAATCLKR")
        clock_packet_builder:set_int32_be(8, rid)
        clock_packet_builder:set_int64_be(12, time)
        clock_packet_builder:set_int64_be(20, delay)
        uv.udp_send(clock_sock, clock_packet_builder:get_string(0, 28), src_addr.ip, src_addr.port, function(...) end)
        --raat.log.trace("[roon] sending clock sync success response time=" .. tostring(time) .. " to " .. addr_tostring(src_addr))
    else 
        --raat.log.trace("[roon] sending clock sync error response: " .. tostring(err))
        clock_packet_builder:set_string(0, "RAATCLKE")
        clock_packet_builder:set_int32_be(8, rid)
        uv.udp_send(clock_sock, clock_packet_builder:get_string(0, 12), src_addr.ip, src_addr.port, function(...) end)
    end
end

function on_clock_response(src_addr, rid, time) 
    --raat.log.trace("[roon] got clock sync response from " .. addr_tostring(src_addr) .. " rid=" .. tostring(rid) .. " time=" .. time)
    local fn = pending_clocksync_requests[rid]
    if fn then
        pending_clocksync_requests[rid] = nil
        fn(true, time)
    end
end

function on_clock_error(src_addr, rid)
    --raat.log.trace("[roon] got clock sync error from " .. addr_tostring(src_addr) .. " rid=" .. tostring(rid))
    local fn = pending_clocksync_requests[rid]
    if fn then
        pending_clocksync_requests[rid] = nil
        fn(false, 0)
    end
end

function on_clock_recv(err, data, src_addr, flags)
    if not data then return end
    on_clock_recv_noalloc(err, data, #data, src_addr, flags)
end

local clock_recv_packet = buffer.new(512)
function on_clock_recv_noalloc(err, data, ndata, src_addr, flags)
    if not current then return end
    if ndata < 12 then return end

    --raat.log.trace("in on_clock_recv_noalloc")
    status,err = pcall(function()
        if (current.state == SETUP) then raat.log.trace("[roon] got packet on clock port") end
        if err then
            raat.log.warning("[roon] got error in on_clock_recv:", err)
            return
        end

        if not data then return end

        clock_recv_packet:set_string(0, data, 0, math.min(512, ndata))
        local hdr = clock_recv_packet:get_string(0, 8)
        if hdr == "RAATCLKQ" then
            rid  = clock_recv_packet:get_int32_be(8)
            on_clock_query(src_addr, rid)
        elseif hdr == "RAATCLKR" then
            rid  = clock_recv_packet:get_int32_be(8)
            time = clock_recv_packet:get_int64_be(12)
            on_clock_response(src_addr, rid, time)
        elseif hdr == "RAATCLKE" then
            rid  = clock_recv_packet:get_int32_be(8)
            on_clock_error(src_addr, rid)
        else
            local hex_str = ""
            for i=0,math.min(512, ndata) do
                hex_str = hex_str .. string.format('%02X-', clock_recv_packet:get_uint8(i))
            end
            raat.log.warning("[roon] ignoring invalid packet on clock port, packet: " .. clock_recv_packet:get_string(0, math.min(512, ndata)))
            raat.log.warning("        data: " .. hex_str)
        end
    end)
    if not status then
        raat.log.error("Error in on_clock_recv: ",err)
    end
    --raat.log.trace("out on_clock_recv_noalloc")
end

local audiobuf_len    = 1
local audiobuf        = buffer.new(1)
local headerbuf       = buffer.new(48)
local has_write_chmap = stream.has_write_chmap

-- state related to TCP audio receiver
local TCP_READ_STATE_COMMAND                    = 0
local TCP_READ_STATE_HEADER                     = 1
local TCP_READ_STATE_AUDIO                      = 2

local COMMAND_DATA                              = 0
local COMMAND_CHMAP                             = 1
local COMMAND_STREAMID                          = 2
local COMMAND_STREAMSAMPLE                      = 3
local COMMAND_NORMALIZATION                     = 4
local COMMAND_CODEC_DATA                        = 5

local tcp_audio_client                          = nil
local tcp_read_state                            = -1

local tcp_streamid                              = -1;
local tcp_gain                                  = 0.0
local tcp_peak                                  = 0.0
local tcp_chmap                                 = 0
local tcp_streamsample                          = 0

local tcp_cmd                                   = -1
local tcp_audio_nsamples                        = 0
local tcp_audio_nbytes                          = 0
local tcp_audio_count                           = 0
local tcp_audio_left                            = 0
local tcp_audio_buf                             = buffer.new(16)
local tcp_audio_buf_size                        = 0

local tcp_header_count                          = 0
local tcp_header_left                           = 0
local tcp_header_buf                            = buffer.new(16)       

function tcp_parse(chunk, off, left)
    while left > 0 do
        --raat.log.trace("    off="..tostring(off).." left="..tostring(left).." state=" .. tcp_read_state .. " cmd=" .. tcp_cmd)
        if     tcp_read_state == TCP_READ_STATE_COMMAND then
            tcp_header_buf:set_string(0, chunk, off, 1)
            tcp_cmd = tcp_header_buf:get_uint8(0)

            if     tcp_cmd == COMMAND_DATA          then
                tcp_header_count = 8; tcp_header_left = 8
            elseif tcp_cmd == COMMAND_CHMAP         then
                tcp_header_count = 4; tcp_header_left = 4
            elseif tcp_cmd == COMMAND_STREAMID      then
                tcp_header_count = 4; tcp_header_left = 4
            elseif tcp_cmd == COMMAND_STREAMSAMPLE  then
                tcp_header_count = 8; tcp_header_left = 8
            elseif tcp_cmd == COMMAND_NORMALIZATION then
                tcp_header_count = 16; tcp_header_left = 16
            elseif tcp_cmd == COMMAND_CODEC_DATA then
                tcp_header_count = 8; tcp_header_left = 8
            else
                raat.log.warning("UNKNOWN COMMAND "..tostring(tcp_cmd))
                return false
            end

            tcp_read_state = TCP_READ_STATE_HEADER
            off  = off  + 1; left = left - 1

        elseif tcp_read_state == TCP_READ_STATE_AUDIO  then
            local tocopy = math.min(left, tcp_audio_left)
            if tocopy > 0 then
                tcp_audio_buf:set_string(tcp_audio_count - tcp_audio_left, chunk, off, tocopy)
            end
            tcp_audio_left = tcp_audio_left - tocopy

            off  = off + tocopy; left = left - tocopy

            if tcp_audio_left == 0 then
                if tcp_streamid == current.stream_id then
                    if current.codec then
                        current.codec:decode_to_stream(current.stream, tcp_streamsample, tcp_gain, tcp_peak, tcp_audio_buf, tcp_audio_nsamples)
                    elseif has_write_chmap then
                        current.stream:write_chmap(tcp_streamsample, tcp_gain, tcp_peak, tcp_audio_buf, tcp_audio_nsamples, tcp_chmap)
                    else
                        current.stream:write(tcp_streamsample, tcp_gain, tcp_peak, tcp_audio_buf, tcp_audio_nsamples)
                    end
                else
                    raat.log.trace("ignoring packet for old stream")
                end
                if current.state == BUFFERING then
                    current.got_samples = tcp_audio_nsamples + current.got_samples
                    check_ready()
                end
                --raat.log.trace("got streamsample " .. tcp_streamsample)
                tcp_streamsample = tcp_streamsample + tcp_audio_nsamples
                tcp_read_state   = TCP_READ_STATE_COMMAND
            end

        elseif tcp_read_state == TCP_READ_STATE_HEADER then 
            local tocopy = math.min(left, tcp_header_left)
            tcp_header_buf:set_string(tcp_header_count - tcp_header_left, chunk, off, tocopy)
            tcp_header_left = tcp_header_left - tocopy

            off  = off + tocopy; left = left - tocopy

            if tcp_header_left == 0 then
                if tcp_cmd == COMMAND_DATA or tcp_cmd == COMMAND_CODEC_DATA then
                    tcp_audio_nsamples        = tcp_header_buf:get_int32_be(0)
                    tcp_audio_count           = tcp_header_buf:get_int32_be(4)
                    --raat.log.trace("GOT COMMAND_DATA "..tostring(tcp_audio_count).." SAMPLES "..tostring(tcp_audio_nsamples))
                    if tcp_audio_buf_size ~= tcp_audio_count then
                        tcp_audio_buf        = buffer.new(tcp_audio_count)
                        tcp_audio_buf_size   = tcp_audio_count
                    end
                    tcp_audio_left = tcp_audio_count
                    tcp_read_state = TCP_READ_STATE_AUDIO

                elseif tcp_cmd == COMMAND_CHMAP then
                    tcp_chmap      = tcp_header_buf:get_int32_be(0)
                    tcp_read_state = TCP_READ_STATE_COMMAND

                elseif tcp_cmd == COMMAND_STREAMID then
                    tcp_streamid   = tcp_header_buf:get_int32_be(0)
                    tcp_read_state = TCP_READ_STATE_COMMAND

                elseif tcp_cmd == COMMAND_STREAMSAMPLE then
                    tcp_streamsample = tcp_header_buf:get_int64_be(0)
                    tcp_read_state   = TCP_READ_STATE_COMMAND

                elseif tcp_cmd == COMMAND_NORMALIZATION then
                    tcp_gain = tcp_header_buf:get_float64_be(0)
                    tcp_peak = tcp_header_buf:get_float64_be(8)
                    tcp_read_state = TCP_READ_STATE_COMMAND
                end
            end
        else
            raat.log.error("invalid state: " .. tcp_read_state)
            return false
        end
    end
    return true
end

function close_tcp_audio_client()
    if tcp_audio_client then
        tcp_audio_client:close()
        tcp_audio_client = nil
        tcp_audio_buf = buffer.new(16)
        collectgarbage("collect")
    end
end

function on_new_client(client)
    close_tcp_audio_client()
    tcp_audio_client    = client
    tcp_read_state      = TCP_READ_STATE_COMMAND
    tcp_streamid        = -1;
    tcp_gain            = 0.0
    tcp_peak            = 0.0
    tcp_chmap           = 0
    tcp_streamsample    = 0
    tcp_cmd             = -1
    tcp_audio_nsamples  = 0
    tcp_audio_nbytes    = 0
    tcp_audio_count     = 0
    tcp_audio_left      = 0
    tcp_audio_buf       = buffer.new(16)
    tcp_audio_buf_size  = 0
    tcp_header_count    = 0
    tcp_header_left     = 0
    tcp_header_buf      = buffer.new(16)       
end

function on_audio_sock_tcp_accept(err) 
    if err then return end
    local client = uv.new_tcp()
    audio_sock_tcp:accept(client)

    if uv.read_start_noalloc then
        client:read_start_noalloc(function (err, chunk, len)
            if err then return end
            if (chunk) then 
                if tcp_audio_client ~= client then 
                    on_new_client(client) 
                end
                if (not current) or (current.state == IDLE) or (not current.stream) then 
                    close_tcp_audio_client()
                else
                    if not tcp_parse(chunk, 0, len) then 
                        close_tcp_audio_client()
                    end
                end
            end
        end)
    else
        client:read_start(function (err, chunk)
            if err then return end
            if (chunk) then 
                if tcp_audio_client ~= client then 
                    on_new_client(client) 
                end
                if (not current) or (current.state == IDLE) or (not current.stream) then 
                    close_tcp_audio_client()
                else
                    if not tcp_parse(chunk, 0, #chunk) then 
                        close_tcp_audio_client()
                    end
                end
            end
        end)
    end
end

local BUS_PORT = 9004
local BUS_IP   = "239.255.90.91"

function bus_init() 
    if not raat.transport.is_supported then return end
    raat.log.trace("init bus")
    bus_sock = uv.new_udp()
    bus_sock:bind("0.0.0.0", BUS_PORT, { reuseaddr = true })
    bus_sock:set_membership(BUS_IP, "0.0.0.0", "join")
    bus_sock:set_multicast_ttl(0)
        uv.udp_recv_start(bus_sock, on_bus_recv)
end

function bus_send(data)
    if not raat.transport.is_supported then return end
    if not bus_sock then return end
    bus_sock:send(data, BUS_IP, BUS_PORT, function() end)
end

function bus_shutdown() 
    if not raat.transport.is_supported then return end
    raat.log.trace("shutdown bus")
    if not bus_sock then return end
    bus_sock:close()
end

function on_bus_recv(err, data, src_addr, flags)
    if not data then return end
    status,err = pcall(function() 
        local j = json.decode(data)
        if not j then return end 

        if j.command == "query"  then
            bus_send(json.encode({
                command        = "status",
                owner_id = owner_id
            }))
        elseif j.command == "status" then 
            owner_id = j.owner_id
        end
    end)
    if not status then
        raat.log.warning("failed in on_bus_recv:", err)
    end
end

function take_control_ownership()
    if not raat.transport.is_supported then return end
    if not controller_id then return end
    owner_id = controller_id
    bus_send(json.encode({
        command  = "status",
        owner_id = controller_id
    }))

    if last_artwork_blob and last_artwork_mime then
        raat.transport.update_artwork(last_artwork_mime, last_artwork_blob)
    else 
        raat.transport.update_artwork(nil)
    end

    if last_status then
        raat.transport.update_status(json.encode(last_status))
    else
        raat.transport.update_status(json.encode({
            loop                  = "disabled",
            shuffle               = false,
            state                 = "stopped",
            is_previous_allowed   = false,        
            is_next_allowed       = false,        
            is_play_allowed       = false,        
            is_pause_allowed      = false,        
            is_seek_allowed       = false,        
        }))
    end

end

function go() 
    local r

    clock_sock     = uv.new_udp()
    audio_sock_tcp = uv.new_tcp()

    bus_init()

    if raat.port_range then
        local clock_port = raat.port_range.start_port
        while clock_port < raat.port_range.end_port do
            local rc = clock_sock:bind("0.0.0.0", clock_port) 
            if 0 == rc then
                raat.log.trace("[roon] bound clock_port=" .. clock_port .. " rc=" .. rc);
                clock_endpoint = uv.udp_getsockname(clock_sock)
                break
            end
            raat.log.trace("[roon] couldn't bind clock_port=" .. clock_port);
            clock_port = clock_port + 1
            if clock_port == raat.port_range.end_port then
                raat.log.trace("[roon] giving up and binding 0 :(");
                clock_sock:bind("0.0.0.0", 0)
                clock_endpoint = uv.udp_getsockname(clock_sock)
            end
        end

        local audio_sock_port = raat.port_range.start_port
        while audio_sock_port < raat.port_range.end_port do
            local rc = audio_sock_tcp:bind("0.0.0.0", audio_sock_port)
            if 0 == rc then
                raat.log.trace("[roon] bound audio_sock_port=" .. audio_sock_port);
                audio_endpoint_tcp = { ip = "0.0.0.0", port = audio_sock_port }
                break
            end
            raat.log.trace("[roon] couldn't bind audio_sock_port=" .. audio_sock_port .. " rc=" .. rc);
            audio_sock_port = audio_sock_port + 1
            if audio_sock_port == raat.port_range.end_port then
                raat.log.trace("[roon] giving up and binding 0 :(");
                audio_sock_tcp:bind("0.0.0.0", 0)
                audio_endpoint_tcp = uv.tcp_getsockname(audio_sock_tcp)
            end
        end
    else
        clock_sock:bind("0.0.0.0", 0)
        audio_sock_tcp:bind("0.0.0.0", 0, { reuseaddr = true })
        clock_endpoint     = uv.udp_getsockname(clock_sock)
        audio_endpoint_tcp = uv.tcp_getsockname(audio_sock_tcp)
    end


    if (uv.udp_recv_start_noalloc) then
        uv.udp_recv_start_noalloc(clock_sock, on_clock_recv_noalloc)
    else
        uv.udp_recv_start(clock_sock, on_clock_recv)
    end

    raat.log.trace("[roon] clock socket listening on udp " .. addr_tostring(clock_endpoint))
    raat.log.trace("[roon] audio socket listening on tcp " .. addr_tostring(audio_endpoint_tcp))

    audio_sock_tcp:listen(10, on_audio_sock_tcp_accept)

    if raat.output.info then
        raat.log.trace("[roon] output info " .. raat.output.info)
    end
    if raat.volume.info then
        raat.log.trace("[roon] volume info " .. raat.volume.info)
    end
end

function shutdown() 
    cleanup_stream()
    if current and current.token then 
        raat.output.teardown(current.token)
    end

    if owner_id == controller_id then 
        if raat.transport.is_update_status_supported then
            raat.transport.update_status(json.encode({
                loop                  = "disabled",
                shuffle               = false,
                state                 = "stopped",
                is_previous_allowed   = false,        
                is_next_allowed       = false,        
                is_play_allowed       = false,        
                is_pause_allowed      = false,        
                is_seek_allowed       = false,        
            }))
        end

        if raat.transport.is_update_artwork_supported then
            raat.transport.update_artwork(nil)
        end
    end

    close_tcp_audio_client()

    uv.udp_recv_stop(clock_sock)
    --clock_sock:shutdown()
    clock_sock:close()
    audio_sock_tcp:close()
    bus_shutdown()
end

function req_echo(cx, r)
    r.request = nil
    r.status = "Success"
    cx:respond(r)
end

function on_app_channel_message(channel, message)
    for k,cx in pairs(app_channel_subscriptions) do
        cx:respond_nonfinal({ status = "Success", channel = channel, message = json.decode(message) })
    end
end

if raat.app_channel and raat.app_channel.is_supported then
    table.insert(raat.app_channel.message_handlers, on_app_channel_message)
end

function on_transport_control(control)
    if owner_id ~= controller_id then return end
    for k,cx in pairs(transport_subscriptions) do
        cx:respond_nonfinal({ status = "Success", control = json.decode(control) })
    end
end

if raat.transport.is_supported then 
    table.insert(raat.transport.control_handlers, on_transport_control)
end

function on_volume_state_changed(state)
    for k,cx in pairs(controls_subscriptions) do
        cx:respond_nonfinal({ status = "Success", action = "Changed", controls = get_controls_state() })
    end
end

if raat.volume.is_supported then 
    table.insert(raat.volume.state_change_handlers, on_volume_state_changed)
end

function on_source_selection_state_changed(state)
    raat.log.trace("source selection state changed: " .. tostring(state.status))
    for k,cx in pairs(controls_subscriptions) do
        cx:respond_nonfinal({ status = "Success", action = "Changed", controls = get_controls_state() })
    end
end

if raat.source_selection.is_supported then 
    table.insert(raat.source_selection.state_change_handlers, on_source_selection_state_changed)
end

function on_output_message(msg) 
    raat.log.trace("got output message",msg)
    if current and current.setup_cx then
        current.setup_cx:respond_nonfinal({ status = "OutputMessage", message = json_str_to_json(msg) })
    end
end

if raat.output.is_supported then
    table.insert(raat.output.message_handlers, on_output_message)
end

function json_str_to_json(s) 
    if s then
        return json.decode(s)
    else 
        return { }
    end
end

function stepify(value, step) 
    local r = math.floor(value / step + 0.5)  * step
    if step == 1.0 then
        return math.floor(r)    -- make sure this is a lua integer when step=1 for devices with old ABI
    end
    return r
end

function is_volume_step_valid(step) 
    if step == nil then return false end
    return step >= 0.01 and step <= 5.0 
end

function get_controls_state() 
    local controls = { }

    if raat.volume.is_supported and raat.volume.type ~= "none" then
        controls.volume = { 
            type   = raat.volume.state.type,
            min    = raat.volume.state.min, 
            max    = raat.volume.state.max, 
            value  = raat.volume.state.value, 
            mute   = raat.volume.state.mute, 
            info   = json_str_to_json(raat.volume.info),
        }
        --
        -- backwards compatibility: older RAATs will not populate these fields, or will
        -- allow them to default to zero
        --
        if raat.volume.state.step ~= nil and is_volume_step_valid(raat.volume.state.step) then
            controls.volume.step   = raat.volume.state.step
        else 
            -- default step to 1.0 unless we got a real value
            controls.volume.step   = 1.0
        end
        controls.volume.value = stepify(controls.volume.value, controls.volume.step)
    end


    if raat.source_selection.is_supported then
        controls.source = {
            value = raat.source_selection.state.status,
            info  = json_str_to_json(raat.source_selection.info),
        }
        if raat.source_selection.is_standby_supported then
            controls.source.is_standby_supported = true
        end
    end

    return controls
end

function compress(supported_formats)
    local s = ""
    local tbl = { }
    for k,f in pairs(supported_formats) do
        local front = tostring(f.sample_rate) .. "/" .. tostring(f.bits_per_sample)
        local chs   = tbl[front]
        if not chs then
            chs = { } 
            tbl[front] = chs
        end
        table.insert(chs, f.channels)
    end
    local list = { }
    for f,chs in pairs(tbl) do
        local front = f
        table.insert(list, f .. "/" .. join("|", chs))
    end
    return join(",",list)
end

function req_refresh_supported_formats(cx, r)
    if raat.output.is_refresh_supported_formats_supported then
        raat.output.refresh_supported_formats()
        local supported_formats = compress(raat.output.supported_formats)
        if last_supported_formats ~= supported_formats then
            cx:respond({
                status            = "Changed",
                supported_formats = supported_formats
            })
        else
            cx:respond({ 
                status           = "NotChanged",
            })
        end
    end
end

local last_supported_formats

function req_info(cx, r)
    last_supported_formats = compress(raat.output.supported_formats)
    cx:respond({ 
        status           = "Success",
        info             = raat.info,
        platform         = raat.platform,
        output           = {
            supported_formats = last_supported_formats,
            info         = json_str_to_json(raat.output.info),
        },
        transport        = {
            is_supported                = raat.transport.is_supported,
            is_update_status_supported  = raat.transport.is_update_status_supported,
            is_update_artwork_supported = raat.transport.is_update_artwork_supported,
            info                        = json_str_to_json(raat.transport.info),
        },
        volume           = {
            is_supported = raat.volume.is_supported,
            info         = json_str_to_json(raat.volume.info),
        },
        source_selection = {
            is_supported = raat.source_selection.is_supported,
            info         = json_str_to_json(raat.source_selection.info),
        },
        flags            = { has_write_chmap = stream.has_write_chmap },
        codecs           = raat.codecs,
    })
end

function req_get_controls(cx, r)
    cx:respond({ 
        status   = "Success",
        controls = get_controls_state()
    })
end

function req_subscribe_transport(cx, r)
    base.subscribe(transport_subscriptions, r, cx)
end

function req_unsubscribe_transport(cx, r)
    base.unsubscribe(transport_subscriptions, r, cx) 
end

function req_subscribe_app_channel(cx, r)
    base.subscribe(app_channel_subscriptions, r, cx)
end

function req_unsubscribe_app_channel(cx, r)
    base.unsubscribe(app_channel_subscriptions, r, cx)
end

function req_subscribe_controls(cx, r)
    controller_id = r.controller_id
    bus_send(json.encode({ command = "query", owner_id   = r.unique_id }))
    local timer = uv.new_timer()
    timer:start(math.random(200,500), 0, function()
        if not owner_id then
            take_control_ownership()
        end
        timer:stop()
        timer:close()
    end)

    base.subscribe(controls_subscriptions, r, cx)
    cx:respond_nonfinal({ status = "Success", action = "Changed", controls = get_controls_state() })
end

function req_unsubscribe_controls(cx, r)
    base.unsubscribe(controls_subscriptions, r, cx) 
end

function req_request_standby(cx, r)
    if not raat.source_selection.is_supported or not raat.source_selection.is_standby_supported then
        cx:respond({ status = "Success" })
    end
    raat.log.trace("request_standby")
    raat.source_selection.request_standby(function (success, err, info) 
        if success then
            cx:respond({ status = "Success", info = info })
        else
            cx:respond({ status = "UnexpectedError", message = err, info = info })
        end
    end)
end

function req_request_source(cx, r)
    if not raat.source_selection.is_supported then
        take_control_ownership()
        cx:respond({ status = "Success" })
    end

    local cb = function (success, err, info)
        if success then
            take_control_ownership()
            cx:respond({ status = "Success", info = info })
        else
            cx:respond({ status = "UnexpectedError", message = err, info = info })
        end
    end
    if raat.source_selection.request_source_v2 then
        raat.source_selection.request_source_v2(json.encode(r.source_info or {}), cb)
    else
        raat.source_selection.request_source(cb)
    end
end

function req_update_artwork(cx, r)
    if not raat.transport.is_update_artwork_supported then
        cx:respond({ status = "NotSupported" })
        return
    end

    if r.mimetype then
        last_artwork_mime = r.mimetype
        last_artwork_blob = protocol.artwork_blob
    else
        last_artwork_mime = nil
        last_artwork_blob = nil
    end

    if owner_id == controller_id then 
        if r.mimetype then
            raat.transport.update_artwork(r.mimetype, protocol.artwork_blob)
        else 
            raat.transport.update_artwork(nil)
        end
    end
    protocol.artwork_blob = nil
    cx:respond({ status = "Success" })
end

function req_update_transport(cx, r)
    if not raat.transport.is_update_status_supported then
        cx:respond({ status = "NotSupported" })
        return
    end

    last_status = r.status

    if owner_id == controller_id then 
        raat.transport.update_status(json.encode(r.status))
    end
    cx:respond({ status = "Success" })
end

function req_app_channel_send(cx, r)
    if not raat.app_channel.is_supported then
        cx:respond({ status = "NotSupported" })
        return
    end
    raat.app_channel.send_message(r.channel, json.encode(r.message))
end

function req_update_volume(cx, r)
    if not raat.volume.is_supported then return end
    if raat.volume.state.type == "incremental" then
        if r.increment then 
            if r.increment > 0 then
                raat.volume.increment_volume("up");
            elseif r.increment < 0 then
                raat.volume.increment_volume("down");
            end
        end
        if r.toggle_mute then
            raat.volume.toggle_mute()
        end
    else 
        if r.value then
            local volume_step = raat.volume.state.step
            if not is_volume_step_valid(volume_step) then
                volume_step = 1.0 
            end

            if not raat.volume.is_double_volume_supported then
                r.value = math.floor(r.value)
            end

            r.value = stepify(r.value, volume_step)
            raat.volume.set_volume(r.value)
        elseif r.increment then
            if not raat.volume.is_double_volume_supported then
                r.increment = math.floor(r.increment)
            end
            local new_value = r.increment + raat.volume.state.value
            if new_value > raat.volume.state.max then new_value = raat.volume.state.max end
            if new_value < raat.volume.state.min then new_value = raat.volume.state.min end
            raat.volume.set_volume(new_value)
        elseif r.mute ~= nil then
            raat.volume.set_mute(r.mute)
        elseif r.toggle_mute then
            raat.volume.set_mute(not raat.volume.state.mute)
        end
    end

    cx:respond({ status = "Success" })
end

function error_response(err) 
    if err == "RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_IN_USE" then
        return { status = "DeviceInUse", message = err }
    elseif err == "RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_INIT_FAILED" then
        return { status = "DeviceInitFailed", message = err }
    elseif err == "RAAT__OUTPUT_PLUGIN_STATUS_DEVICE_OPEN_FAILED" then
        return { status = "DeviceOpenFailed", message = err }
    else
        return { status = "UnexpectedError", message = err }
    end
end

function req_setup(cx, r)
    if not raat.output.is_supported then 
        cx:respond({ status = "NotSupported" }) 
        return 
    end

    if not raat.source_selection.is_supported then
        take_control_ownership()
    end

    local format = validate_format(r.format)
    if format == nil then
        cx:respond({ status = "InvalidArgument", message = "invalid or missing 'format' argument" }) 
        return
    end

    if current then
        if current.state == PLAYING then 
            status,err = pcall(function() 
                raat.output.stop(current.token)
            end)
            if not status then
                raat.log.warning("failed to stop:", err)
            end
        end
        cleanup_stream()
        if current and current.token then 
            status,err = pcall(function() 
                raat.output.teardown(current.token)
            end)
            if not status then
                raat.log.warning("failed to teardown:", err)
            end
        end

        if current.setup_cx then
            current.setup_cx:respond({ status = "Teardown" })
        end
        current = nil
    end

    current = {
        state               = IDLE,
        setup_cx            = cx,
        format              = format,
        wire_codec          = r.wire_codec,
        is_started          = false,
        got_samples         = 0,
    }
    local my_current = current
    raat.output.setup(r.format, {
        on_setup        = function (success, token_or_error) 
            if success then
                my_current.token = token_or_error
                my_current.state = SETUP
                my_current.setup_cx:respond_nonfinal({ status = "Success", clock_port = clock_endpoint.port, audio_port = 0, audio_port_tcp = audio_endpoint_tcp.port })
            else 
                raat.log.warning("setup failed:", token_or_error)
                my_current.setup_cx:respond(error_response(token_or_error))
                if current == my_current then
                    current = nil
                end
            end
        end,
        on_output_lost  = function (reason) 
            raat.log.trace("output lost")
            my_current.setup_cx:respond({ status = "Lost", reason = json_str_to_json(reason) })
            if current == my_current then
                current = nil
            end
        end
    })
end

function cleanup_stream() 
    if not current then return end

    if current.stats_timer then
        current.stats_timer:stop()
        current.stats_timer:close()
        current.stats_timer = nil
    end

    current.state = SETUP

    if current.stream_cx then
        current.stream_cx:respond({ status = "Ended" })
    end

    if current.codec then
        current.codec = nil
    end

    if current.stream then
        current.stream:clear()
        current.stream = nil
    end

    current.seq                 = nil
    current.stream_cx           = nil
    current.stream_dropouts     = nil
    current.stream_id           = nil
    current.start_time          = nil
    current.start_stream_sample = nil
end

function req_teardown(cx, r)
    if not raat.output.is_supported then 
        cx:respond({ status = "NotSupported" }) 
        return 
    end
    if current == nil then
        cx:respond({ status = "InvalidState" }) 
        return
    end

    if current then
        cleanup_stream()

        if (current.token) then 
            status,err = pcall(function() 
                raat.output.teardown(current.token)
            end)
            if not status then
                raat.log.warning("failed to teardown:", err)
            end
        end

        if current.setup_cx then
            current.setup_cx:respond({ status = "Teardown" })
        end
    end
    current = nil
end

function req_sync(cx, r)
    if not raat.output.is_supported then 
        cx:respond({ status = "NotSupported" }) 
        return 
    end
    if not current or current.state == IDLE then
        cx:respond({ status = "InvalidState" }) 
        return
    end

    local timer = uv.new_timer()
    local ticks_remaining   = 200        -- how many times to nn

    local success_response_count = 0
    local done                   = false

    if current.new_source_pending then
        raat.log.trace("new source pending!")
        r.new_source = true
        current.new_source_pending = false
    end

    local min_rtt        = 100000000000000
    local min_rtt_offset = 0

    timer:start(0, 5, function()
        if done then return end
        if not current or not current.token then 
            done = true
            cx:respond({ status = "InvalidState" })
            timer:stop()
            timer:close()
            return
        end

        local rid = pending_clocksync_requests.next_rid
        local packet = buffer.new(12)
        packet:set_string(0, "RAATCLKQ")
        packet:set_int32_be(8, rid)
        local xmit_time = raat.output.get_local_time(current.token)
        uv.udp_send(clock_sock, packet:get_string(0, 12), r.host, r.port, function(...) end)
        pending_clocksync_requests.next_rid = pending_clocksync_requests.next_rid + 1

        pending_clocksync_requests[rid] = function(success, remote_time) 
            if success and not done then
                local recv_time          = raat.output.get_local_time(current.token)
                local rtt                = recv_time - xmit_time
                local nominal_local_time = xmit_time + rtt / 2;
                local offset             = nominal_local_time - remote_time

                if rtt < min_rtt then
                    min_rtt        = rtt
                    min_rtt_offset = offset
                end

                success_response_count = success_response_count + 1
                raat.log.trace("[roon] got clock sync response, offset=",offset/1000000.0,"rtt=",rtt/1000000.0)

                if success_response_count == CLOCK_SYNC_ITERATIONS and not done then
                    done = true
                    local nominal_offset      = min_rtt_offset
                    local nominal_remote_time = math.floor(raat.output.get_local_time(current.token) - nominal_offset)
                    raat.log.trace("[roon] synced to master clock. offset=" .. tostring(nominal_offset/1000000.0) .. " nominal remote time " .. tostring(nominal_remote_time) .. " rtt " .. tostring(min_rtt/1000000.0))
                    if min_rtt < 30000000 then
                        raat.output.set_remote_time(current.token, nominal_offset, r.new_source)
                    else
                        if r.new_source then
                            current.new_source_pending = true
                        end
                        raat.log.warning("[roon] not passing clock measurement to endpoint because RTT was too large (>30ms)")
                    end
                    cx:respond({ status = "Success" })
                    timer:stop()
                    timer:close()
                end
            end
        end

        ticks_remaining = ticks_remaining - 1
        if ticks_remaining == 0 or done then 
            timer:stop()
            timer:close()
            if not done then
                done = true
                raat.log.trace("[roon] clock sync failed (failed to get 5 successful responses after 20 attempts")
                cx:respond({ status = "Timeout" })
            end
        end
    end)
end

function req_set_client_type(cx, r)
    if raat.session.set_client_type then
        raat.session.set_client_type(r.client_type)
    end
    cx:respond({ status = "Success" }) 
end

function req_force_exit(cx, r)
    os.exit(1)
end

function req_end_stream(cx, r)
    if not current then 
        cx:respond({ status = "Success" }) 
        return
    end

    if not raat.output.is_supported then 
        cx:respond({ status = "NotSupported" }) 
        return 
    end
    if current.state == IDLE then
        cx:respond({ status = "InvalidState" }) 
        return
    end

    if current.state == PLAYING then 
        status,err = pcall(function() 
            raat.output.stop(current.token)
        end)
        if not status then
            raat.log.warning("failed to stop:", err)
        end
    end

    cleanup_stream()

    cx:respond({ status = "Success" }) 
end

function req_stream(cx, r)
    if not raat.output.is_supported then 
        cx:respond({ status = "NotSupported" }) 
        return 
    end

    local first_seq = tonumber(r.first_seq)
    if not first_seq then
        cx:respond({ status = "InvalidArgument", message = "missing or invalid 'first_seq' argument" }) 
        return
    end

    local stream_id = tonumber(r.stream_id)
    if not stream_id then
        cx:respond({ status = "InvalidArgument", message = "missing or invalid 'stream_id' argument" }) 
        return
    end

    if not current then
        cx:respond({ status = "InvalidState" }) 
        return
    end

    local bytes_per_second = current.format.sample_rate * current.format.channels * current.format.bits_per_sample / 8

    current.buffer_size     = r.buffer_size * current.format.sample_rate
    current.ready_threshold = r.ready_threshold
    current.stream_id       = stream_id
    current.stream          = stream.new(current.format, 2*current.buffer_size)
    if current.wire_codec then
        current.codec       = codec.new(current.wire_codec, current.format)
    end
    current.state           = BUFFERING
    current.got_samples     = 0
    current.stream_cx       = cx
    current.stream_dropouts = nil
    current.stream_cx:respond_nonfinal({ status = "Buffering" })

    -- setup stats timer (500ms)
    if current.stats_timer then
        current.stats_timer:stop()
        current.stats_timer:close()
        current.stats_timer = nil
    end
    current.stats_timer = uv.new_timer()
    current.stats_timer:start(500, 500, on_stats_timer)
end

function req_start(cx, r)
    if not raat.output.is_supported then 
        cx:respond({ status = "NotSupported" }) 
        return 
    end
    if not current or (current.state ~= BUFFERING and current.state ~= STOPPED and current.state ~= READY) then
        cx:respond({ status = "InvalidState" }) 
        return
    end

    local time          = tonumber(r.time)
    local stream_sample = tonumber(r.stream_sample)
    local min_offset    = tonumber(r.min_offset)

    if not time then
        local delay = 50000000
        if raat.output.is_get_output_delay_supported and current and current.token ~= nil then
            delay = raat.output.get_output_delay(current.token)
        end
        time = raat.output.get_local_time(current.token) + min_offset
        raat.log.trace("Doing ASAP start based on current device clock (min_offset=" .. tostring(min_offset) .. ")")
    end

    if not stream_sample then
        cx:respond({ status = "InvalidArgument", message = "invalid or missing 'stream_sample' argument" }) 
        return
    end

    local status,err = pcall(function() 
        raat.output.start(current.token, time, stream_sample, current.stream)
        current.state               = PLAYING
        current.start_time          = time
        current.start_stream_sample = stream_sample
    end)

    if status then
        current.stream_cx:respond_nonfinal({ status = "Playing" })
        cx:respond({ status = "Success", time = time })
    else 
        cx:respond({ status = "UnexpectedError", message = err })
    end
end

function req_stop(cx, r)
    if not raat.output.is_supported then 
        cx:respond({ status = "NotSupported" }) 
        return 
    end
    if not current or current.state ~= PLAYING then
        cx:respond({ status = "InvalidState" }) 
        return
    end

    local status,err = pcall(function() 
        raat.output.stop(current.token)
        current.start_time          = nil
        current.start_stream_sample = nil
        current.state               = STOPPED
    end)

    if status then
        current.stream_cx:respond_nonfinal({ status = "Stopped" })
        cx:respond({ status = "Success" })
    else
        cx:respond({ status = "UnexpectedError", message = err })
    end
end

--
-- Toplevel request dispatcher
--
local handlers = {
    echo                  = req_echo,
    get_controls          = req_get_controls,
    subscribe_controls    = req_subscribe_controls,
    unsubscribe_controls  = req_unsubscribe_controls,
    subscribe_transport   = req_subscribe_transport,
    unsubscribe_transport = req_unsubscribe_transport,
    subscribe_app_channel    = req_subscribe_app_channel,
    unsubscribe_app_channel  = req_unsubscribe_app_channel,
    app_channel_send_message = req_app_channel_send,
    update_volume         = req_update_volume,
    increment_volume      = req_increment_volume,
    update_transport      = req_update_transport, 
    update_artwork        = req_update_artwork, 
    request_source        = req_request_source,
    request_standby       = req_request_standby,
    info                  = req_info,
    refresh_supported_formats = req_refresh_supported_formats,
    setup                 = req_setup,
    teardown              = req_teardown,
    sync                  = req_sync,
    stream                = req_stream,
    start                 = req_start,
    stop                  = req_stop,
    end_stream            = req_end_stream,
    set_client_type       = req_set_client_type,
    force_exit            = req_force_exit,
}
protocol.on_request = function (cx, r) 
    local handler = handlers[r.request]
    if handler == nil then 
        cx:respond({ status = "NotSupported" })
    else
        handler(cx, r)
    end
end

table.insert(raat.session.shutdown_handlers, shutdown)
go()            -- setup the sockets and go

return roon

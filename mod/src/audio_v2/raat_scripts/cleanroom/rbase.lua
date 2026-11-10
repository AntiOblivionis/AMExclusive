-- Rune clean-room RAAT protocol scripts -- tiny shared helpers.
-- Written from the publicly documented RAAT behaviour; no Roon source was
-- copied (see docs/CLEAN_ROOM.md).

local rbase = {}

local anonSeq = 0

-- Registry helpers shared by every subscribe_* request.
-- A subscription table maps subscription-id -> responding context.
function rbase.subscribeAt(tbl, req)
  if req.subscription then
    return req.subscription
  end
  anonSeq = anonSeq + 1
  return "__anon_" .. anonSeq
end

function rbase.subscribe(tbl, req, cx)
  tbl[rbase.subscribeAt(req)] = cx
end

function rbase.unsubscribe(tbl, req, cx)
  local id = req.subscription
  if id and tbl[id] then
    tbl[id] = nil
    cx:respond({ status = "Success" })
    return
  end
  cx:respond({ status = "NotFound" })
end

function rbase.notifyAll(tbl, payload)
  for _, cx in pairs(tbl) do
    cx:respond_nonfinal(payload)
  end
end

function rbase.addrToString(addr)
  return tostring(addr.ip) .. ":" .. tostring(addr.port)
end

return rbase

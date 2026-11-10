local base = { }

local next_subscription_id_gen = 1

function base.subscribe(table, request, cx) 
    local subscription = request.subscription
    if not subscription then
        subscription = "__uniq__" .. next_subscription_id_gen .. "__uniq__"
        next_subscription_id_gen = next_subscription_id_gen + 1
    end
    table[subscription] = cx
end

function base.unsubscribe(table, request, cx)  
    if request.subscription and table[request.subscription] then
        table[request.subscription] = nil
        cx:respond({ status = "Success" })
        return
    end
    cx:respond({ status = "NotFound" })
end

function addr_tostring(addr)
    return tostring(addr.ip) .. ":" .. tostring(addr.port)
end

function keys(tbl)
    local keyset={}
    local n=0
    for k,v in pairs(tab) do
        n=n+1
        keyset[n]=k
    end
end

function join(delimiter, list)
    local len = #list
    if len == 0 then 
        return "" 
    end
    local string = list[1]
    for i = 2, len do 
        string = string .. delimiter .. list[i] 
    end
    return string
end

return base

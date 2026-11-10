-- Rune clean-room JSON codec (decode/encode) for the RAAT Lua scripts.
-- Minimal recursive-descent implementation written for this project.
-- Lua 5.1/JIT compatible: no goto, no integer division, no bitwise ops.

local rjson = {}

local FAIL = {} -- parse-failure sentinel
local NULL = {} -- JSON null marker (kept as identity table)
rjson.null = NULL

local ESC = {
  ['\\'] = '\\', ['"'] = '"', ['/'] = '/',
  b = "\8", f = "\12", n = "\10", r = "\13", t = "\9",
}

local function utf8FromCodepoint(cp)
  if cp < 0x80 then
    return string.char(cp)
  elseif cp < 0x800 then
    return string.char(0xC0 + math.floor(cp / 0x40),
                       0x80 + (cp % 0x40))
  elseif cp < 0x10000 then
    return string.char(0xE0 + math.floor(cp / 0x1000),
                       0x80 + (math.floor(cp / 0x40) % 0x40),
                       0x80 + (cp % 0x40))
  else
    return string.char(0xF0 + math.floor(cp / 0x40000),
                       0x80 + (math.floor(cp / 0x1000) % 0x40),
                       0x80 + (math.floor(cp / 0x40) % 0x40),
                       0x80 + (cp % 0x40))
  end
end

local parseValue -- forward

local function skipSpace(s, i)
  while true do
    local c = string.sub(s, i, i)
    if c == " " or c == "\t" or c == "\n" or c == "\r" then
      i = i + 1
    else
      return i
    end
  end
end

local function parseString(s, i)
  i = i + 1 -- opening quote
  local pieces = {}
  while true do
    local c = string.sub(s, i, i)
    if c == "" then
      return FAIL
    elseif c == '"' then
      return table.concat(pieces), i + 1
    elseif c == "\\" then
      local e = string.sub(s, i + 1, i + 1)
      if e == "u" then
        local hex = string.sub(s, i + 2, i + 5)
        local cp = tonumber(hex, 16)
        if not cp then return FAIL end
        -- surrogate pairs
        if cp >= 0xD800 and cp <= 0xDBFF then
          if string.sub(s, i + 6, i + 7) == "\\u" then
            local lo = tonumber(string.sub(s, i + 8, i + 11), 16)
            if lo then
              cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)
              pieces[#pieces + 1] = utf8FromCodepoint(cp)
              i = i + 12
            else
              return FAIL
            end
          else
            return FAIL
          end
        else
          pieces[#pieces + 1] = utf8FromCodepoint(cp)
          i = i + 6
        end
      else
        pieces[#pieces + 1] = ESC[e] or e
        i = i + 2
      end
    else
      pieces[#pieces + 1] = c
      i = i + 1
    end
  end
end

local function parseNumber(s, i)
  local last = i
  while true do
    local c = string.sub(s, last + 1, last + 1)
    if c:find("[%-%+0-9eE%.]") then
      last = last + 1
    else
      break
    end
  end
  local n = tonumber(string.sub(s, i, last))
  if n == nil then return FAIL end
  return n, last + 1
end

local function parseLiteral(s, i, word, val)
  if string.sub(s, i, i + #word - 1) == word then
    return val, i + #word
  end
  return FAIL
end

local function parseArray(s, i)
  local out = {}
  i = skipSpace(s, i + 1)
  if string.sub(s, i, i) == "]" then
    return out, i + 1
  end
  while true do
    local v
    v, i = parseValue(s, i)
    if v == FAIL then return FAIL end
    out[#out + 1] = (v == NULL) and rjson.null or v
    i = skipSpace(s, i)
    local c = string.sub(s, i, i)
    if c == "," then
      i = skipSpace(s, i + 1)
    elseif c == "]" then
      return out, i + 1
    else
      return FAIL
    end
  end
end

local function parseObject(s, i)
  local out = {}
  i = skipSpace(s, i + 1)
  if string.sub(s, i, i) == "}" then
    return out, i + 1
  end
  while true do
    i = skipSpace(s, i)
    if string.sub(s, i, i) ~= '"' then return FAIL end
    local key
    key, i = parseString(s, i)
    if key == FAIL then return FAIL end
    i = skipSpace(s, i)
    if string.sub(s, i, i) ~= ":" then return FAIL end
    local v
    v, i = parseValue(s, skipSpace(s, i + 1))
    if v == FAIL then return FAIL end
    if v ~= NULL then
      out[key] = v
    end
    i = skipSpace(s, i)
    local c = string.sub(s, i, i)
    if c == "," then
      i = i + 1
    elseif c == "}" then
      return out, i + 1
    else
      return FAIL
    end
  end
end

parseValue = function(s, i)
  i = skipSpace(s, i)
  local c = string.sub(s, i, i)
  if c == "{" then
    return parseObject(s, i)
  elseif c == "[" then
    return parseArray(s, i)
  elseif c == '"' then
    return parseString(s, i)
  elseif c == "t" then
    return parseLiteral(s, i, "true", true)
  elseif c == "f" then
    return parseLiteral(s, i, "false", false)
  elseif c == "n" then
    return parseLiteral(s, i, "null", NULL)
  else
    return parseNumber(s, i)
  end
end

function rjson.decode(str)
  if type(str) ~= "string" then return nil end
  local v = parseValue(str, 1)
  if v == FAIL or v == nil then return nil end
  if v == NULL then return nil end
  return v
end

local escapeStr = { ['\\'] = '\\\\', ['"'] = '\\"', ['\10'] = '\\n',
                    ['\13'] = '\\r', ['\9'] = '\\t' }

local function encodeString(v)
  local out = string.gsub(v, '[\\"\10\13\9]', escapeStr)
  return '"' .. out .. '"'
end

local function isSequential(t)
  local n = 0
  for k in pairs(t) do
    if type(k) ~= "number" then return false end
    n = n + 1
  end
  for i = 1, n do
    if t[i] == nil then return false end
  end
  return true
end

local encodeValue -- forward

local function encodeTable(t)
  if isSequential(t) then
    local parts = {}
    for i = 1, #t do
      parts[i] = encodeValue(t[i])
    end
    return "[" .. table.concat(parts, ",") .. "]"
  end
  local parts = {}
  for k, v in pairs(t) do
    if type(k) == "string" then
      parts[#parts + 1] = encodeString(k) .. ":" .. encodeValue(v)
    end
  end
  return "{" .. table.concat(parts, ",") .. "}"
end

encodeValue = function(v)
  if v == NULL then
    return "null"
  end
  local ty = type(v)
  if ty == "string" then
    return encodeString(v)
  elseif ty == "number" then
    if v ~= v then return "null" end
    if v == math.floor(v) and math.abs(v) < 2^53 then
      return string.format("%.0f", v)
    end
    return string.format("%.17g", v)
  elseif ty == "boolean" then
    return tostring(v)
  elseif ty == "table" then
    return encodeTable(v)
  end
  return "null"
end

function rjson.encode(v)
  return encodeValue(v)
end

return rjson

-- configs/fastfetch.lua — render [config.fastfetch] to fastfetch's JSONC
-- config file at ~/.config/fastfetch/config.jsonc.
--
-- fastfetch's own format is JSONC (JSON with comments), but consuming
-- side accepts plain JSON, so we just emit JSON. The structure mirrors
-- whatever the user wrote in the blueprint — we don't impose a schema
-- because fastfetch's config knobs are too numerous to track.
--
-- Implementation note: Lua doesn't have a built-in JSON encoder, so we
-- roll a tiny one here. Fine for renderer-side use — config blobs are
-- small (10s of fields), no recursion-depth concerns in practice, and
-- we control the input shape (it came from the blueprint's TOML/Lua
-- which is well-formed by construction).

local M = {}

function M.target_path(_cfg, ctx)
  return ctx.xdg_config .. "/fastfetch/config.jsonc"
end

local encode  -- forward declare for mutual recursion

local function escape_string(s)
  -- JSON requires escaping these. We don't bother with full unicode
  -- escapes; valid UTF-8 input passes through verbatim, which JSON
  -- spec allows.
  s = s:gsub("\\", "\\\\")
  s = s:gsub('"', '\\"')
  s = s:gsub("\n", "\\n")
  s = s:gsub("\r", "\\r")
  s = s:gsub("\t", "\\t")
  return s
end

-- Detect array-vs-object the same way blueprint_lua / lua_json does:
-- contiguous integer keys 1..N → array.
local function is_array(t)
  local n = 0
  for k, _ in pairs(t) do
    if type(k) ~= "number" then return false end
    if k < 1 or k ~= math.floor(k) then return false end
    if k > n then n = k end
  end
  -- Count actual entries; if max == count, it's a contiguous array.
  local count = 0
  for _ in pairs(t) do count = count + 1 end
  return count == n, n
end

local function encode_array(t, n, indent, level)
  if n == 0 then return "[]" end
  local pad     = string.rep("  ", level + 1)
  local pad_end = string.rep("  ", level)
  local parts = {}
  for i = 1, n do
    table.insert(parts, pad .. encode(t[i], indent, level + 1))
  end
  return "[\n" .. table.concat(parts, ",\n") .. "\n" .. pad_end .. "]"
end

local function encode_object(t, indent, level)
  -- Sort keys for stable diffs.
  local keys = {}
  for k, _ in pairs(t) do
    table.insert(keys, tostring(k))
  end
  table.sort(keys)
  if #keys == 0 then return "{}" end
  local pad     = string.rep("  ", level + 1)
  local pad_end = string.rep("  ", level)
  local parts = {}
  for _, k in ipairs(keys) do
    table.insert(parts,
      pad .. '"' .. escape_string(k) .. '": ' ..
      encode(t[k], indent, level + 1))
  end
  return "{\n" .. table.concat(parts, ",\n") .. "\n" .. pad_end .. "}"
end

encode = function(v, indent, level)
  level = level or 0
  local t = type(v)
  if v == nil then
    return "null"
  elseif t == "boolean" then
    return v and "true" or "false"
  elseif t == "number" then
    -- Integers stay integral; floats stringify naturally.
    if v == math.floor(v) and v == v and math.abs(v) < 1e15 then
      return tostring(math.floor(v))
    end
    return tostring(v)
  elseif t == "string" then
    return '"' .. escape_string(v) .. '"'
  elseif t == "table" then
    local arr, n = is_array(v)
    if arr then
      return encode_array(v, n, indent, level)
    end
    return encode_object(v, indent, level)
  end
  -- Functions / userdata — emit their tostring as a fallback string.
  return '"' .. escape_string(tostring(v)) .. '"'
end

function M.render(cfg, _ctx)
  return encode(cfg, true, 0) .. "\n"
end

return M

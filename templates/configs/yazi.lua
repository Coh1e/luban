-- configs/yazi.lua — render [config.yazi] to yazi's TOML config file.
--
-- yazi reads ~/.config/yazi/yazi.toml (and a few sibling files) directly
-- as TOML. Since the blueprint already supplies structured config, we
-- emit it back as TOML — essentially a passthrough re-serialization.
--
-- This is the simplest of the renderers: most fidelity comes from the
-- TOML emitter staying boring (alphabetized keys, double-quoted strings,
-- one-line arrays for primitives, indented dotted notation for nested
-- tables). We don't try to roundtrip TOML's date/time types because the
-- blueprint side never generates them.

local M = {}

function M.target_path(_cfg, ctx)
  return ctx.xdg_config .. "/yazi/yazi.toml"
end

local emit_value  -- forward decl for mutual recursion

local function escape_string(s)
  s = s:gsub("\\", "\\\\")
  s = s:gsub('"', '\\"')
  s = s:gsub("\n", "\\n")
  s = s:gsub("\r", "\\r")
  s = s:gsub("\t", "\\t")
  return '"' .. s .. '"'
end

local function is_array(t)
  local n = 0
  for k, _ in pairs(t) do
    if type(k) ~= "number" then return false end
    if k < 1 or k ~= math.floor(k) then return false end
    if k > n then n = k end
  end
  local count = 0
  for _ in pairs(t) do count = count + 1 end
  return count == n, n
end

emit_value = function(v)
  local t = type(v)
  if t == "string"  then return escape_string(v) end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number" then
    if v == math.floor(v) and math.abs(v) < 1e15 then
      return tostring(math.floor(v))
    end
    return tostring(v)
  end
  if t == "table" then
    local arr, n = is_array(v)
    if arr then
      local parts = {}
      for i = 1, n do
        table.insert(parts, emit_value(v[i]))
      end
      return "[" .. table.concat(parts, ", ") .. "]"
    end
    -- Inline table — not commonly used in yazi but handle defensively.
    local keys = {}
    for k, _ in pairs(v) do table.insert(keys, tostring(k)) end
    table.sort(keys)
    local parts = {}
    for _, k in ipairs(keys) do
      table.insert(parts, k .. " = " .. emit_value(v[k]))
    end
    return "{ " .. table.concat(parts, ", ") .. " }"
  end
  return escape_string(tostring(v))
end

-- Walk `cfg` and emit one [section] block per top-level table whose own
-- entries are scalars/arrays. Plain scalar entries at the top level go
-- before any section. Nested tables become dotted [a.b.c] sections.
local function emit_table(out, prefix, tbl)
  -- Two passes: scalars first, sub-tables next (TOML readability).
  local scalars = {}
  local subtables = {}
  for k, v in pairs(tbl) do
    if type(v) == "table" and not is_array(v) then
      -- Confirm it's a true sub-table (not array-of-primitives).
      table.insert(subtables, k)
    else
      table.insert(scalars, k)
    end
  end
  table.sort(scalars)
  table.sort(subtables)

  if #scalars > 0 then
    if prefix ~= "" then
      table.insert(out, "[" .. prefix .. "]")
    end
    for _, k in ipairs(scalars) do
      table.insert(out, k .. " = " .. emit_value(tbl[k]))
    end
    table.insert(out, "")  -- blank line between sections
  end

  for _, k in ipairs(subtables) do
    local new_prefix = prefix == "" and k or (prefix .. "." .. k)
    emit_table(out, new_prefix, tbl[k])
  end
end

function M.render(cfg, _ctx)
  local out = {}
  emit_table(out, "", cfg)
  -- Drop trailing blank line if any.
  while #out > 0 and out[#out] == "" do
    table.remove(out)
  end
  table.insert(out, "")  -- exactly one trailing newline
  return table.concat(out, "\n")
end

return M

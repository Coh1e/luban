-- configs/bat.lua — render [config.bat] to bat's CLI-flag config file.
--
-- bat reads ~/.config/bat/config (Linux/macOS) or %APPDATA%/bat/config
-- (Windows) — a plain-text file where each line is a CLI flag, e.g.:
--
--     --theme=ansi
--     --style=numbers,changes,header
--     --paging=never
--
-- We collapse blueprint config (kebab- or snake-case keys) into that
-- shape. Boolean true emits a bare `--flag`; false skips. Lists become
-- `--key=a,b,c`. Strings/numbers become `--key=value`.

local M = {}

function M.target_path(_cfg, ctx)
  return ctx.xdg_config .. "/bat/config"
end

-- Map snake_case / camelCase to kebab-case, since bat's flags are kebab.
local function to_flag(key)
  -- Convert snake_case to kebab-case.
  key = key:gsub("_", "-")
  -- Convert camelCase to kebab-case.
  key = key:gsub("(%l)(%u)", function(a, b) return a .. "-" .. b:lower() end)
  return key:lower()
end

-- Emit one config line for (key, value) of various Lua types.
local function emit_line(lines, key, value)
  local flag = to_flag(key)
  local t = type(value)
  if t == "boolean" then
    if value then
      table.insert(lines, "--" .. flag)
    end
  elseif t == "table" then
    -- Treated as a list of strings → comma-joined.
    local parts = {}
    for _, v in ipairs(value) do
      table.insert(parts, tostring(v))
    end
    table.insert(lines, "--" .. flag .. "=" .. table.concat(parts, ","))
  else
    table.insert(lines, "--" .. flag .. "=" .. tostring(value))
  end
end

function M.render(cfg, _ctx)
  local lines = {}
  -- Iterate sorted to keep output stable across runs.
  local keys = {}
  for k, _ in pairs(cfg) do
    table.insert(keys, k)
  end
  table.sort(keys)
  for _, k in ipairs(keys) do
    emit_line(lines, k, cfg[k])
  end
  -- Trailing newline.
  if #lines > 0 then
    table.insert(lines, "")
  end
  return table.concat(lines, "\n")
end

return M

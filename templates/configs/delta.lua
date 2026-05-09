-- configs/delta.lua — render [config.delta] to a git-config drop-in that
-- wires delta in as git's pager and adds a [delta] section.
--
-- delta itself reads its options from ~/.gitconfig under the [delta]
-- section. So the renderer's output is in fact git INI format, and the
-- target path is a separate drop-in alongside config.git's so users
-- can opt into either independently:
--
--     ~/.gitconfig.d/<bp>-delta.gitconfig
--
-- Schema:
--   [config.delta]
--   features    = "decorations side-by-side"
--   navigate    = true
--   line_numbers = true
--   syntax_theme = "Monokai Extended"
--
-- Anything we don't recognize is passed through to [delta] verbatim
-- (key = value), preserving the same lenient policy as git.lua.

local M = {}

-- DESIGN §4/§7 capability declaration (read by lua_frontend::extract_capability).
M.capability = {
  writable_dirs   = { "~/.gitconfig.d/" },
  overwrite       = false,    -- per-bp drop-in, never the canonical .gitconfig
  needs_confirm   = false,
  touches_profile = false,
}

function M.target_path(_cfg, ctx)
  return ctx.home .. "/.gitconfig.d/" .. ctx.blueprint_name .. "-delta.gitconfig"
end

local function quote(v)
  v = tostring(v)
  v = v:gsub("\\", "\\\\"):gsub('"', '\\"')
  return '"' .. v .. '"'
end

local function emit_kv(lines, k, v)
  local t = type(v)
  if t == "boolean" then
    table.insert(lines, "    " .. k .. " = " .. (v and "true" or "false"))
  elseif t == "number" then
    table.insert(lines, "    " .. k .. " = " .. tostring(v))
  else
    table.insert(lines, "    " .. k .. " = " .. quote(v))
  end
end

function M.render(cfg, _ctx)
  local lines = {}

  -- [core] pager = delta — required for delta to actually run.
  table.insert(lines, "[core]")
  table.insert(lines, "    pager = delta")

  -- [interactive] diffFilter = delta --color-only — for git add -p.
  table.insert(lines, "[interactive]")
  table.insert(lines, "    diffFilter = delta --color-only")

  -- [delta] block carries the user's actual delta options. Sort keys
  -- for diff stability.
  table.insert(lines, "[delta]")
  local keys = {}
  for k, _ in pairs(cfg) do
    table.insert(keys, k)
  end
  table.sort(keys)
  for _, k in ipairs(keys) do
    -- delta's option names are kebab-case but git INI keys can be
    -- camelCase or kebab — delta accepts both. Pass through as the
    -- user wrote them.
    emit_kv(lines, k, cfg[k])
  end

  table.insert(lines, "")
  return table.concat(lines, "\n")
end

return M

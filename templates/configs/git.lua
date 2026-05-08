-- configs/git.lua — render [config.git] blocks to git's INI format.
--
-- Per docs/DESIGN.md §11.3, git is a "drop-in" tool: we never touch the
-- user's ~/.gitconfig directly. Instead we write to ~/.gitconfig.d/<bp>.gitconfig
-- and trust the user has [include]ed that subdir from their main config
-- (luban will print an instruction the first time apply runs).
--
-- Schema accepted (matches home-manager's programs.git pretty closely):
--
--   [config.git]
--   userName  = "Coh1e"
--   userEmail = "x@example.com"
--
--   [config.git.aliases]
--   co = "checkout"
--   br = "branch"
--
--   [config.git.core]
--   editor = "vim"
--
--   [config.git.credential]
--   helper = "manager"        -- emits [credential] helper = manager
--
--   lfs = true                -- emits [filter "lfs"] with the canonical
--                             -- clean/smudge/process/required four-tuple,
--                             -- equivalent to running `git lfs install`.
--                             -- Requires git-lfs.exe on PATH (see cpp-base
--                             -- blueprint).
--
--   [config.git.extra]
--   "section.key" = "value"   -- escape hatch for arbitrary [section] key=value
--
-- Anything we don't recognize at the top level is silently ignored — we
-- prefer renderer stability over schema strictness, since each new git
-- config knob doesn't deserve a luban release.

local M = {}

function M.target_path(cfg, ctx)
  return ctx.home .. "/.gitconfig.d/" .. ctx.blueprint_name .. ".gitconfig"
end

-- Quote a value for git's INI parser. Git accepts unquoted values for
-- simple cases but trips on values with `;`, `#`, or leading/trailing
-- whitespace. Always quoting is safer than guessing.
local function quote(v)
  v = tostring(v)
  -- Escape backslashes and double quotes per git-config(1).
  v = v:gsub("\\", "\\\\"):gsub('"', '\\"')
  return '"' .. v .. '"'
end

-- Emit one `[section] key = value` block. `entries` is a Lua table of
-- key→value (string or number); skipped if empty.
local function emit_section(lines, name, entries)
  if entries == nil then return end
  local sorted_keys = {}
  for k, _ in pairs(entries) do
    table.insert(sorted_keys, k)
  end
  table.sort(sorted_keys)
  if #sorted_keys == 0 then return end
  table.insert(lines, "[" .. name .. "]")
  for _, k in ipairs(sorted_keys) do
    table.insert(lines, "    " .. k .. " = " .. quote(entries[k]))
  end
end

function M.render(cfg, _ctx)
  local lines = {}

  -- [user] from top-level userName / userEmail (home-manager idiom).
  if cfg.userName or cfg.userEmail or cfg.signingKey then
    table.insert(lines, "[user]")
    if cfg.userName    then table.insert(lines, "    name = "       .. quote(cfg.userName))    end
    if cfg.userEmail   then table.insert(lines, "    email = "      .. quote(cfg.userEmail))   end
    if cfg.signingKey  then table.insert(lines, "    signingKey = " .. quote(cfg.signingKey))  end
  end

  emit_section(lines, "alias", cfg.aliases)
  emit_section(lines, "core", cfg.core)
  emit_section(lines, "color", cfg.color)
  emit_section(lines, "init", cfg.init)
  emit_section(lines, "merge", cfg.merge)
  emit_section(lines, "pull", cfg.pull)
  emit_section(lines, "push", cfg.push)
  emit_section(lines, "rebase", cfg.rebase)
  emit_section(lines, "diff", cfg.diff)
  emit_section(lines, "credential", cfg.credential)

  -- [filter "lfs"] — equivalent to `git lfs install` (writes the four
  -- canonical filter keys). Activated by `lfs = true` at top level; the
  -- subsection name needs the quoted two-level form, so we emit it
  -- inline rather than via emit_section.
  if cfg.lfs then
    table.insert(lines, '[filter "lfs"]')
    table.insert(lines, '    clean = '    .. quote("git-lfs clean -- %f"))
    table.insert(lines, '    smudge = '   .. quote("git-lfs smudge -- %f"))
    table.insert(lines, '    process = '  .. quote("git-lfs filter-process"))
    table.insert(lines, '    required = ' .. quote("true"))
  end

  -- [extra] escape hatch: cfg.extra = { ["section.key"] = "value", ... }
  -- Each key is "section.subkey" form; we emit them as
  -- [section] subkey = value.
  if cfg.extra ~= nil then
    -- Group by section first.
    local by_section = {}
    for k, v in pairs(cfg.extra) do
      local section, subkey = k:match("^([^.]+)%.(.+)$")
      if section and subkey then
        by_section[section] = by_section[section] or {}
        by_section[section][subkey] = v
      end
    end
    for section, entries in pairs(by_section) do
      emit_section(lines, section, entries)
    end
  end

  -- Blank trailing line for tidy diffs.
  if #lines > 0 then
    table.insert(lines, "")
  end
  return table.concat(lines, "\n")
end

return M

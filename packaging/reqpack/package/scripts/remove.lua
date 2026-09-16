local layout = dofile(context.paths.controlDir .. "/scripts/layout.lua")
local paths = layout.paths(context)

local function remove_if_owned(symlink_path, expected_target)
  if context.fs.exists(symlink_path) then
    local current_target = layout.read_symlink(context, symlink_path)
    if current_target == expected_target then
      local ok, error_message = layout.remove_path(context, symlink_path)
      if not ok then
        context.tx.failed(error_message)
        return false
      end
    end
  end
  return true
end

if not remove_if_owned(paths.symlink_path, paths.binary_path) then
  return false
end

if not remove_if_owned(paths.cli_symlink_path, paths.cli_binary_path) then
  return false
end

return true

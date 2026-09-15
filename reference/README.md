# Reference implementations

This directory contains upstream projects used as UI/UX reference during development.

## nvim-dap-ui

Cloned from https://github.com/rcarriga/nvim-dap-ui

Key files to compare when implementing panels:

| nvim-dap-ui element | Lua source | C++ panel |
|---|---|---|
| Layout (sidebar + tray) | `lua/dapui/config/init.lua` | `cpp/src/ui/app.cpp` |
| Scopes | `lua/dapui/elements/scopes.lua` | `cpp/src/ui/scopes_panel.cpp` |
| Stacks | `lua/dapui/elements/stacks.lua` | `cpp/src/ui/stacks_panel.cpp` |
| Breakpoints | `lua/dapui/elements/breakpoints.lua` | `cpp/src/ui/breakpoints_panel.cpp` |
| Watches | `lua/dapui/elements/watches.lua` | `cpp/src/ui/watches_panel.cpp` |
| REPL | `lua/dapui/elements/repl.lua` | `cpp/src/ui/repl_panel.cpp` |
| Console | `lua/dapui/elements/console.lua` | `cpp/src/ui/console_panel.cpp` |
| Source (standalone) | — | `cpp/src/ui/source_panel.cpp` |

Default layout from nvim-dap-ui config:

- **Left sidebar (40 cols):** scopes 25%, breakpoints 25%, stacks 25%, watches 25%
- **Bottom tray (10 lines):** repl + console
- **Center:** source viewer (our addition)

# Reference implementations

This directory contains upstream projects used as UI/UX reference during development.

## nvim-dap-ui

Cloned from https://github.com/rcarriga/nvim-dap-ui

Key files to compare when implementing Rust panels:

| nvim-dap-ui element | Lua source | Rust panel |
|---|---|---|
| Layout (sidebar + tray) | `lua/dapui/config/init.lua` | `src/ui/layout.rs` |
| Scopes | `lua/dapui/elements/scopes.lua` | `src/ui/panels/scopes.rs` |
| Stacks | `lua/dapui/elements/stacks.lua` | `src/ui/panels/stacks.rs` |
| Breakpoints | `lua/dapui/elements/breakpoints.lua` | `src/ui/panels/breakpoints.rs` |
| Watches | `lua/dapui/elements/watches.lua` | `src/ui/panels/watches.rs` |
| REPL | `lua/dapui/elements/repl.lua` | `src/ui/panels/repl.rs` |
| Console | `lua/dapui/elements/console.lua` | `src/ui/panels/console.rs` |
| Source (standalone) | — | `src/ui/panels/source.rs` |

Default layout from nvim-dap-ui config:

- **Left sidebar (40 cols):** scopes 25%, breakpoints 25%, stacks 25%, watches 25%
- **Bottom tray (10 lines):** repl + console
- **Center:** source viewer (our addition)

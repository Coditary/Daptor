pub mod breakpoints;
pub mod console;
pub mod repl;
pub mod scopes;
pub mod source;
pub mod stacks;
pub mod watches;

use breakpoints::BreakpointsPanel;
use console::ConsolePanel;
use repl::ReplPanel;
use scopes::ScopesPanel;
use source::SourcePanel;
use stacks::StacksPanel;
use watches::WatchesPanel;

/// All nvim-dap-ui elements plus standalone source viewer.
pub struct AllPanels {
    pub source: SourcePanel,
    pub scopes: ScopesPanel,
    pub breakpoints: BreakpointsPanel,
    pub stacks: StacksPanel,
    pub watches: WatchesPanel,
    pub repl: ReplPanel,
    pub console: ConsolePanel,
}

impl Default for AllPanels {
    fn default() -> Self {
        Self {
            source: SourcePanel::default(),
            scopes: ScopesPanel::default(),
            breakpoints: BreakpointsPanel::default(),
            stacks: StacksPanel::default(),
            watches: WatchesPanel::default(),
            repl: ReplPanel::default(),
            console: ConsolePanel::default(),
        }
    }
}

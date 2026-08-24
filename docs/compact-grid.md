# Compact Grid Representation

wmux stores terminal lines as variable-length, copy-on-write cell prefixes.
Columns after the stored prefix are implicit default cells. A newly created
160-column line therefore owns no cell allocation, while a character written
at column 12 stores only the first 13 cells. Non-default blank cells remain
explicit because their background and attributes are terminal state.

Each line's cell vector is held behind `Arc`. Cloning a `Grid`, `Screen`, frozen
pane, render scene, or client baseline shares immutable line backing. The first
mutation to a shared line copies only that line. Client snapshots never share
mutable state with the authoritative server grid.

Styles use canonical 64-bit intern IDs without a lookup table. The complete
wmux style domain fits in 57 bits, so equal styles map bijectively to the same
ID and resolve without allocation, hashing, locking, or process-global state.
Long runs and scrollback therefore do not repeat full style structs in every
cell, and snapshots remain `Send` across the OS-neutral server boundary.

The public terminal behavior is unchanged. Wide-character continuation cells,
styled erases, wrapped-line reflow, alternate screens, damage generations, and
renderer diffs operate over the logical line width rather than the stored
prefix length.

## Correctness Gates

Core tests verify implicit tails, explicit styled blanks, style sharing,
line-level copy-on-write, and render-baseline sharing. The existing terminal
and renderer tests cover wide cells, reflow, styles, clear operations, split
scenes, frozen panes, and alternate screens. The benchmark replay gate compares
every visible cell produced by the batched parser against the legacy parser.

## Summary

Describe the user-visible problem and the resulting behavior.

## Reference model

Describe the tmux, Zellij, or official platform documentation inspected and
the model adopted by this change.

## Verification

List the exact commands and manual checks run.

## Checklist

- [ ] Product changes are in the canonical `wmux-clean/` workspace.
- [ ] Core code remains free of Windows and Unix APIs.
- [ ] Server-owned state is mutated only through serialized commands/events.
- [ ] The change preserves disposable clients and persistent pane processes.
- [ ] Tests cover the changed behavior or regression.
- [ ] Formatting, Clippy with warnings denied, and relevant tests pass.
- [ ] Documentation and compatibility evidence are updated where required.
- [ ] Windows, Linux, and macOS impact is described.
- [ ] Hot-path performance impact is measured or shown to be irrelevant.
- [ ] No build output, runtime state, credential, private key, or local agent file is included.

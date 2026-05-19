# Product Overview

## Vision

`wmux` is a native Windows terminal multiplexer inspired by tmux. The project
exists to bring persistent, session-based, pane-oriented terminal workflows to
Windows without requiring WSL.

The intended experience is simple:

```text
open a terminal
run wmux
enter a persistent multi-pane workspace
detach when needed
reattach later to the same running environment
```

The product should feel fast, durable, terminal-native, and familiar to tmux
users while still embracing Windows as the native environment.

## Problem

Advanced development workflows often require multiple long-running processes:

- coding agents
- development servers
- logs and monitoring panes
- build systems
- backtesting jobs
- infrastructure tools
- multiple active projects

On Linux and macOS, tmux solves this workflow well. On Windows, users often
fall back to WSL, many terminal tabs, fragile startup scripts, or repeatedly
rebuilding the same workspace after restarts.

`wmux` aims to provide the tmux-style workflow model directly on Windows.

## Core Capabilities

### Sessions, Windows, and Panes

The core model is:

```text
session
  windows
    panes
```

Users should be able to create sessions, create multiple windows per session,
split panes horizontally and vertically, and navigate quickly between panes and
windows.

### Detach and Reattach

The daemon owns shell processes and session state. A client can detach or exit
without killing the running workload.

Users should be able to close the visible terminal and later reattach to the
same session with processes still running.

### Mouse Support

Mouse support is a first-class workflow, not an afterthought. Users should be
able to focus panes, resize pane boundaries, and interact with copy mode using
mouse input where the surrounding terminal supports it.

### Copy Mode and Clipboard Integration

`wmux` should support scrollback navigation, text selection, copy mode, internal
paste buffers, and Windows clipboard integration.

## Non-Goals

`wmux` should not become:

- a terminal emulator product
- an IDE
- a GUI desktop application
- a terminal emulator replacement
- a cloud collaboration platform
- a distributed terminal system
- a complete tmux clone in the initial release

Internally, `wmux` will still need terminal-state handling for pane rendering,
scrollback, copy mode, and reattach. Product-wise, however, terminal rendering
surfaces remain the responsibility of existing terminal emulators.

## Intended Audience

`wmux` is for terminal-heavy users who work with many concurrent processes:

- backend engineers
- infrastructure engineers
- systems programmers
- quant developers
- AI-agent workflow users
- power users who prefer keyboard-centric workflows

The product is especially aimed at people who want durable terminal workspaces
that survive beyond one terminal window.


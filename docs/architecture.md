# KetPlus CM architecture

## Boundaries

```text
Qt application shell
    -> editor adapter
        -> Scintilla + Lexilla
        -> native persistent typography settings
    -> document core
    -> workspace explorer
        -> QFileSystemModel (lazy directory loading, no content index)
    -> Git service
        -> system Git CLI via short-lived asynchronous processes
    -> terminal panel (lazy)
        -> libvterm screen and keyboard model
        -> PTY process backend
            -> forkpty (macOS/Linux)
            -> ConPTY (future Windows backend)
```

- `src/app` owns windows, menus, tabs, and user interaction.
- `src/editor` adapts Scintilla to KetPlus CM concepts. Scintilla API calls must not
  leak into the application shell. Editor typography is normalized and persisted
  here, then shared with read-only diff views.
- `src/core` owns documents and persistence. It must not depend on Qt Widgets.
- `src/workspace` owns the lightweight file tree and filesystem actions. It
  discovers entries only when folders are expanded and does not build an index.
- `src/git` owns read-only repository discovery, status/diff requests, porcelain
  parsing, and linked-worktree discovery. It never stages, commits, or changes
  the checkout.
- `src/terminal` owns the embedded terminal surface, VT state, scrollback, and
  process transport. `PtyProcess` isolates platform process creation from the
  renderer so a ConPTY backend can be added without changing terminal UI code.
- Proprietary KetPlus features consume a pinned KetPlus CM revision and live in
  separate files and targets. They must not be added behind compile flags in
  this repository or implemented by modifying CM files without publishing those
  modifications under MPL 2.0.
- Terminal creation is lazy. Hiding its panel only detaches the visible area; it
  does not stop the shell. The first implementation retains one session, while
  the planned session registry will retain one or more sessions per worktree.

## Principles

1. Keep startup synchronous work minimal.
2. Load heavyweight services only when a feature needs them.
3. Keep parsing, indexing, and project search away from the UI thread.
4. Prefer process-based extensions over an unstable in-process C++ plugin ABI.
5. Measure startup time, memory, and large-file behavior before adding features.
6. Keep Git optional and read-only; terminal workflows remain the source of truth
   for operations that mutate a repository.

## Suggested milestones

1. Editing foundation: robust encodings, line endings, recent files, session
   restore, and drag-and-drop.
2. Workspace: file watcher, project search powered by ripgrep, and command
   palette. The non-indexing file tree is implemented.
3. Code intelligence: Tree-sitter and optional LSP processes.
4. Extensions: versioned JSON-RPC protocol with out-of-process plugins.

# KetPlus CM

<p align="center">
  <img src="assets/brand/ketplus-mark.png" alt="KetPlus CM logo" width="144">
</p>

KetPlus CM is the open-source, fast, lightweight, cross-platform editor at the
core of KetPlus for Windows, macOS, and Linux.

The first milestone deliberately stays small: reliable file editing, tabs,
UTF-8, syntax lexing, and a native desktop experience without a browser runtime.

## Stack

- C++20
- Qt 6 Widgets
- Scintilla editing engine
- Lexilla syntax lexers
- CMake

Scintilla, Lexilla, and libvterm are fetched automatically at configure time and
pinned to known revisions. The Inter variable font is bundled for the
application chrome.

## Prerequisites

- CMake 3.24+
- Ninja
- Qt 6.5+ with `Core5Compat`, `Svg`, and `SvgWidgets`
- A C++20 compiler
- Optional: `sccache` or `ccache` for persistent local compiler caching

On macOS with Homebrew:

```sh
brew install cmake ninja qt qt5compat
export QT_ROOT="$(brew --prefix qt)"
```

## Build and test

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

All presets share downloaded sources through `.cache/fetchcontent`. CMake uses
`sccache` or `ccache` automatically when either executable is available. Set
`-DKETPLUS_USE_COMPILER_CACHE=OFF` to disable the compiler cache.

Run the development build:

```sh
open "build/dev/KetPlus CM.app"   # macOS
./build/dev/ketplus-cm             # Linux
build\\dev\\KetPlusCM.exe        # Windows
```

## Use as a CMake dependency

KetPlus CM can also be embedded without building its application or tests:

```cmake
set(KETPLUS_CM_BUILD_APP OFF CACHE BOOL "" FORCE)
set(KETPLUS_CM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KETPLUS_CM_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/ketplus-cm EXCLUDE_FROM_ALL)

target_link_libraries(my_app PRIVATE
    KetPlusCM::core
    KetPlusCM::editor
    KetPlusCM::git
    KetPlusCM::git_ui
    KetPlusCM::preview
    KetPlusCM::settings_ui
    KetPlusCM::terminal
    KetPlusCM::ui
    KetPlusCM::workspace
)
```

Consumers should pin an immutable release commit or tag. `develop` is the
integration branch; `main` and `cm-v*` tags identify production revisions.

Mermaid blocks in Markdown preview are optional. To enable them, install the
official Mermaid CLI and restart KetPlus CM (or edit the document again):

```sh
npm install -g @mermaid-js/mermaid-cli
```

KetPlus CM also checks `KETPLUS_MMDC` when a custom `mmdc` executable path is needed.

## Current capabilities

- Create, open, edit, and atomically save files
- Multiple movable and closable tabs
- Undo, redo, cut, copy, paste, delete, and select all
- Find and replace with next/previous, case, whole-word, and replace-all modes
- Keyboard navigation between tabs
- Unsaved-change protection
- UTF-8 editing
- Scintilla-based editor surface
- Generic syntax highlighting for unknown source files (comments, strings, numbers, and common keywords)
- Language-aware highlighting for C/C++, JavaScript/TypeScript, Python, HTML/XML/PHP,
  CSS, JSON, Markdown, shell, YAML/TOML, SQL, Rust, Java, C#, Go, Swift, Kotlin,
  Ruby, Lua, Dart, Zig, CMake, Makefiles, Dockerfiles, properties, and diffs
- Live native Markdown preview with optional Mermaid diagrams rendered by `mmdc`
- Lazy workspace Explorer that reads directories on demand without indexing
- Single-click file opening plus create, rename, reveal, and move-to-Trash actions
- Read-only Git status with separate staged/working-tree lists and scoped diffs,
  powered by the installed Git CLI
- Styled inline Git diff tabs with line gutters and semantic added/removed colors
- Fast switching between linked Git worktrees without changing branches in place
- Lazy embedded terminal backed by a real PTY and libvterm on macOS and Linux;
  hiding the panel keeps the shell session alive
- KetJS-inspired compact visual system with matching semantic color tokens
- System, light, and dark appearance modes under `View > Appearance`
- Persistent editor font, font-size, and line-height settings under `Settings…`
  (`Ctrl/Cmd+,`)
- Open files passed on the command line
- Open workspace folders from the command line or with `Ctrl/Cmd+K, Ctrl/Cmd+O`
- Open or focus the embedded terminal with Ctrl/Cmd+Backtick and
  Ctrl/Cmd+Shift+Backtick

See [docs/architecture.md](docs/architecture.md) for project boundaries and the
next milestones. See [docs/design-system.md](docs/design-system.md) for the
desktop adaptation of the KetJS visual language.

Performance baselines and the repeatable Release-mode benchmark suite live in
[benchmarks](benchmarks/README.md).

## License

KetPlus CM is licensed under the Mozilla Public License 2.0. The full KetPlus
desktop product, AI features, account and entitlement code, KetRouter
integration, remote sessions, and daemon are developed separately and are not
covered by this repository's license.

Third-party components retain their upstream licenses. See
[docs/licensing.md](docs/licensing.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

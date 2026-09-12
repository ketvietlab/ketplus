# Compatibility risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Headers are exposed from `src/` build-tree paths, not a stable installed `include/` tree. | Consumers may depend on private layout or break when files move. | Keep Wave 1 docs explicit; add install/export package as a later release slice. |
| `ThemePalette` stores concrete colors, not token ids. | Consumers can treat observed values as canonical and resist KDS updates. | Introduce value snapshots with source revision metadata; document drift and QA status. |
| QSS role names are de facto public because tests and consumers can find them. | Renaming `kvRole` selectors can break downstream UI composition. | Promote only reviewed roles; keep internal roles undocumented until stabilized. |
| `SettingsDialog::addPage` currently accepts raw page pointers and only id/title. | Page id collisions or unclear ownership can make composed settings brittle. | Proposed registration API validates ids, ownership, order, and duplicate handling. |
| Save/Cancel/Reset behavior spans built-in and extension pages. | A private page could persist on Cancel or skip reset semantics. | Contract requires buffered pages, dirty reporting, and no persistence before Save. |
| Existing editor-only settings constructor is still public. | Consumers may miss newer appearance-wide settings. | Keep compatibility but document preferred constructor and signal. |
| Git service is read-only today, while linked-worktree workflows can imply mutation. | Accidental checkout/delete/prune behavior would violate public CM constraints. | Require explicit operation interface, blocking guards, cancellation, and disposable test repos. |
| Worktree identity can change between menu display and operation. | Opening or operating on the wrong branch/head. | Revalidate target path, branch, head, repository root, and lock state immediately before outcome. |
| Dirty/untracked policy varies by workflow. | Integrator uncertainty about whether a target is safe. | Surface dirty and untracked guards separately; let callers decide only after public API approval. |
| Visual measurements from current QSS may differ from KDS 0.1.7. | Premature screenshots/baselines could bless accidental drift. | Mark unresolved measurements Pending QA and avoid screenshot baselines in Wave 1. |
| Private concepts could leak through generic public extension points. | Public API becomes a container for product-specific names or behavior. | Ban private session, daemon, cloud, transcript, entitlement, and account semantics in CM contracts. |

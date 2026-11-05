# Proposed public interfaces

These interfaces are review proposals, not production APIs. Approval, runtime
implementation, visual approval, and release are separate gates.

## Native design-token snapshot

The native contract should expose resolved values without vendoring or changing
the canonical KDS source.

```cpp
namespace ketplus {

enum class NativeColorScheme { Light, Dark };

struct NativeColorTokens final {
    QString pageBackground;
    QString panelBackground;
    QString textMain;
    QString textSecondary;
    QString border;
    QString accent;
    QString focusBorder;
    QString positive;
    QString warning;
    QString danger;
    QString info;
};

struct NativeMetricTokens final {
    qreal controlHeightXs;
    qreal controlHeightSm;
    qreal controlHeightMd;
    qreal controlHeightLg;
    qreal sidebarItemHeight;
    qreal tableHeaderHeight;
    qreal tableRowHeight;
    qreal radiusXs;
    qreal radiusSm;
    qreal radiusMd;
    qreal radiusLg;
    qreal pagePaddingX;
    qreal pagePaddingY;
};

struct NativeTypographyTokens final {
    QString sansFamily;
    QString monoFamily;
    qreal textXl;
    qreal text2xl;
    int weightNormal;
    int weightMedium;
    int weightSemibold;
    int weightBold;
    qreal leadingTight;
    qreal leadingNormal;
    qreal leadingRelaxed;
};

struct NativeDesignTokens final {
    NativeColorScheme scheme;
    NativeColorTokens colors;
    NativeMetricTokens metrics;
    NativeTypographyTokens typography;
    bool reducedMotion;
};

} // namespace ketplus
```

Values are immutable snapshots. A resolver must expand `var()` aliases, select
the requested arm of `light-dark()`, and convert CSS alpha colours to an
equivalent `QColor` without compositing away alpha. Token provenance and unit
rules are specified in [token-metric-mapping.md](token-metric-mapping.md).
`ThemePalette` remains available during migration. A future QML consumer uses
the same resolved snapshot through a Qt-facing adapter; this proposal does not
add a QML dependency.

## Generic settings host

The host treats consumer identifiers as opaque. It reserves only documented
built-in identifiers and prefixes; it does not infer ownership or policy from a
consumer id.

```cpp
namespace ketplus {

struct SettingsPageDescriptor final {
    QString id;
    QString title;
    QString sectionId;
    int order{0};
};

enum class SettingsRegistrationResult {
    Registered,
    InvalidId,
    DuplicateId,
    MissingPage,
    AlreadyParented,
    PointerAlreadyRegistered,
    MetadataMismatch,
};

struct SettingsPageRegistration final {
    SettingsPageDescriptor descriptor;
    SettingsPage* page{nullptr};
};

class SettingsHost {
  public:
    virtual ~SettingsHost() = default;
    virtual SettingsRegistrationResult
    registerPage(SettingsPageRegistration registration) = 0;
    virtual bool selectPage(const QString& id) = 0;
    [[nodiscard]] virtual QString selectedPageId() const = 0;
};

} // namespace ketplus
```

### Registration, identity, and ordering

- `descriptor.id` and `descriptor.title` are the source of truth after a
  successful registration. For the compatibility `addPage(SettingsPage*)`
  path, the adapter copies `SettingsPage::id()` and `title()` into a descriptor.
  Registration requires descriptor id/title to equal the page's id/title;
  a mismatch returns `MetadataMismatch` without changing either object.
- A valid id is non-empty, normalized lowercase slash-separated ASCII with no
  empty, `.` or `..` segment. This proposal reserves the existing built-in ids
  `general` and `appearance` only, with no reserved prefixes. A consumer attempt
  to use either returns `DuplicateId`; no unlisted prefix is special.
- Duplicate ids, the same page pointer registered twice, null pages, and pages
  already parented to any owner return the distinct result above and leave
  the widget, selection, and registration order unchanged.
- On success the host reparents the page to its page container and owns it by
  normal QObject parent-child lifetime. Registration failure transfers no
  ownership. Destroying the page removes its registration and connections;
  destroying the host destroys successfully registered pages and disconnects
  their signal connections through QObject lifetime rules.
  Registration and page access are GUI-thread-only. Validate in this order:
  null, already-registered pointer, existing parent, invalid id, duplicate id,
  metadata mismatch. On selected-page destruction select the first remaining
  page in display order (or report an empty selected id if none remains).
- Pages sort by section order (when sections are introduced), then descriptor
  `order`, then successful registration sequence. Titles and ids do not break
  ties, so translated titles cannot reorder pages.
- `selectPage(id)` returns `false` and preserves selection for an unknown id.
  Selection before registration is rejected; it is not queued.

### Current behavior and proposed behavior

The current `SettingsDialog` Save action calls `apply()` once on **every**
registered extension page, whether or not `hasChanges()` is true. The proposed
dirty-only behavior is a behavior change, not semantics-preserving forwarding.
Migration requires first documenting that `apply()` must be idempotent, then
adding diagnostics/tests for pages whose correctness relies on an unconditional
call. Dirty-only application can ship only in a separately approved slice;
until then the compatibility path retains apply-all behavior.

The proposed dialog contract is:

- Reset is dialog-wide: built-in buffered values and every registered page
  receive reset. Reset changes buffers only and does not call `apply()`.
- Save validates built-in buffers, then calls page application according to the
  compatibility mode above. With the current `void apply()` interface the host
  cannot detect an extension failure and cannot promise an atomic transaction
  across independent stores.
- Cancel, window close, and Escape all take the reject path. They do not call
  extension `apply()`. Closing ends the editing session; buffers may remain in
  hidden widgets until destruction. Reuse requires a fresh dialog/pages loaded
  from persisted state; rejection does not reset surviving page buffers. The existing
  `ThemeManager` preview is cancelled because that constructor has an explicit
  preview owner and rollback hook.
- Extension previews have no rollback guarantee in this proposal. Such a
  guarantee requires a separate hook plus an ownership/lifetime contract; until
  then an extension must keep preview side effects local or restore them itself
  on destruction/rejection.
- If page validation or recoverable apply failure is required later, introduce
  an explicit result/error API before changing Save flow. Do not reinterpret
  `void apply()` side effects as transactional validation.

Compatibility surface retained: both existing constructors,
`SettingsDialog::addPage(SettingsPage*)`, `settingsSaved`, and
`appearanceSettingsSaved`.

## Linked-worktree observation and operations

### Observed identity and state

Observation reports facts; each operation maps those facts to blockers and
warnings. A dirty, untracked, current, main, or locked worktree is not
intrinsically invalid. In particular, selecting/opening an already registered
workspace must continue preserving its tabs and unsaved work.

```cpp
namespace ketplus {

struct RepositoryIdentity final {
    QString canonicalCommonDir;
};

struct WorktreeTargetIdentity final {
    RepositoryIdentity repository;
    QString canonicalWorktreePath;
    QString canonicalGitDir;
    QString branch;
    QString head;
    bool detached{false};
    bool unborn{false};
};

struct WorktreeObservation final {
    WorktreeTargetIdentity target;
    bool registered{false};
    bool exists{false};
    bool mainWorktree{false};
    bool currentWorktree{false};
    bool locked{false};
    bool prunable{false};
    bool bare{false};
    bool dirtyTrackedFiles{false};
    bool untrackedFiles{false};
};

enum class WorktreeIssue {
    MissingTarget,
    UnregisteredTarget,
    CurrentWorktree,
    MainWorktree,
    Locked,
    Bare,
    Prunable,
    DirtyTrackedFiles,
    UntrackedFiles,
    HeadChanged,
    BranchChanged,
    PathReused,
    RepositoryIdentityChanged,
};

struct WorktreePreflightResult final {
    QString correlationId;
    WorktreeObservation observation;
    QVector<WorktreeIssue> blockingIssues;
    QVector<WorktreeIssue> warningIssues;
    QString diagnostic;
};

enum class WorktreeOperationStatus {
    Succeeded,
    Cancelled,
    TimedOut,
    FailedPreflight,
    FailedRevalidation,
    FailedExecution,
};

struct WorktreeOperationOutcome final {
    QString correlationId;
    WorktreeOperationStatus status;
    WorktreeTargetIdentity target;
    QVector<WorktreeIssue> issues;
    QString diagnostic;
};

class WorktreeOperation : public QObject {
    Q_OBJECT

  public slots:
    virtual void cancel() = 0;

  signals:
    void preflightReady(const ketplus::WorktreePreflightResult& result);
    void finished(const ketplus::WorktreeOperationOutcome& outcome);
};

enum class WorktreeOperationKind { SelectExistingWorkspace };

struct WorktreeOperationRequest final {
    WorktreeOperationKind kind;
    WorktreeTargetIdentity expectedTarget;
    QString correlationId;
    qint64 timeoutMilliseconds;
};

class WorktreeOperationFactory {
  public:
    virtual ~WorktreeOperationFactory() = default;
    virtual WorktreeOperation* create(const WorktreeOperationRequest& request,
                                      QObject* parent = nullptr) = 0;
};

} // namespace ketplus
```

### Path and repository identity

- A canonical path is absolute, cleaned, and symlink-resolved for every existing
  component. A missing final target retains a cleaned absolute spelling and is
  marked missing; it must not be treated as equivalent merely by string form.
- Path comparison follows the platform/filesystem behavior used by Git: case
  sensitive where the filesystem is case sensitive, case folded where it is
  not. Do not apply unconditional lowercase normalization.
- Repository identity is the canonical path returned by
  `git rev-parse --path-format=absolute --git-common-dir`, corroborated by the
  registered worktree's canonical git-dir. A valid linked worktree may be
  anywhere on the filesystem; descendant-of-main-path checks are invalid.
- Detached HEAD has a head object id and no branch. Unborn HEAD has no commit
  object id and may have a symbolic branch. These are distinct observed states.
- Bare repositories are observed but are not selectable as a workspace.
  Missing or prunable registrations remain observable diagnostics; they do not
  become valid merely because a path with the same spelling is recreated.
- Path reuse means the canonical target path now resolves to a different
  registered git-dir than preflight. Repository identity changed means its
  common-dir differs. Either is a revalidation failure.

### Per-operation policy

| Observed fact | Open/select existing workspace | Future mutation |
| --- | --- | --- |
| registered, exists, same repository identity | eligible | operation-specific |
| current or main worktree | allowed no-op/select; preserve UI state | normally block if mutation would target active/main state |
| dirty tracked or untracked files | warning only; preserve tabs/unsaved work | operation-specific blocker or warning |
| locked | warning for open/select | normally block operations that conflict with the lock |
| detached or unborn HEAD | warning, if otherwise selectable | operation-specific |
| missing, prunable, bare, unregistered, path reused, repository changed | block | block |

This table is the policy for the named operations only. A generic Git capability
that is deferred from this slice is not automatically outside public ownership;
when separately scoped, generic worktree behavior belongs in public CM while
consumer policy remains in the consumer.

### Factory, lifetime, threads, and completion

- A future factory accepts an immutable request containing the operation kind,
  target identity, a caller-generated correlation id, and a finite timeout. It
  returns one parentless `WorktreeOperation*`; the caller owns it unless a
  QObject parent is explicitly passed to the factory.
- Request validation failure still returns an operation that completes
  asynchronously, so callers have one completion model. Correlation is copied
  unchanged into every event/outcome; the public layer does not assign meaning
  to it.
- The object has affinity to the factory's thread. Public slots are invoked on
  that thread (queued when called cross-thread); signals are emitted from that
  thread. Blocking Git/process work must not run on the UI thread.
- While the operation remains alive, `finished` is the exactly-once terminal signal. After it, no preflight or other
  progress signal is emitted. Cancellation is idempotent and cooperative:
  before execution it yields `Cancelled`; after a terminal result it is a no-op;
  during a non-interruptible step the eventual truthful result may win.
- Timeout is measured from accepted request creation to terminal outcome. It
  requests cancellation and reports `TimedOut` unless another terminal outcome
  already won. Destroying the operation cancels owned work and emits no signal
  from the destructor; early destruction explicitly abandons delivery. Callers
  needing an outcome must cancel and retain the operation until `finished`, then
  use `deleteLater()`. Factory ownership of a workspace-selection adapter and its
  thread marshalling must be approved before implementing selection execution.

Preflight and immediate revalidation reduce stale-target risk but do not make
filesystem access atomic. State can change between the final check and use.
Implementations must keep commands explicitly scoped, preserve truthful failure
reporting, and avoid claiming TOCTOU safety without OS/Git primitives that
actually provide it.

Wave 1 remains read-only. No delete, checkout, prune, branch creation, commit,
rebase, or remote mutation is implemented here.

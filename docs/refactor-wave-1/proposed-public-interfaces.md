# Proposed public interfaces

These interfaces are proposals for review. They are not production APIs in Wave
1 and should not be consumed by private code until approved, released, and pinned
through the integration gate.

## Native design tokens

Goal: give native CM consumers a stable semantic surface without vendoring or
mutating the public Két Design System token source.

```cpp
namespace ketplus {

enum class NativeColorScheme {
    Light,
    Dark,
};

struct NativeColorTokens final {
    QString pageBackground;
    QString appBackground;
    QString sidebarBackground;
    QString panelBackground;
    QString panelSubtle;
    QString surfaceRaised;
    QString surfaceHover;
    QString textMain;
    QString textSecondary;
    QString textMuted;
    QString textDisabled;
    QString border;
    QString borderStrong;
    QString interactiveBackground;
    QString interactiveHover;
    QString interactiveActive;
    QString interactiveDisabled;
    QString accent;
    QString accentHover;
    QString accentActive;
    QString accentSubtle;
    QString accentMuted;
    QString accentBorder;
    QString focusBorder;
    QString positive;
    QString warning;
    QString danger;
    QString info;
};

struct NativeMetricTokens final {
    int controlHeightXs;
    int controlHeightSm;
    int controlHeightMd;
    int controlHeightLg;
    int sidebarItemHeight;
    int tableHeaderHeight;
    int tableRowHeight;
    int radiusXs;
    int radiusSm;
    int radiusMd;
    int radiusLg;
    int space1;
    int space2;
    int space3;
    int space4;
    int space5;
    int pagePaddingX;
    int pagePaddingY;
};

struct NativeTypographyTokens final {
    QString sansFamily;
    QString monoFamily;
    int text2xs;
    int textXs;
    int textSm;
    int textMd;
    int textBase;
    int textLg;
    int textXl;
    int weightNormal;
    int weightMedium;
    int weightSemibold;
    int weightBold;
    double leadingTight;
    double leadingNormal;
    double leadingRelaxed;
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

Contract:

- Source values map from public KDS semantic/component roles at revision
  `6f923d1e...`, with native fallbacks where Qt needs concrete colors.
- Consumers receive value snapshots; they do not mutate canonical tokens.
- `ThemeManager` may continue to expose `ThemePalette` during migration.
- Motion is represented as a boolean effective preference plus resolved
  durations in later slices if Qt animation APIs need them.

## Generic settings host

Goal: keep the existing `SettingsDialog` compatible while making page
registration stable and composable.

```cpp
namespace ketplus {

struct SettingsPageDescriptor final {
    QString id;
    QString title;
    QString sectionId;
    int order{0};
};

struct SettingsPageRegistration final {
    SettingsPageDescriptor descriptor;
    SettingsPage* page{nullptr};
};

class SettingsHost {
  public:
    virtual ~SettingsHost() = default;
    virtual bool registerPage(SettingsPageRegistration registration) = 0;
    virtual bool selectPage(const QString& id) = 0;
    [[nodiscard]] virtual QString selectedPageId() const = 0;
};

} // namespace ketplus
```

Semantics:

- Page ids are stable, public, lowercase slash-separated strings such as
  `appearance/theme` or `editor/typography`.
- Registration fails for duplicate ids, empty ids, missing widgets, or ids using
  a reserved private namespace.
- Save calls `apply()` on dirty pages after built-in appearance validation.
- Cancel never calls `apply()` and must restore preview state.
- Reset calls `resetToDefaults()` on the selected page group or all pages,
  depending on the dialog action chosen in a later UI slice.
- Compatibility remains: existing `SettingsDialog::addPage(SettingsPage*)`,
  editor-only constructor, `settingsSaved`, and `appearanceSettingsSaved` keep
  working while forwarding internally to the host.

Public CM must not include account, AI provider, entitlement, remote-control,
session, daemon, cloud, or transcript contracts merely to host a page.

## Safe linked-worktree operations

Goal: provide a generic public operation contract for selecting or preparing a
linked worktree while preserving explicit target identity and read-only defaults.

```cpp
namespace ketplus {

struct WorktreeTargetIdentity final {
    QString repositoryRoot;
    QString worktreePath;
    QString branch;
    QString head;
};

enum class WorktreeGuard {
    MissingTarget,
    OutsideRepository,
    CurrentWorktree,
    MainWorktree,
    Locked,
    Bare,
    Prunable,
    DirtyTrackedFiles,
    UntrackedFiles,
    HeadChanged,
    BranchChanged,
    RepositoryChanged,
};

struct WorktreePreflightResult final {
    WorktreeTargetIdentity target;
    QVector<WorktreeGuard> blockingGuards;
    QVector<WorktreeGuard> warningGuards;
    QString diagnostic;
};

enum class WorktreeOperationStatus {
    Succeeded,
    Cancelled,
    FailedPreflight,
    FailedRevalidation,
    FailedExecution,
};

struct WorktreeOperationOutcome final {
    WorktreeOperationStatus status;
    WorktreeTargetIdentity target;
    QVector<WorktreeGuard> guards;
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

} // namespace ketplus
```

Required behavior:

- Target identity is explicit and includes path, branch, and head observed at
  preflight time.
- Preflight validates existence, repository identity, current/main worktree
  status, locked/bare/prunable flags, dirty tracked files, and untracked files.
- The operation is asynchronous and cancellable before execution begins.
- The target is revalidated immediately before opening or mutating anything.
- Any later mutating operation must run only inside disposable test repositories
  unless a focused public scope explicitly approves it.
- Public CM never knows private sessions, daemon state, cloud records, task ids,
  product transcripts, or private branch naming rules.

Wave 1 recommended default:

- Implement only `preflight` and safe workspace selection around already-existing
  worktrees.
- Keep branch creation, checkout, commit, rebase, delete, prune, and remote
  operations outside public CM.

# UI and content language inventory

T30 audits the public repository. The contract is
[English UI, prompt-driven content](decisions/2026-11-18-english-ui-team-content.md).

## Sources and boundaries inspected

| Area | Selection/translation source | Treatment |
| --- | --- | --- |
| Shared team facade | `modules/remote/src/TeamLanguage.*`, Linguist resource/catalog | Remove process translator installation; English fixed formatting; retain catalogs and compatibility APIs |
| New Team | `TeamDialogs.*`: system uiLanguages, goal-letter detection, manual Language | Replace OS/detection selection with Follow prompt; explicit content-only preference and English helper |
| Team page/menu/dialogs | `TeamPage.cpp` scoped/ambient context, TeamMenus/TeamTask/TeamDecision helpers | Compatibility contexts cannot translate chrome; content strings displayed as stored |
| Sidebar filter | `SidebarFilterText.h` selected UI locale | Explicit English independent of OS/team; preserve search text |
| Settings/archive/machine status | SettingsUpdatesPage, SettingsStorageOverview, ArchivedSettingsPage, LinuxMachineModel | Explicit English date labels; RemoteActivitySection already uses English |
| Other desktop shell/core/Lite | Source literals and locale/translator entry points | No other translator loader found; proper product/font names remain; documents and terminal output are content |
| Agent workspace/provider setup | `modules/ai`, ProviderSetupGuide | English source chrome; ProviderSetupGuide already uses English locale |
| Browser/computer/remote modules | Translator/locale loading and Vietnamese UI-literal search | No additional Vietnamese UI loader found in public sources; external page/provider/OS text is not app chrome |
| Daemon system templates | `teams::say`, state/role names, fixed decisions/digests/validation | New fixed text English; stored records and original decision answers are preserved |
| Agent-authored team content | AgentInstructions, SessionHostTeamAgents context | Prompt and explicit content instructions first; legacy en/vi fallback only, all roles |
| Creation/persistence | Team default, TeamRules validation, SessionHostTeamsProtocol, Team serde | New auto default; explicit/legacy values round-trip; missing preference is auto, no historical text migration |
| Local/selected-machine/paired projection | TeamDirectory transport, TeamRemoteView, SessionHostTeamsRemote, remote/cloud fixtures | Strict paired whitelist now admits content preference with the same validation and revision fence; wire content projected unchanged, shared renderer English |
| Content parsing | TeamBranchNames, TeamVerdictLine, ResponseGuidance; sample Unicode tests | Language-aware content/identifier parsing is not UI translation; retain |

Searches covered `apps/`, `core/`, `modules/`: translator installation/loading, `.qm`
resources, QLocale/system UI-language selectors, language preference readers/writers and
Vietnamese literals. Proper nouns (for example font attribution), input fixtures, authored
content and the preserved translation catalog are intentionally not deleted.
Private clients/services are separate owners; this change does not implement or certify them.

## Validation scope

Native Qt fixtures cover English chrome with legacy Vietnamese team/content and Vietnamese
OS locale, menus, New Team, task/history, decisions, sidebar and two-column/narrow layouts.
Daemon fixtures cover default/explicit language validation, persisted content/history,
fixed English decisions and prompt-priority instructions. Paired fixtures verify projection
keeps the goal and content preference; selected-machine rendering uses the same facade.
Build, focused checks, render evidence, full Pro results and any original failure/retry are
recorded with the PR's exact head. No installed-app or hosted-provider behavior is inferred
from these fixtures.

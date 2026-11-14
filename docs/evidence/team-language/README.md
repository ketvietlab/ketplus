# Team language evidence

The offscreen `TeamPageLanguageTest` renders the actual team page at 1024 × 1500
and opens its task dialog. The test uses a synthetic daemon response and temporary
IniFormat settings, without a live team, provider or real application preferences.

- `team-page-vi.png`: Vietnamese team chrome and fixture Updates.
- `team-page-en.png`: English team chrome with the same user-authored fixture content.
- `team-task-vi.png`: the Vietnamese task dialog.

User-authored goals, titles, decision questions, member names and lane names are
preserved across language changes. The screenshots do not claim phone coverage.
The language suite checks the displayed fixed strings; the catalog check also
extracts current source texts through lupdate and rejects missing translations.

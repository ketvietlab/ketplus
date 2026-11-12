# Real composer toast placement

The `windowToastsStayAboveTheRealComposer` regression renders the production
AgentWorkspaceView and real kit Toast at 1000×760, in light and dark, offscreen.
It uses an isolated temporary INI settings directory, asserts the settings filename
is inside that directory and that no previous workspace root exists, and attaches
only a synthetic fixture text file. No provider, real workspace or installed app
is opened.

For each theme:
- `composer-*-before.png`: the same binary with the bottom reservation explicitly
  cleared in the test, reproducing the previous fixed-bottom placement over the input.
- `composer-*-after.png`: the real measured composer reservation restored; the toast
  is fully above the attachment and multi-line input with the recipe gap.
- `*-native-scene.png`: one original QQuickView frame per theme, shared by both captures.

QWidget::grab excludes embedded native QQuickView pixels on the offscreen platform.
The test therefore composes the original QQuickView frame and real QWidget toast
render at their actual window coordinates. These are captures of production Qt
components, not a mocked or reconstructed interface. The raw scene frames are kept
alongside them so the capture method is reviewable.

The geometry assertions wait for a laid-out viewport, check the initial exact gap,
verify the composer grows with a multi-line draft and attachment and the toast moves
up, retain separation after resize and reparenting/moving a new ancestor, and check
the ordinary bottom position returns
when the composer is hidden in transcript-only mode or the workspace is hidden. The before/after images use the grown composer.

Reproduce with `QT_QPA_PLATFORM=offscreen KETPLUS_TOAST_CAPTURES=<scratch>` and run
`ketplus_agent_shell_tests windowToastsStayAboveTheRealComposer` from a task build.
The mock and workspace-local QML toasts use the same reserved-region behavior.

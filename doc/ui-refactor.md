# UI refactor

On Linux, `DARKSPEAK_KDE_COLOR_SCHEME` defaults to ON when KF6 ColorScheme
6.6 or newer development files are found. Explicit ON requires that dependency;
OFF retains the Qt color-scheme hint implementation. The option and dependency
discovery are absent on other platforms. Flatpak explicitly enables it using
the KDE SDK. Ubuntu development package: `libkf6colorscheme-dev`; runtime
Light/Dark selection also needs the Breeze color schemes (`breeze`).

With KDE support, explicit Light/Dark selects the installed BreezeLight/BreezeDark
palette through KColorSchemeManager; Automatic restores its system scheme and
runtime following. A missing palette falls back to the Qt hint and existing
unavailable-variant notice. KDE autosaving is disabled so DarkSpeak's Settings
preview/Save/Cancel remains authoritative. Only native palette data is consumed;
no external QML, scripts, fonts, or peer resources are loaded by this integration.
This presentation change adds no MCP interface.

Controls startup explicitly selects Fusion before QML imports, even when the
desktop exports `QT_QUICK_CONTROLS_STYLE=org.kde.desktop`. The KDE controls style
uses Kirigami's native colors independently of the application palette and can
leave dark custom dialogs with dark text and light input controls. Native still
gets its palette through the optional KDE integration; only the controls
implementation is fixed. Tests deliberately supply the KDE style environment
and check actual heading, combo-box, and footer-button colors in addition to
container palettes. Screenshot captures include the open Settings dialog.

Theme validation now covers all six custom themes in Automatic/Light/Dark,
body and metadata contrast on every primary/message surface (at least 4.5:1),
selected/status text contrast, persistence, invalid theme IDs, and system
palette changes. Quick Test switches themes on retained pages and an already-open
Settings dialog at 20/50/100/150/200%, checks palette propagation, and verifies
Cancel. Set `DARKSPEAK_UI_CAPTURE_DIR` when running UI tests to retain mock gallery
and shell screenshots outside the disposable test profile. The interactive UI
preview also has Theme and Appearance selectors.

This implementation follows `darkspeak-ui-design-spec.md`. All seven themes are
now available: Native, Coffee, Late Night, Rosewood, Windows, Matrix, and Inferno.
Manual fingerprint verification and new translations remain outside this pass.
Log output stays in English.

## Implementation

- `Appearance` supplies event-driven palette/font tokens, trusted asset
  lookup, 20–200% scaling, reversible previews, and geometry/layout preferences.
  Automatic follows desktop changes. Unsupported Native variants are explained
  in Settings. Fusion remains the single controls implementation.
  Custom palettes and corner metrics are compiled data in `src/app/theme_data.h`.
  Coffee, Rosewood, Windows, and Inferno have Light/Dark variants; Late Night
  and Matrix are always dark, with an explanation in Settings. Automatic follows
  the system palette without observing the custom palette as a system preference.
  Theme IDs are validated against the compiled list and persisted as
  `appearance/theme`; Cancel restores theme, mode, and scale together.
  Custom themes retain the system font and bundled icon/placeholder fallbacks.
  Entire palettes propagate to retained pages, popups, and dialogs, including
  tooltip and disabled roles. Decorative accents do not change status colors.
  No network, content policy, decoder, or MCP behavior changes with a theme.
- The shell retains each primary page after its first activation. Only Home
  loads initially. Settings, About, contact-request review, and custom window
  sizing load on demand. Tor auto-connect is queued after shell creation.
- Settings forms retain drafts while changing sections. C++ validates and saves
  settings; closing without Save reverses appearance previews and discards
  edits. Autostart and folder changes happen only after Save validation. Six
  resource-limit controls now use the keys actually consumed by the backend.
- Conversations use a responsive split view with a narrow Back action, a
  persistent split width, selectable plain-text bubbles, and explicit delivery
  states. Protocol authentication is labeled separately from transport and
  manual verification. Drafts live in bounded C++ memory and are never persisted.
- The history reader uses a single worker-owned read-only SQLite connection per
  request, tuple cursors, and 100-row pages. It retains at most 500 nearby rows,
  with paging in both directions and a Latest action. Generation checks discard
  results for obsolete selections. Live changes and deletions are reconciled
  while reads are in flight. Message delegate reads perform no database I/O.
  File objects still resolve through the existing owning C++ FileManager, with
  each resolution batch bounded by the page size; this remains a performance
  measurement point for file-heavy histories.
- Identity-local image/video preview permissions default to disabled. They are
  local presentation preferences, independent of file acceptance and download,
  and are never advertised as wire capabilities. No new general-purpose media
  decoder, autoplay facility, or multimedia dependency has been introduced.
  Restricted bitmap avatars retain their existing pipeline.
- ASCII-only identities check known v3 text fields before application-envelope
  Unicode conversion, with fixed nesting and field/work budgets. Stored message,
  nickname, bio, notification, and file display text is reassessed on display.
  Suppression does not rewrite stored messages or actual filesystem names.
- File-offer affordances follow advertised peer permissions. New connections
  wait for current UserInfo before processing message/file queues. MIME checks
  also occur in C++ before creating a local outgoing offer.
- Identity/contact cards, logs, settings, profiles, editors, transfer cards, and
  secondary dialogs use shared appearance tokens. Logs support C++ filtering.
  QML resources are packaged with `qt_add_qml_module` for compilation/caching.

## Validation without building the application

The UI tests can be configured separately and require no Tor, OS vault, real
contacts, or application library build:

```sh
cmake -S tests/ui -B /tmp/darkspeak-ui-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/darkspeak-ui-tests -j2
ctest --test-dir /tmp/darkspeak-ui-tests --output-on-failure
```

For an interactive component review on your desktop, run
`/tmp/darkspeak-ui-tests/darkspeak_ui_preview`. It uses a temporary settings
profile and the generated message model; it cannot connect to real peers.

The fixtures exercise the actual shell, empty and populated primary pages,
retained page lifetimes, actual message/file delegates, Settings section changes
and Cancel, and the shared message components with a virtualized 10,000-row C++
model. Tests use software rendering and fail on QML runtime warnings. Scale
coverage includes 20%, 50%, 100%, 150%, and 200%; Ctrl-wheel routing is also tested.
Screenshots use disposable test directories and contain only generated fixtures.

C++ tests cover appearance preview persistence, canceled settings, invalid port
ranges without writes, geometry clamping, bounded MIME preferences, raw ASCII
validation including malformed lengths/varints, and stored-text restrictions.
SQLite tests cover bounded history pages, timestamp ties, overlapping message/file
IDs, conversation isolation, newer paging, and invalid requests.

## Review and measurements still required

The production Debug application and compiled QML resources have been built in
`/tmp/darkspeak-refactor-build` after explicit authorization to compile this
refactor. The full CTest suite covers nine test targets, including the existing
protocol/crypto/Tor configuration tests and the production QML smoke test. The
real history-model integration test also exercises stale asynchronous selections,
C++ drafts, paging/window eviction, local preview defaults, and current ASCII
policy using a disposable database and passphrase-protected test vault.

The production smoke test exposed synchronous notification-server D-Bus
introspection during startup. Notification calls now use asynchronous messages,
1500 ms timeouts, and at most eight pending notifications. Optional notification
failure cannot block delivery or shell creation. Headless QML smoke tests skip
system tray creation. Isolated profiles keep their managed DarkSpeak folder
inside the profile rather than writing to the real home directory.

Clean configuration also exposed the historical SafeKeeping pin being unavailable
in a shallow HEAD clone. That dependency now uses a full checkout; its pinned
revision is unchanged, and the QR dependency still uses a shallow tagged checkout.

Real desktop identity/contact, message, transfer, tray, notification, and secure
OS-vault workflows still need interactive review; disposable tests do not replace
those checks.

Review Native on the target desktop in Automatic/Light/Dark, at the five tested
scales, with window presets, narrow widths, keyboard navigation, high DPI, and
long Norwegian/Bulgarian-style fixture text. No translation catalogs were added.
System palette/font changes need a real desktop check in addition to the mock
harness. File offers must remain disabled when peer permission is unknown or
forbidden. Media preview toggles must never activate an unsupported decoder.

Performance constraints are best effort, not numerical release gates. On the
older Celeron, record cold/warm time to the first shell frame, first page visits
and revisits, resident memory, idle CPU, and scrolling in message-heavy and
file-heavy histories. Compare the production build before/after under the same
profile and renderer. The mock harness establishes structural virtualization;
it is not a substitute for production startup or Celeron measurements.

MCP impact: these are presentation/preferences changes using the existing domain
interfaces. No MCP service or tool is introduced or extended.

## Local performance sample

A Debug build on this host, using the software renderer, an empty disposable
profile, test-vault credentials, disabled Tor auto-connect, and unavailable
D-Bus services produced:

| Profile | First shell frame | Resident memory |
|---|---:|---:|
| Fresh | 230 ms | 133.6 MiB |
| Reopened | 223 ms | 134.0 MiB |

No additional process CPU ticks were observed during either one-second idle
sample. This is a short empty-profile sample, not a guarantee of zero CPU, a
cold filesystem-cache benchmark, an older-Celeron result, or a comparison with
the pre-refactor application. Interactive OS-vault unlock and real conversations
still need the desktop measurements described above.

The verified full build is at `/tmp/darkspeak-refactor-build/bin/darkspeak`;
the interactive fixture preview is
`/tmp/darkspeak-refactor-build/bin/darkspeak_ui_preview`.

## Follow-up fixes

The toolbar scale selector is now a narrower (72 logical pixels at 100%),
read-only dropdown. Its left 60% consumes single clicks and resets to 100% on
double-click; the arrow area opens the dropdown. Ctrl+0 and View → Scale also
reset to 100%. Values reached with zoom shortcuts remain visible even when
they are not dropdown presets.

The initial People search previously bound a null QString as SQL NULL, causing
its empty-search predicate to match no contacts. Empty searches now omit the
predicate entirely. A real-model regression test covers initial loading,
filtering, and clearing the search. The supplied database copy was inspected
read-only; only row counts were used for diagnosis.

The scale selector and zoom buttons now use custom Qt Quick template visuals,
with explicit scaled dimensions, fonts, padding, indicator, and popup rows.
UI tests verify their geometry from 20–200% (allowing layout pixel rounding),
open the dropdown at each scale, and select a preset by mouse. Navigation uses
matching icon-only collapse/expand controls with action-specific tooltips and
accessible names; its toggle is covered by a click test.

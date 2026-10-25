# Release checklist

Manual items that are not automated. Run on a clean Windows 11 22H2+ x64 machine unless noted. Automated gates first:
`pwsh scripts/build.ps1 -Project All`, `pwsh scripts/test.ps1`, `scripts/perf-acceptance.ps1` (all PASS), CI green on the tag.

## Explorer and hook
- [ ] Space opens the preview: single Explorer window, several tabs, desktop with the setting on and off.
- [ ] Esc and Space close it; Space in rename (F2) and in the address/search box does not trigger it.
- [ ] Rename and type-ahead search work normally, with EN and RU keyboard layouts.
- [ ] Left/Right navigation changes the preview and the Explorer selection stays in sync; Up/Down in grid and list views.
- [ ] Explorer restart (kill `explorer.exe`): the hook recovers and Space works again.
- [ ] Sleep/resume and lock/unlock: Space still works afterwards.
- [ ] Elevated and non-elevated Explorer windows both work (or the limitation is understood and documented).
- [ ] PowerToys Peek enabled: FastLook shows the conflict warning; with Peek off the warning is gone.

## Formats
- [ ] Every file in `tests/fixtures` previews correctly (images incl. all EXIF orientations, SVG, GIF, APNG, animated WebP, text encodings, Markdown, PDF, media, archives, unknown file).
- [ ] Password-protected PDF and encrypted zip show the expected prompt/message.
- [ ] Office preview with a real `.docx` (and `.xlsx`/`.pptx` if available) through the shell handler.
- [ ] Broken/corrupt media and bogus images show an error, not a crash.

## Window and actions
- [ ] Open, Open with (the OpenWith dialog gets focus), Show in folder, Copy, Pin.
- [ ] Mouse selection, double-click word and Ctrl+C in text and code.
- [ ] After clicking into a shell-handler preview (font, Office), Esc, Space and Left/Right still work.
- [ ] Zoom (Ctrl +/-/0), F11 full screen.
- [ ] Multi-monitor with mixed DPI: the window opens on the right monitor at the right size, text stays sharp when dragged between monitors.

## Settings and updates
- [ ] Settings persist across restart; language switch applies after restart (RU and EN complete, no missing strings).
- [ ] Theme: light, dark, system.
- [ ] Check for updates: no release / no newer release, newer release (download and install), hash mismatch is rejected.
- [ ] Real GitHub update check against a published release (redirect chain works) once a release exists.

## Installer and packaging
- [ ] Fresh install (per-user); app starts, tray icon present.
- [ ] Autostart option on and off.
- [ ] Upgrade over an existing install and over a running instance; autostart preserved.
- [ ] Uninstall: the data prompt defaults to No (keep settings); answering Yes removes settings.
- [ ] Portable zip: contains `portable.txt` and `THIRD_PARTY_NOTICES.txt`, runs from an arbitrary folder, writes settings beside the exe.
- [ ] `.sha256` files match the artifacts.
- [ ] ProductVersion is `1.0.0` for FastLook.exe, the Viewer and PreviewHost.
- [ ] SmartScreen behaviour on a clean machine matches the README note.

## Soak
- [ ] 24 h soak with periodic Space presses: no crash, no growth in the memory figures, hook still alive at the end.

## Release
- [ ] README performance section filled from the perf-acceptance report.
- [ ] Tag `v1.0.0`, release assets uploaded with `.sha256`.

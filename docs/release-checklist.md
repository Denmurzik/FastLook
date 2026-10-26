# Release checklist

Manual items that are not automated. Run on a clean Windows 11 22H2+ x64 machine unless noted. Automated gates first:
`pwsh scripts/build.ps1 -Project All`, `pwsh scripts/test.ps1`, `scripts/perf-acceptance.ps1` (all PASS). There is no CI: the release is built and
published locally.

Release sequence (local):
1. `pwsh scripts/package.ps1 -Clean` (clean `out\bin\Release`, build, stage, smoke, zip, installer). Confirm in the output that `iscc` was found
   (no "Inno Setup (iscc) not found" warning) and that `out\dist` holds exactly four files: `FastLook-1.0.0-portable-x64.zip`,
   `FastLook-Setup-1.0.0.exe` and their `.sha256`.
2. Optionally keep `out\bin\Release\*.pdb` zipped (not published) to symbolise crash dumps of this version.
3. `gh release create v1.0.0 <the four files from out\dist>` (name them explicitly).

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
- [ ] A rar, a cab and an iso file list their contents in the archive previewer.
- [ ] Broken/corrupt media and bogus images show an error, not a crash.

## Window and actions
- [ ] Open, Open with (the OpenWith dialog gets focus), Show in folder, Copy, Pin.
- [ ] Mouse selection, double-click word and Ctrl+C in text and code.
- [ ] After clicking into a shell-handler preview (font, Office), Esc, Space and Left/Right still work.
- [ ] Zoom (Ctrl +/-/0), F11 full screen.
- [ ] Esc in the update ContentDialog closes it (deferred from Task 24).
- [ ] Multi-monitor with mixed DPI: the window opens on the right monitor at the right size, text stays sharp when dragged between monitors.

## Settings and updates
- [ ] Settings persist across restart; language switch applies after restart (RU and EN complete, no missing strings).
- [ ] Theme: light, dark, system.
- [ ] Check for updates: no release / no newer release, hash mismatch is rejected. "Newer release (download and install)" cannot be tested until a newer
  release exists: post-release, or against a `FASTLOOK_UPDATE_API` test feed.
- [ ] Real GitHub update check against a published release (redirect chain works) once a release exists.

## Installer and packaging
- [ ] Inno Setup 6.3+ was used to build the installer (the `x64compatible` identifiers; the local install is 6.6).
- [ ] Fresh install (per-user); app starts, tray icon present.
- [ ] Autostart option on and off.
- [ ] Upgrade over an existing install and over a running instance (interactive); autostart state preserved (off stays off, the task checkbox shows the current state).
- [ ] Silent upgrade, as the in-app updater runs it: `FastLook-Setup-1.0.0.exe /SILENT /SUPPRESSMSGBOXES /NORESTART` over a *running* installed 1.0.0
  with autostart switched OFF in Settings. Expected: FastLook is stopped, files are replaced, autostart stays off, FastLook relaunches.
  A `Setup Log ....txt` appears in `%TEMP%` (SetupLogging=yes).
- [ ] "Still running" box: block the stop (e.g. a hung FastLook), interactive upgrade, click Cancel: Setup stops on the Preparing page, nothing is deleted.
- [ ] Uninstall while FastLook is running, interactive and `/SILENT`: it is stopped first, all files go, `%TEMP%\FastLook-Update` is gone; a portable copy's autostart value is untouched.
- [ ] No directory page in the installer (fixed path); a fresh install does not touch other files in the folder.
- [ ] Uninstall: the data prompt defaults to No (keep settings); answering Yes removes settings.
- [ ] Portable zip: contains `portable.txt` and `THIRD_PARTY_NOTICES.txt`, runs from an arbitrary folder, writes settings beside the exe.
- [ ] `.sha256` files match the artifacts.
- [ ] ProductVersion is `1.0.0` for FastLook.exe, the Viewer and PreviewHost.
- [ ] SmartScreen behaviour on a clean machine matches the README note.

## Soak
- [ ] 24 h soak with periodic Space presses: no crash, no growth in the memory figures, hook still alive at the end.

## Release
- [ ] README performance section filled from the perf-acceptance report.
- [ ] Tag `v1.0.0`, release assets (the four files above) uploaded with `.sha256`.

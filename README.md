# FastLook

[Русский](#русский) | [English](#english)

---

## Русский

### Что это и зачем

FastLook — нативный предпросмотр файлов по пробелу для Windows 11 в духе macOS Quick Look. Выделите файл в Проводнике, нажмите Пробел — окно предпросмотра открывается мгновенно; Esc или Пробел закрывает его.

- C++/WinRT + WinUI 3, без WebView и без встроенного браузера.
- Малое потребление ОЗУ в простое: тяжёлое окно просмотра выгружается, остаётся лёгкий фоновый процесс.
- Самовосстанавливающийся хук: если Проводник перезапускается или хук теряется, он возвращается сам.
- Стиль Windows 11 (Mica, Fluent), светлая и тёмная темы, русский и английский интерфейс.

### Возможности и форматы

- **Изображения:** PNG, JPEG (с учётом EXIF-ориентации), GIF, APNG, WebP (в том числе анимированный), BMP, ICO, SVG и др.
- **Текст и код:** кодировки UTF-8/16, CP1251 и др.; подсветка синтаксиса, перенос строк, выделение мышью, двойной щелчок по слову, Ctrl+C.
- **Markdown:** отрисовка с таблицами и локальными картинками, переключение на исходник.
- **PDF:** постраничный просмотр, масштаб, PDF с паролем.
- **Видео и аудио:** воспроизведение, автозапуск настраивается.
- **Архивы:** zip, 7z, rar, tar, gz, cab, iso и др. — список содержимого без распаковки.
- **Документы Office и другие типы с обработчиками Проводника:** через предпросмотр оболочки Windows (если Office установлен).
- **Папки и неизвестные файлы:** сведения о файле.
- Действия: Открыть, Открыть с помощью, Показать в папке, Копировать, Закрепить окно (Pin).

### Требования

Windows 11 22H2 или новее, x64.

### Установка

- **Установщик** (`FastLook-Setup-<версия>.exe`) — установка для текущего пользователя, права администратора не нужны; можно включить автозапуск.
- **Portable-архив** (`FastLook-<версия>-portable-x64.zip`) — распакуйте в любую папку и запустите `FastLook.exe`. Файл `portable.txt` в папке включает портативный режим: настройки и журнал хранятся рядом с программой.

**SmartScreen.** Бинарные файлы не подписаны цифровой подписью, поэтому SmartScreen может показать предупреждение: нажмите «Подробнее» → «Выполнить в любом случае». Проверить целостность можно по файлу `.sha256` (формат `<хеш>  <имя файла>`), лежащему рядом с загрузкой (`Get-FileHash <файл> -Algorithm SHA256`).

**PowerToys Peek.** Peek тоже может реагировать на Пробел. Отключите его в PowerToys → Peek (или смените его сочетание); при обнаружении конфликта FastLook показывает предупреждение.

### Управление

| Клавиша | Действие |
|---|---|
| Пробел / Esc | Показать / закрыть предпросмотр |
| ← / → | Предыдущий / следующий файл (выделение в Проводнике синхронизируется) |
| ↑ / ↓ | Навигация по сетке/списку; прокрутка в тексте и PDF |
| Enter | Открыть файл в приложении по умолчанию |
| Ctrl+C | Копировать выделенный текст, а без выделения — файл |
| Ctrl + / Ctrl − / Ctrl 0 | Увеличить / уменьшить / сбросить масштаб |
| F11 | Полноэкранный режим |
| Pin | Закрепить окно (не закрывается при смене выделения) |

### Настройки

Окно настроек открывается из значка в трее. Разделы: **Общие** (клавиша активации, предпросмотр на рабочем столе, запуск при входе, предупреждение о Peek, прогрев, выгрузка в простое, тема, язык), **Просмотр** (перенос строк, размер шрифта кода, автозапуск медиа, обработчики оболочки, режим Markdown), **О программе** (версия, проверка обновлений, GitHub, лицензии, журналы). Язык применяется после перезапуска.

### Конфиденциальность

FastLook не обращается к сети, кроме ручной проверки обновлений (кнопка в настройках): запрос к GitHub Releases этого репозитория, без телеметрии и без отправки данных о ваших файлах. Автоматических проверок нет.

### Известные ограничения

- Текст внутри SVG (`<text>`) не отображается (Direct2D).
- Анимированные изображения больше 3 Мп показывают только первый кадр.
- Подсветка синтаксиса — до 8 МиБ / 500 000 строк, дальше обычный текст.
- Markdown рисуется до 2 МиБ / 4000 блоков, иначе показывается исходник.
- Страницы PDF рендерятся не выше 2 Мп: при сильном увеличении картинка мягче.
- Архивы 7z с огромными заголовками не открываются.
- Предпросмотр Office выполняется в процессах самого Office, вне цифр потребления памяти FastLook.
- Бинарные файлы не подписаны (см. SmartScreen выше).

### Производительность

Замерено 2026-10-26 скриптом `scripts/perf-acceptance.ps1` (Release, версия 1.0.0).

| Метрика | Цель | Замер |
|---|---|---|
| ОЗУ, только демон в простое (холодный) | ≤ 5 МБ | 0,2 МБ |
| ОЗУ в простое с прогретым Viewer | ≤ 25 МБ | 1,8 МБ |
| Space → окно (тёплый), p50 | ≤ 50 мс | 16,5 мс |
| Space → текст показан (тёплый), p50 | ≤ 150 мс | 34,5 мс |
| Space → изображение 24 Мп (JPEG) показано (тёплый), p50 | ≤ 150 мс | 176,8 мс (не уложились) |
| Space → окно (холодный старт), p50 | ≤ 400 мс | 432 мс (не уложились) |
| ОЗУ при просмотре фото 24 Мп (демон + Viewer) | ≤ 120 МБ | 29,7 МБ |
| 1000 открытий: рост ОЗУ в простое (Private WS) | ≤ 2 МБ | 0,0 МБ |
| 1000 нажатий Space: пропущено / падений | 0 / 0 | 0 / 0 |
| Хук, путь Space, p99 | ≤ 250 мкс | 212 мкс |

Замеры сделаны на ноутбуке (графика Intel, iGPU) от сети при другой нагрузке (фоновая загрузка ЦП около 20–35 %), поэтому задержки завышены. Показ JPEG 24 Мп (около 177 мс) выше цели в 150 мс — известное ограничение v1.0 (декодирование JPEG в WIC). Холодный старт (p50 432 мс против 400 мс) измерен под той же нагрузкой; сравнение A/B не показало регрессии в коде. Private Bytes Viewer могут расти при активной работе с видео; выше 400 МБ Viewer перезапускается сам.

### Сборка из исходников

Нужны: Visual Studio Build Tools 18.x с рабочей нагрузкой C++ (MSVC v145) и компонентом «vcpkg package manager» (либо переменная `VCPKG_ROOT` с готовым vcpkg), Windows SDK 10.0.26100, доступ к NuGet, PowerShell 7. Для установщика — Inno Setup 6.3 или новее.

```powershell
pwsh scripts/build.ps1 -Project All
pwsh scripts/test.ps1
```

Версия задаётся в одном месте — `src/Common/Version.h`. Упаковка (чистая сборка, портативный zip и установщик в `out/dist`): `pwsh scripts/package.ps1 -Clean`.

### Лицензия

MIT, см. [LICENSE](LICENSE). Лицензии сторонних компонентов — в [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Чек-лист релиза — [docs/release-checklist.md](docs/release-checklist.md).

---

## English

### What it is and why

FastLook is a native Space-bar file previewer for Windows 11 in the spirit of macOS Quick Look. Select a file in Explorer, press Space and the preview window opens instantly; Esc or Space closes it.

- C++/WinRT + WinUI 3, no WebView and no embedded browser.
- Low RAM at idle: the heavy viewer window is unloaded and only a light background process remains.
- Self-healing hook: if Explorer restarts or the hook is lost, it comes back on its own.
- Windows 11 look (Mica, Fluent), light and dark themes, Russian and English UI.

### Features and supported formats

- **Images:** PNG, JPEG (EXIF orientation honoured), GIF, APNG, WebP (including animated), BMP, ICO, SVG and more.
- **Text and code:** UTF-8/16, CP1251 and other encodings; syntax highlighting, word wrap, mouse selection, double-click word, Ctrl+C.
- **Markdown:** rendered with tables and local images, switchable to source.
- **PDF:** paged view, zoom, password-protected PDFs.
- **Video and audio:** playback, autoplay is configurable.
- **Archives:** zip, 7z, rar, tar, gz, cab, iso and more — contents listed without extracting.
- **Office documents and other types with Explorer preview handlers:** through the Windows shell preview (when Office is installed).
- **Folders and unknown files:** file details.
- Actions: Open, Open with, Show in folder, Copy, Pin.

### Requirements

Windows 11 22H2 or newer, x64.

### Install

- **Installer** (`FastLook-Setup-<version>.exe`) — per-user install, no administrator rights; autostart can be enabled.
- **Portable zip** (`FastLook-<version>-portable-x64.zip`) — extract anywhere and run `FastLook.exe`. A `portable.txt` file next to the exe enables portable mode: settings and logs are kept beside the program.

**SmartScreen.** The binaries are unsigned, so SmartScreen may show a warning: click "More info" → "Run anyway". Verify integrity with the `.sha256` file (format `<hash>  <file name>`) published next to the download (`Get-FileHash <file> -Algorithm SHA256`).

**PowerToys Peek.** Peek can also react to Space. Turn it off in PowerToys → Peek (or change its shortcut); FastLook shows a warning when it detects the conflict.

### Keys

| Key | Action |
|---|---|
| Space / Esc | Show / close the preview |
| ← / → | Previous / next file (the Explorer selection stays in sync) |
| ↑ / ↓ | Grid/list navigation; scrolling in text and PDF |
| Enter | Open the file in its default app |
| Ctrl+C | Copy selected text, or the file when nothing is selected |
| Ctrl + / Ctrl − / Ctrl 0 | Zoom in / out / reset |
| F11 | Full screen |
| Pin | Pin the window (it stays when the selection changes) |

### Settings

The settings window opens from the tray icon. Sections: **General** (activation key, preview on the desktop, launch at sign-in, Peek warning, prewarm, idle unload, theme, language), **Preview** (word wrap, code font size, media autoplay, shell handlers, Markdown mode), **About** (version, update check, GitHub, licences, logs). A language change takes effect after restart.

### Privacy

FastLook uses the network only for the manual update check (a button in settings): one request to this repository's GitHub Releases, no telemetry, and nothing about your files is sent. There are no automatic checks.

### Known limitations

- SVG `<text>` is not rendered (Direct2D).
- Animated images larger than 3 MP show the first frame only.
- Syntax highlighting up to 8 MiB / 500 000 lines; larger files show plain text.
- Markdown is rendered up to 2 MiB / 4000 blocks, otherwise the source is shown.
- PDF pages are rendered at no more than 2 MP, so the image is softer at high zoom.
- 7z archives with huge headers are refused.
- Office previews run in Office's own processes, outside FastLook's memory figures.
- The binaries are unsigned (see SmartScreen above).

### Performance

Measured on 2026-10-26 with `scripts/perf-acceptance.ps1` (Release, version 1.0.0).

| Metric | Target | Measured |
|---|---|---|
| Daemon-only idle RAM (cold) | <= 5 MB | 0.2 MB |
| Warm-idle RAM with the prewarmed Viewer | <= 25 MB | 1.8 MB |
| Space -> window (warm), p50 | <= 50 ms | 16.5 ms |
| Space -> text rendered (warm), p50 | <= 150 ms | 34.5 ms |
| Space -> 24 MP JPEG rendered (warm), p50 | <= 150 ms | 176.8 ms (missed) |
| Space -> window (cold start), p50 | <= 400 ms | 432 ms (missed) |
| RAM while previewing a 24 MP photo (daemon + Viewer) | <= 120 MB | 29.7 MB |
| 1000 opens: warm-idle RAM growth (Private WS) | <= 2 MB | 0.0 MB |
| 1000 Space presses: missed / crashes | 0 / 0 | 0 / 0 |
| Hook callback, Space path, p99 | <= 250 us | 212 us |

Measured on a laptop (Intel iGPU) on AC power with other workloads running (about 20-35 % background CPU), so latencies are pessimistic. The 24 MP JPEG render (about 177 ms) exceeds the 150 ms target: a known v1.0 limitation (WIC JPEG decode). Cold start (p50 432 ms vs the 400 ms target) was measured under the same load; an A/B run showed no code regression. The Viewer's Private Bytes can grow with heavy video use; above 400 MB the Viewer restarts itself.

### Build from source

Requires: Visual Studio Build Tools 18.x with the C++ workload (MSVC v145) and the "vcpkg package manager" component (or `VCPKG_ROOT` pointing at a vcpkg checkout), Windows SDK 10.0.26100, NuGet access, and PowerShell 7. The installer additionally needs Inno Setup 6.3 or newer.

```powershell
pwsh scripts/build.ps1 -Project All
pwsh scripts/test.ps1
```

The version lives in one place: `src/Common/Version.h`. Packaging (clean build, portable zip and installer in `out/dist`): `pwsh scripts/package.ps1 -Clean`.

### License

MIT, see [LICENSE](LICENSE). Third-party licences are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Release checklist: [docs/release-checklist.md](docs/release-checklist.md).

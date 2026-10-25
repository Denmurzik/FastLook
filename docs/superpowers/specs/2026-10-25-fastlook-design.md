# FastLook — спецификация дизайна

Дата: 2026-10-25 · Статус: утверждено в grill-сессии · Репозиторий: https://github.com/Denmurzik/FastLook

## 1. Цель

Быстрый, лёгкий и надёжный предпросмотр файлов по пробелу в Проводнике Windows 11 — замена PowerToys Peek в духе macOS Quick Look.

Проблемы Peek, которые решаем:
1. **ОЗУ:** Peek держит WebView2 (6+ процессов Edge) — у нас нет ни одного WebView/браузерного движка.
2. **«Перестаёт откликаться»:** low-level keyboard hook молча снимается Windows при медленном callback; COM-вызовы в зависший Explorer/preview handler вешают UI. У нас хук изолирован и самовосстанавливается, все внешние вызовы — с таймаутом и в изолированных потоках/процессах.

## 2. Решения (зафиксированы с владельцем)

| Тема | Решение |
|---|---|
| Стек | C++20, C++/WinRT, WinUI 3 (Windows App SDK), unpackaged, self-contained, x64 |
| ОС | Windows 11 22H2+ (Windows 10 не поддерживается) |
| Модель процесса | Тёплый резидент + трим памяти после закрытия + полная выгрузка XAML после простоя (по умолчанию 30 мин, настраивается) |
| Форматы | Изображения, текст/код, видео/аудио, PDF, Markdown, Office и прочее через shell preview handlers, архивы, папки, фолбэк |
| Триггер | Пробел в Проводнике; на рабочем столе — опционально (настройка) |
| Навигация | Как в macOS: ←/→ по выделенным (если выделено >1) иначе по всей папке в порядке Проводника; выделение в Проводнике синхронизируется; соседи предзагружаются |
| Окно | Размер по контенту (≤80% рабочей области), центр монитора с Проводником, Mica, компактный заголовок, анимация появления от иконки файла |
| Закрытие | Пробел / Esc. Потеря фокуса не закрывает. Пробел в Проводнике на другом файле — переключает превью. «Закрепить» — отвязывает в постоянное окно |
| PowerToys Peek | Обнаружить включённый Peek с конфликтующим хоткеем и показать уведомление с инструкцией; ничего не менять |
| Кодеки | Только системные (WIC / Media Foundation); при отсутствии — фолбэк с миниатюрой и ссылкой на расширение в Store |
| Текст | Lexilla (лексеры) + собственный виртуализированный рендер на DirectWrite/Direct2D |
| Markdown | Отрендеренным (md4c → нативные XAML-блоки), переключатель в исходник |
| Язык UI | EN + RU (.resw), по языку системы, переопределяется в настройках |
| Распространение | Публично на GitHub, MIT, установщик + portable zip, релизы через GitHub Actions |
| Обновления | Только вручную: кнопка в настройках проверяет GitHub Releases; фоновых проверок нет |
| Метрики | Строгие (раздел 9) |
| Объём | Всё до готового продукта и релиза v1.0.0 |

## 3. Архитектура

Три исполняемых файла + общая статическая библиотека.

```
FastLook.exe            (daemon, чистый Win32, без XAML; всегда запущен; ~2–5 МБ)
  ├─ keyboard hook thread (WH_KEYBOARD_LL)
  ├─ tray icon + меню (Настройки / Пауза / Выход)
  ├─ shell worker (STA, запросы к Explorer с таймаутом)
  ├─ viewer process manager (запуск, пинг, перезапуск, prewarm)
  └─ IPC client → Viewer (named pipe)

FastLook.Viewer.exe     (WinUI 3; тёплый, тримится; выходит после простоя)
  ├─ PreviewWindow (одно тёплое) + Pinned windows
  ├─ Previewers (image, text, markdown, media, pdf, archive, folder, shell, fallback)
  ├─ SettingsWindow (создаётся по требованию, уничтожается при закрытии)
  └─ IPC server ← Daemon

FastLook.PreviewHost.exe (Win32; «жертвенный» хост shell preview handlers; живёт только пока показывается такой превью)

FastLook.Core.lib       (без WinUI: типы файлов, настройки, кодировки, индексация строк, навигация, semver, IPC-протокол, лог)
```

**Почему отдельный daemon:** XAML-фреймворк WinUI 3 нельзя выгрузить из процесса после инициализации. Выполнить решение «после простоя остаётся только хук» можно, лишь завершив процесс Viewer. Daemon также изолирует хук от любых зависаний UI: что бы ни случилось с Viewer, пробел продолжает обрабатываться, а Viewer перезапускается.

### 3.1 Состояния памяти

| Состояние | Что живо | Цель Private WS (сумма процессов) |
|---|---|---|
| Cold | Daemon | ≤ 5 МБ |
| Warm-idle | Daemon + Viewer (окно скрыто, ресурсы освобождены, working set подрезан) | ≤ 25 МБ |
| Active | + открытый превью | см. раздел 9 |

Переходы:
- Закрытие превью → Viewer выгружает контент превьюера, освобождает декодеры/медиа/PDF, убивает PreviewHost, через ~1 с вызывает `SetProcessWorkingSetSize(-1,-1)` (и `HeapCompact`).
- Warm-idle дольше `idleUnloadMinutes` (5/15/30/60/никогда; по умолчанию 30) → Viewer корректно завершается → Cold.
- **Prewarm:** в Cold daemon слушает `EVENT_SYSTEM_FOREGROUND` (WinEvent, out-of-context); когда на передний план выходит окно Проводника (или рабочий стол, если включено), daemon запускает Viewer скрыто. Так первое нажатие почти всегда попадает в тёплый Viewer. Настройка «Предзагрузка при активации Проводника» (по умолчанию вкл).
- Пробел в Cold без prewarm → холодный запуск Viewer (цель ≤ 400 мс до кадра).
- Viewer при старте с флагом `--background` создаёт PreviewWindow, активирует один раз за экраном (или с `Hide` до первого показа — как выяснит спайк) и прячет.

## 4. Daemon (FastLook.exe)

### 4.1 Хук клавиатуры — надёжность прежде всего
- Отдельный поток с собственным message loop, `SetWindowsHookExW(WH_KEYBOARD_LL)`, приоритет `THREAD_PRIORITY_TIME_CRITICAL` для этого потока.
- Callback выполняет **только** дешёвые проверки (без COM, без аллокаций, без блокировок, ≤ 50 мкс):
  1. `VK_SPACE` (или настроенная клавиша), keydown, не injected-нами, без Ctrl/Alt/Shift/Win; автоповтор игнорируется (отслеживаем состояние «нажата»).
  2. Foreground HWND — класс `CabinetWClass` (Проводник) или, если включено, рабочий стол (`Progman`/`WorkerW` c `SHELLDLL_DefView`); либо foreground — наше PreviewWindow (тогда пробел обрабатывает сам Viewer, хук пропускает).
  3. `GetGUIThreadInfo` foreground-потока: фокус не в `Edit`/`RichEdit`/поле поиска/адресной строке, нет активного переименования (`hwndCaret == null` или класс не Edit), нет меню (`GUI_INMENUMODE`).
  4. Type-ahead: если в этот же поток Проводника за последние 1000 мс была нажата буквенно-цифровая клавиша (callback помнит время последнего такого keydown) — пропустить пробел в Проводник.
  - Если все условия выполнены — `PostThreadMessage` в поток-диспетчер daemon и вернуть 1 (поглотить keydown; соответствующий keyup тоже поглощается).
- **Самовосстановление:** поток хука переустанавливает хук (unhook+hook) каждые 60 с, а также по `WM_POWERBROADCAST/PBT_APMRESUMEAUTOMATIC`, `WTS_SESSION_UNLOCK`, `TaskbarCreated` (перезапуск Explorer). Watchdog в главном потоке: поток хука раз в секунду обновляет heartbeat; если heartbeat застыл > 5 с — поток пересоздаётся.
- Пауза (из трея) — хук снимается.

### 4.2 Получение выделения в Проводнике
- Выполняется на отдельном STA-потоке «shell worker». Каждый запрос имеет таймаут 1000 мс; если поток не ответил — он «бросается» (detached, помечен как отравленный), создаётся новый. UI daemon никогда не блокируется.
- `IShellWindows` → найти окно, чей `HWND` == foreground; для вкладок Win11 — активная вкладка: среди дочерних `ShellTabWindowClass` выбрать видимую и сравнить с `IShellBrowser`→`IOleWindow::GetWindow`.
- Рабочий стол: `IShellWindows::FindWindowSW(CSIDL_DESKTOP, …, SWC_DESKTOP, …)`.
- `IFolderView2`: выделенные элементы (`SVGIO_SELECTION`), все элементы в порядке отображения (`SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER`), фокусный индекс, `GetItemPosition` + `ClientToScreen` для прямоугольника иконки (для анимации), монитор Проводника.
- Элемент → путь `SIGDN_FILESYSPATH`; если пути нет (виртуальные папки, корзина, «Этот компьютер») — передаётся parsing name и флаг «virtual» → Viewer показывает фолбэк с миниатюрой.
- Большие папки: список элементов ограничен 20 000; если больше — навигация только по выделению ± 5000 соседей вокруг фокуса.

### 4.3 Управление Viewer
- Запуск: `CreateProcess` `FastLook.Viewer.exe --background --pipe <name>`; одна инстанция.
- IPC: named pipe `\\.\pipe\FastLook-<SID>-v1`, сообщения — длина + UTF-8 JSON (протокол в Core, версионирован).
  - `show {explorerHwnd, items[], index, iconRect, monitor, isDesktop}`
  - `toggle` (тот же файл уже показан → закрыть), `ping`/`pong`, `settingsChanged`, `openSettings`, `shutdown`.
- Перед `show` daemon делает `ping` с таймаутом 1500 мс; нет ответа → `TerminateProcess`, запуск нового Viewer и повтор. Падение Viewer детектируется по handle процесса — следующий пробел запускает новый.
- Single instance daemon: именованный mutex; повторный запуск FastLook.exe → открыть настройки.

### 4.4 Трей и интеграция
- `Shell_NotifyIconW` с иконкой приложения; меню: «Настройки», «Пауза/Продолжить», «Выход». Двойной клик — настройки (команда в Viewer).
- Автозапуск: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\FastLook` (переключается в настройках; установщик ставит по умолчанию).
- Детект Peek: при старте прочитать `%LOCALAPPDATA%\Microsoft\PowerToys\settings.json` (`enabled.Peek`) и `%LOCALAPPDATA%\Microsoft\PowerToys\Peek\settings.json` (activation shortcut). Если Peek включён и шорткат конфликтует (по умолчанию в PowerToys Ctrl+Space — конфликт только если пользователь назначил Space) **или** Peek включён и наш хоткей совпадает — показать balloon-уведомление в трее с инструкцией «PowerToys → Peek → выключить». Один раз за запуск, настройкой можно отключить.
- Per-monitor DPI v2 через манифест (во всех трёх exe).

## 5. Viewer (FastLook.Viewer.exe)

### 5.1 PreviewWindow
- WinUI 3 `Window`, `MicaBackdrop`, `ExtendsContentIntoTitleBar`, `OverlappedPresenter` без min/max, всегда поверх Проводника при показе (не topmost постоянно).
- Заголовок (48 px): иконка файла, имя, счётчик «3 / 27», кнопки: «Открыть» (приложение по умолчанию, Enter), «Открыть с помощью…» (меню: системный диалог `SHOpenWithDialog`), «Показать в папке», «Закрепить», «Закрыть».
- Размер: превьюер сообщает предпочтительный размер (изображение — нативный размер в DIP, PDF — пропорции страницы, текст — 960×720, медиа — разрешение видео, прочее — 720×540), ограничение [480×320, 80% рабочей области монитора Проводника], центр этого монитора. При навигации окно плавно меняет размер (≤ 150 мс), если новый контент существенно другой.
- Анимация: появление — масштаб из прямоугольника иконки файла в финальный + fade (180 мс, ease-out); закрытие — обратно (120 мс). Если прямоугольника нет — scale 0.95→1 + fade. Отключается при `SPI_GETCLIENTAREAANIMATION == FALSE`.
- Тема: светлая/тёмная по системе (или из настроек).
- Клавиши: Space/Esc — закрыть; ←/→ и ↑/↓ — пред./след.; Enter — открыть; Ctrl+C — скопировать файл в буфер (CF_HDROP); Ctrl +/−/0 — зум (изображения/PDF/текст); F11 — полноэкранный режим; Ctrl+W — закрыть.
- Синхронизация выделения: при навигации Viewer вызывает `IFolderView2::SelectItem(index, SVSI_SELECT|SVSI_DESELECTOTHERS|SVSI_ENSUREVISIBLE|SVSI_FOCUSED)` в Проводнике — на своём shell worker с таймаутом 500 мс (ошибка/таймаут не мешает навигации).
- Предзагрузка: после показа текущего — фоновая подготовка ±1 соседа (только «дешёвые» превьюеры: изображение в целевом размере, первые 64 КБ текста). Кэш ≤ 3 элемента, освобождается при закрытии.
- Отмена: каждый показ имеет `CancellationToken`; быстрая навигация отменяет предыдущую загрузку; UI-поток никогда не ждёт I/O.
- «Закрепить»: текущее PreviewWindow становится обычным окном (с min/max, в панели задач), пробел больше не закрывает его; daemon получает уведомление, и Viewer создаёт новое тёплое PreviewWindow. Закреплённые окна не мешают idle-выгрузке пока открыты (Viewer не выгружается, пока есть закреплённые окна).

### 5.2 Модель превьюеров
```cpp
struct IPreviewer {
  // фоновая подготовка (worker thread): чтение/декодирование
  virtual winrt::Windows::Foundation::IAsyncAction PrepareAsync(PreviewItem const&, PreviewContext&) = 0;
  // UI thread: построить/обновить UIElement
  virtual winrt::Microsoft::UI::Xaml::UIElement View() = 0;
  virtual winrt::Windows::Foundation::Size PreferredSize() const = 0;
  virtual void Unload() = 0;  // освободить всё
};
```
Выбор: `PreviewerRegistry` по расширению → по MIME/PerceivedType (`AssocGetPerceivedType`) → сниффинг первых 4 КБ (текст vs бинарь) → shell preview handler → фолбэк. Ошибка/таймаут любого превьюера (кроме фолбэка) → фолбэк с сообщением.

### 5.3 Превьюеры
- **Image** (`jpg jpeg png gif bmp tif tiff ico webp heic heif avif jxr dds` + любой с WIC-декодером, `svg`): WIC на фоновом потоке, декодирование сразу в целевой размер (`IWICBitmapSourceTransform`/`IWICBitmapScaler`, учёт DPI), EXIF-ориентация. Показ через `SoftwareBitmapSource`/`SurfaceImageSource`. Зум/панорама (ScrollViewer + колесо/пинч); при зуме > fit — перекодирование в большем разрешении (до нативного). Анимации GIF/WebP/APNG — кадры декодируются по требованию, цикл с таймингом из метаданных. SVG — `ID2D1SvgDocument` (Direct2D, без скриптов) в `SurfaceImageSource`. Нет кодека (`WINCODEC_ERR_COMPONENTNOTFOUND`) → фолбэк + ссылка на расширение в Store (HEIF `ms-windows-store://pdp/?productid=9PMMSR1CGPWG`, AV1/AVIF `9MVZQVXJBQ9V`, RAW `9NCTDW2W1BH8`).
- **Text/Code**: memory-mapped файл; определение кодировки (BOM → валидный UTF-8 → системная ANSI codepage); индекс строк строится инкрементально на фоне; лексинг Lexilla по расширению (≥ 40 языков: c/cpp/h/cs/java/kt/js/ts/jsx/tsx/json/xml/html/css/scss/py/rb/go/rs/php/sql/sh/ps1/bat/yaml/toml/ini/md(исходник)/lua/swift/dart/r/pl/makefile/cmake/dockerfile/diff/log/csv…) только для видимого окна + запас; файлы > 20 МБ — без подсветки. Рендер: собственный контрол `CodeView` на `VirtualSurfaceImageSource` + Direct2D/DirectWrite: номера строк, моноширинный шрифт (Cascadia Mono → Consolas), выделение мышью, Ctrl+A/Ctrl+C, перенос строк (переключатель), цветовые схемы светлая/тёмная. Бинарь по сниффингу → не сюда.
- **Markdown** (`md markdown`): md4c → дерево блоков → `RichTextBlock`/`StackPanel` (заголовки, параграфы, списки, цитаты, таблицы, код с подсветкой Lexilla, горизонтальные линии, ссылки — открываются в браузере по клику, локальные изображения через WIC). Удалённые изображения не загружаются (приватность). Кнопка «Исходник» переключает на CodeView.
- **Media** (видео/аудио, расширения Media Foundation): `MediaPlayerElement` с транспортными контролами, автозапуск, громкость запоминается. Аудио: обложка (через shell thumbnail), название/исполнитель/альбом/длительность (property store). При Unload — `MediaPlayer` полностью закрывается и обнуляется. Неподдерживаемый кодек → фолбэк + ссылка на расширение (HEVC `9NMZLZ57R3T7`).
- **PDF**: `Windows.Data.Pdf.PdfDocument`; страницы в виртуализированном `ItemsRepeater`, рендер лениво по видимости в ширину окна × DPI; при зуме — перерендер. Не более 6 отрендеренных страниц в памяти. Защищённый паролем → фолбэк с сообщением.
- **Shell preview handler** (docx/xlsx/pptx/msg/vsdx… — всё, у чего есть зарегистрированный `{8895b1c6-b41f-4c1c-a562-0d564250836f}` handler, кроме форматов, которые мы показываем сами): см. 6. Блок-лист CLSID handler'ов на WebView2 (PowerToys Markdown/SVG/Monaco/PDF, Edge PDF) — для них используем свои превьюеры или фолбэк.
- **Archive** (`zip 7z rar tar gz tgz bz2 xz zst cab iso`): libarchive, только чтение заголовков (без распаковки), дерево/список: имя, размер, сжатый размер, дата; итог: файлов/папок/суммарный размер. Лимит 100 000 записей и 2 с — дальше «показаны первые N». Зашифрованные заголовки → сообщение.
- **Folder**: крупная иконка, имя, путь, даты; количество элементов и суммарный размер считаются на фоне с отменой (лимит 5 с, затем «более N»).
- **Fallback**: крупная миниатюра/иконка (`IShellItemImageFactory`, 256 px), имя, тип (описание из ассоциации), размер, даты создания/изменения, атрибуты; кнопки «Открыть» / «Открыть с помощью». Для отсутствующих кодеков — ссылка на Store.

### 5.4 SettingsWindow
WinUI 3, NavigationView-стиль Windows 11 (как «Параметры»). Создаётся по команде, уничтожается при закрытии (не держит память).
Разделы:
- **Общие:** запуск с Windows; работать на рабочем столе (по умолчанию выкл); клавиша активации (Space по умолчанию; Ctrl+Space, Alt+Space, Shift+Space); предзагрузка при активации Проводника; выгрузка из памяти после простоя (5/15/30/60/никогда); тема (система/светлая/тёмная); язык (система/English/Русский); предупреждение о PowerToys Peek.
- **Превью:** перенос строк по умолчанию; размер шрифта кода; автозапуск медиа; использовать системные обработчики (Office и др.); Markdown — отрендерить/исходник по умолчанию.
- **О программе:** версия, «Проверить обновления», ссылка на GitHub, лицензии сторонних компонентов, «Открыть папку логов».

Хранение: `%LOCALAPPDATA%\FastLook\settings.json` (схема версионирована, неизвестные поля сохраняются, битый файл → бэкап `.bak` + дефолты). Daemon и Viewer перечитывают по сообщению `settingsChanged`.

## 6. PreviewHost (FastLook.PreviewHost.exe)
- Запускается Viewer по требованию; одна инстанция на открытый shell-превью; убивается при закрытии превью/переходе к другому типу/таймауте.
- Viewer создаёт дочерний Win32 HWND («site») поверх области контента PreviewWindow и передаёт его HWND хосту через pipe/командную строку. Хост: `CoInitializeEx(STA)`, `CoCreateInstance(clsid, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER)`, инициализация по `IInitializeWithFile` → `IInitializeWithStream` (SHCreateStreamOnFileEx) → `IInitializeWithItem`, `SetWindow(site, rect)`, `DoPreview`, `SetRect` при ресайзе, тема (`IPreviewHandlerVisuals`).
- Таймаут показа 8 с (Office медленный при первом запуске) → kill, фолбэк. Падение/зависание хоста не влияет на Viewer.
- Пока активна область shell-превью, XAML не рисует поверх неё (airspace) — заголовок окна остаётся XAML.

## 7. Сборка, зависимости, структура репозитория

```
FastLook.sln
Directory.Build.props          (общие свойства: C++20, /permissive-, /W4, v145 с откатом на v143, x64)
vcpkg.json                     (manifest: lexilla, md4c, libarchive, gtest; triplet x64-windows-static-md)
src/Core/        FastLook.Core.vcxproj (static lib)
src/Daemon/      FastLook.Daemon.vcxproj → FastLook.exe (Win32, без WinAppSDK)
src/Viewer/      FastLook.Viewer.vcxproj → FastLook.Viewer.exe (WinUI 3, self-contained)
src/PreviewHost/ FastLook.PreviewHost.vcxproj → FastLook.PreviewHost.exe (Win32)
tests/Core.Tests/ FastLook.Core.Tests.vcxproj (GoogleTest)
tests/fixtures/  тестовые файлы всех форматов
scripts/build.ps1, scripts/test.ps1, scripts/measure-ram.ps1, scripts/bench.ps1, scripts/package.ps1
installer/FastLook.iss          (Inno Setup)
.github/workflows/ci.yml, release.yml
docs/
```
- Выходная папка релиза: все exe + WinAppSDK runtime рядом (self-contained) + `Assets/` + `*.pri`. Все три exe в одной папке.
- Сторонние лицензии: Lexilla (HPND-подобная), md4c (MIT), libarchive (BSD-2), WinAppSDK (MIT) — в `THIRD_PARTY_NOTICES.md`.

## 8. Распространение и обновления
- **Установщик** Inno Setup: per-user (`PrivilegesRequired=lowest`), `%LOCALAPPDATA%\Programs\FastLook`, ярлык в «Пуск», опция автозапуска (по умолчанию вкл), запуск после установки. При обновлении — закрывает FastLook (`FastLook.exe --shutdown`), заменяет файлы. Деинсталлятор удаляет Run-ключ и (по флажку) настройки.
- **Portable zip** — та же папка; портативный режим, если рядом с exe лежит `portable.txt` (настройки рядом с exe).
- Бинарники не подписываются (нет сертификата) — README предупреждает о SmartScreen.
- **Обновления (вручную):** «Проверить обновления» → WinHTTP `GET https://api.github.com/repos/Denmurzik/FastLook/releases/latest` → semver-сравнение → «Скачать и установить»: качается `FastLook-Setup-x.y.z.exe` и `.sha256` из ассетов, проверяется SHA-256, запускается установщик `/SILENT`, приложение завершается. Portable — открывает страницу релиза. Никаких фоновых сетевых запросов.
- **CI** (`ci.yml`): на push/PR — Release-сборка, unit-тесты. **Release** (`release.yml`): на тег `v*` — сборка, тесты, `package.ps1` (zip + Inno Setup), SHA-256, публикация GitHub Release с ассетами.

## 9. Метрики приёмки (Private Working Set, Диспетчер задач)
| Метрика | Цель |
|---|---|
| Cold (только daemon) | ≤ 5 МБ |
| Warm-idle (daemon + Viewer) | ≤ 25 МБ |
| Пробел → окно видно (warm) | ≤ 50 мс |
| Пробел → изображение/текст отрисованы (warm, файл ≤ 10 МБ / 24 Мп) | ≤ 150 мс |
| Пробел → окно (cold, без prewarm) | ≤ 400 мс |
| Просмотр фото 24 Мп | ≤ 120 МБ суммарно |
| 1000 последовательных открытий/закрытий (смешанные форматы) | рост warm-idle ≤ 2 МБ, 0 пропущенных пробелов, 0 падений |
| 24 ч работы с выходами из сна/блокировками | пробел срабатывает всегда |

Измерение: `scripts/measure-ram.ps1` и `scripts/bench.ps1` (bench-режим Viewer: `--bench <fixtures-dir> --iterations 1000` открывает/закрывает превью программно, без хука; задержка хука измеряется отдельно через `SendInput` в тестовое окно Проводника).

## 10. Тестирование
- Unit (GoogleTest, Core): выбор превьюера по расширению/сниффингу, детект кодировки, индекс строк, навигационный список (выделение vs папка, границы, лимиты), парсинг/миграция настроек, semver, IPC-сериализация, детект конфликта Peek (по фикстурам JSON), фильтр условий хука (чистая функция над снимком состояния), md4c → модель блоков.
- Smoke/интеграция (скрипты): Viewer `--preview <file>` для каждой фикстуры — без падений, окно появилось, RAM в пределах; bench 1000 открытий; daemon старт/стоп/перезапуск Viewer при kill.
- Ручная проверка перед релизом: чек-лист в `docs/release-checklist.md` (пробел в Проводнике/вкладках/рабочем столе, переименование и type-ahead не ломаются, навигация, синхронизация выделения, все форматы, Peek-предупреждение, сон/пробуждение, установщик/обновление/удаление).

## 11. Безопасность и приватность
- Никаких сетевых запросов, кроме ручной проверки обновлений (HTTPS к api.github.com/github.com).
- Не исполняем файлы; SVG через D2D (без скриптов); Markdown не грузит удалённые ресурсы; shell handlers изолированы в отдельном процессе.
- Установщик проверяется по SHA-256 перед запуском.
- Логи: `%LOCALAPPDATA%\FastLook\logs\`, ротация 2×1 МБ, без содержимого файлов (только пути на уровне Debug, по умолчанию Info без путей).

## 12. Вне объёма v1.0
ARM64-сборка, Windows 10, диалоги «Открыть/Сохранить» сторонних программ, редактирование файлов, плагины сторонних разработчиков, подпись кода, Microsoft Store, winget.

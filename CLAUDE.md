# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## О проекте

CalendarServer — сервер синхронизации «Космос календаря»: программа на Qt 6
(Core, Network, Sql), без GUI и сторонних библиотек, для Windows и Linux. На
Windows установщик ставит её службой `CosmosCalendarServer`, на Linux она
служба systemd, для отладки — консоль. Хранит аккаунты и записи планеров в
SQLite, раздаёт изменения устройствам и ведёт общие календари.

Рядом — админ-панель `admin/` (`CalendarServerAdmin`, Qt Widgets): состояние,
запуск и остановка службы, аккаунты с их календарями и записями, новый
пароль, резервные копии, статистика, журнал. Она читает ту же базу напрямую и
собирается из исходников сервера (DesDoc, раздел 12).

Устройство — в [DesDoc.md](DesDoc.md), сборка, запуск и API — в
[README.md](README.md). Сервер — сабмодуль зонтичного репозитория
[CosmosCalendar](https://github.com/SayxZero/CosmosCalendar) (в рабочей копии —
`..`): там общий `CLAUDE.md` (порядок коммитов, правила проверок, локальное
окружение), `DESDOC.md` — как сервер и планер работают вместе — и `ISSUES.md`
с граблями обоих проектов. Клиент — соседний сабмодуль `../Calendar`.

## Сборка и тесты

Батники, как у планера; оба понимают `-noui`, пути — в `build-env.bat` с
приставкой `SERVER_`:

```bash
cmd //c "D:\\Prog\\Calendar\\CalendarServer\\build-server.bat" -noui   # сервер, панель, 27 + 10 тестов
cmd //c "D:\\Prog\\Calendar\\CalendarServer\\run-server.bat" -noui --list-users
```

Из Git Bash батник зовётся полным путём: в окружении агента `cmd` не ищет
программы в текущей папке.

**Боевой сервер обычно запущен** — его не останавливать. Это служба
`CosmosCalendarServer` из `C:\Program Files\CalendarServer` (`../CLAUDE.md`,
«Локальное окружение»). Пока сервер работает из
каталога сборки, его `CalendarServer.exe` не перелинковать — так и с
тестовым сервером из `build-check`, — поэтому рядом с ним собирается и
гоняется только цель тестов:

```bash
PATH="/d/Qt/6.5.3/mingw_64/bin:/d/Qt/Tools/mingw1120_64/bin:/d/Qt/Tools/Ninja:$PATH"
/d/Qt/Tools/CMake_64/bin/cmake --build build --target tst_server
./build/tst_server -o out.txt,txt && grep -E "FAIL|Totals" out.txt
./build/tst_server linkRulesAreKept -o out.txt,txt   # одна функция
./build/tst_admin -o out-admin.txt,txt && grep -E "FAIL|Totals" out-admin.txt
```

Вывод QtTest в консоль этой оболочки не виден — только через файл. Сервер из
свежей сборки поднимается в отдельном каталоге и со своей папкой данных:

```bash
SERVER_BUILD=build-check cmd //c "D:\\Prog\\Calendar\\CalendarServer\\build-server.bat" -noui
SERVER_BUILD=build-check cmd //c "D:\\Prog\\Calendar\\CalendarServer\\run-server.bat" -noui -p 8766 -d "<своя папка>"
```

Тестовые аккаунты не должны попадать в настоящую базу: любая ручная проверка —
с `-d <своя папка>`.

**Служба и админ-панель.** Службу на этой машине не ставить и не запускать
(`../CLAUDE.md`): код службы проверяет владелец установкой. Агенту остаётся
`--service` вне диспетчера служб — он должен отказаться с кодом 2 — и всё
остальное в консоли: `--log-file`, `--import-db` на копии тестовой базы.
Панель — на тестовом сервере, без UAC и мимо настоящей базы:

```bash
build-check/CalendarServerAdmin.exe --config "<папка>/server.ini" --grab "<папка снимков>"
```

`--grab` снимает все страницы, вкладки аккаунта и диалоги в PNG и выходит,
окна на экране нет. Тестовую историю обменов удобно положить прямо в таблицу
`activity` тестовой базы.

**Установщики Windows** — `build-installer.bat` (`-out <папка>`,
`-skipbuild` — упаковать уже собранные сервер и панель): сборка и тесты в
`build-release` (`SERVER_BUILD`), `windeployqt` обеих программ в одну папку,
затем WiX 3 (`SERVER_WIX`, по умолчанию из `%WIX%`) с расширениями UI, Util и
Firewall — `CalendarServer-<v>-x64.msi` и `-setup.exe`. `CalendarServer.exe`
перед `heat` уходит из собираемой папки: это файл компонента службы в
`CalendarServer.wxs` (`ServiceInstall`, `ServiceControl`, правило
брандмауэра). Исходники — `packaging/windows/`: `service.ini` — `server.ini`
папки данных службы, `firewall-ru.wxl` — русские строки расширения
брандмауэра, которых нет в WiX 3.7; `setup-theme.xml`, `setup-logo.png`,
`msi-*.bmp` и `CalendarServer.ico` — копии из планера, `setup-ru.wxl` — тоже,
но с текстами про админ-панель. `UpgradeCode` в `.wxs` не меняются никогда.
Проверка без установки — `msiexec /a`, запуск распакованных программ без Qt в
`PATH` и таблицы MSI через COM (`../CLAUDE.md`). Все пакеты выпуска разом —
`../build-packages.bat`.

## Соглашения

- **Сообщения для людей — по-русски** (`message` в ответах, журнал, справка),
  идентификаторы, коды ошибок (`error`) и комментарии — по-английски.
- **Два файла проекта.** `CMakeLists.txt` собирает сервер, тесты и панель из
  `SERVER_CORE_SOURCES` (библиотека `server_core`), `CalendarServer.pro`
  перечисляет файлы сам — новый файл сервера добавляется в оба, и ещё в
  `admin/CalendarServerAdmin.pro`, который тоже компилирует исходники
  сервера. Файл панели — в `CMakeLists.txt` (`ADMIN_DATA_SOURCES` для того,
  что без окна и нужно `tst_admin`, иначе в цель `CalendarServerAdmin`) и в её
  `.pro`.
- **Записи непрозрачны.** Сервер не разбирает `data` записи; единственное
  исключение — `calendarId` записей общего календаря (`ShareStore`). Новое
  место, где серверу захотелось заглянуть внутрь записи, — повод
  остановиться: формат записи принадлежит планеру. Админ-панель — не сервер:
  она читает несколько полей только для показа (`admin/records.*`), и
  переименованное в планере поле даёт у неё пустую колонку, а не ошибку.
- **Админ-панель базу не создаёт и схему не меняет.** Пишет она только новый
  пароль и восстановление из копии; всё остальное — чтение рядом с работающим
  сервером.
- **Схема базы** — в `PRAGMA user_version` (`kSchemaVersion` в `storage.cpp`,
  сейчас 3); новые таблицы — через `CREATE TABLE IF NOT EXISTS`, как у версий
  2 и 3. Базу более новой схемы сервер не открывает.
- **Версия программы** — в трёх местах: `project()` в `CMakeLists.txt`,
  `VERSION` обоих `.pro` и `VERSIONINFO` в `admin/admin.rc`.
- **Протокол меняется вместе с планером.** Новый маршрут или поле ответа
  описываются в `README.md` (таблица API), в `DesDoc.md` и в `DESDOC.md`
  зонтичного репозитория; несовместимое изменение поднимает
  `Api::apiVersion()`.
- Включён `QT_DISABLE_DEPRECATED_BEFORE=0x060000`.

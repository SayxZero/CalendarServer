# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## О проекте

CalendarServer — сервер синхронизации «Космос календаря»: консольная программа
на Qt 6 (Core, Network, Sql), без GUI и сторонних библиотек, для Windows и
Linux. Хранит аккаунты и записи планеров в SQLite, раздаёт изменения
устройствам и ведёт общие календари.

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
cmd //c "D:\\Prog\\Calendar\\CalendarServer\\build-server.bat" -noui   # настройка, сборка, 22 теста
cmd //c "D:\\Prog\\Calendar\\CalendarServer\\run-server.bat" -noui --list-users
```

Из Git Bash батник зовётся полным путём: в окружении агента `cmd` не ищет
программы в текущей папке.

**Боевой сервер обычно запущен** из `build/` — его не останавливать. Пока он
работает, `CalendarServer.exe` не перелинковать, поэтому собирается и
гоняется только цель тестов:

```bash
PATH="/d/Qt/6.5.3/mingw_64/bin:/d/Qt/Tools/mingw1120_64/bin:/d/Qt/Tools/Ninja:$PATH"
/d/Qt/Tools/CMake_64/bin/cmake --build build --target tst_server
./build/tst_server -o out.txt,txt && grep -E "FAIL|Totals" out.txt
./build/tst_server linkRulesAreKept -o out.txt,txt   # одна функция
```

Вывод QtTest в консоль этой оболочки не виден — только через файл. Сервер из
свежей сборки поднимается в отдельном каталоге и со своей папкой данных:

```bash
SERVER_BUILD=build-check cmd //c "D:\\Prog\\Calendar\\CalendarServer\\build-server.bat" -noui
SERVER_BUILD=build-check cmd //c "D:\\Prog\\Calendar\\CalendarServer\\run-server.bat" -noui -p 8766 -d "<своя папка>"
```

Тестовые аккаунты не должны попадать в настоящую базу: любая ручная проверка —
с `-d <своя папка>`.

## Соглашения

- **Сообщения для людей — по-русски** (`message` в ответах, журнал, справка),
  идентификаторы, коды ошибок (`error`) и комментарии — по-английски.
- **Два файла проекта.** `CMakeLists.txt` собирает сервер и тесты из
  `SERVER_CORE_SOURCES`, `CalendarServer.pro` перечисляет файлы сам — новый
  файл добавляется в оба.
- **Записи непрозрачны.** Сервер не разбирает `data` записи; единственное
  исключение — `calendarId` записей общего календаря (`ShareStore`). Новое
  место, где серверу захотелось заглянуть внутрь записи, — повод
  остановиться: формат записи принадлежит планеру.
- **Схема базы** — в `PRAGMA user_version` (`kSchemaVersion` в `storage.cpp`,
  сейчас 2); новые таблицы — через `CREATE TABLE IF NOT EXISTS`, как у версии 2.
- **Протокол меняется вместе с планером.** Новый маршрут или поле ответа
  описываются в `README.md` (таблица API), в `DesDoc.md` и в `DESDOC.md`
  зонтичного репозитория; несовместимое изменение поднимает
  `Api::apiVersion()`.
- Включён `QT_DISABLE_DEPRECATED_BEFORE=0x060000`.

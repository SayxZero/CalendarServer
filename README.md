# CalendarServer

Сервер синхронизации «Космос календаря». Хранит аккаунты пользователей и их
записи (календари, события, задачи, журнал выполнений, привычки, напоминания и
прочее, что присылает планер) и держит их одинаковыми на всех устройствах
аккаунта — компьютерах и телефонах. Он же раздаёт общие календари: кто кому
открыл календарь, ссылки, которыми доступ выдают, и разводка правок между
всеми, кто календарь делит.

Консольное приложение на Qt 6 (Core, Network, Sql) без других зависимостей и
без графического интерфейса. Работает на Windows и Linux, собирается на обеих
одинаково.

**Связанные проекты.** Клиент — планер
[Calendar](https://github.com/SayxZero/Calendar) (Windows, Linux, Android):
сервер ему нужен только для аккаунтов, без аккаунта планер работает сам по
себе. Оба проекта собраны в зонтичном репозитории
[CosmosCalendar](https://github.com/SayxZero/CosmosCalendar), там же общее
описание того, как они работают вместе (`DESDOC.md`). Внутреннее устройство
сервера — в [DesDoc.md](DesDoc.md).

## Сборка

Нужны Qt 6.2 или новее с модулями Core, Network и Sql (драйвер SQLite входит в
Qt; модуль Test — для тестов), CMake 3.16+ и компилятор C++17. Регулярно сервер
собирается на Windows 10 с Qt 6.5.3 и MinGW 11.2; сборка на Linux описана по
пакетам дистрибутивов.

Общий путь на любой системе:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Тесты собираются вместе с сервером (`-DSERVER_BUILD_TESTS=OFF` их выключает).

### Windows: батники

Проще всего — батником из корня проекта: он настроит CMake, соберёт сервер и
прогонит тесты, а пути к Qt возьмёт из `build-env.bat`.

```bat
build-server.bat
build-server.bat -noui     :: не ждать нажатия клавиши в конце (для скриптов)
```

Пути переопределяются переменными с приставкой `SERVER_` — `SERVER_QT` (набор
Qt, по умолчанию `D:\Qt\6.5.3\mingw_64`), `SERVER_QT_TOOLS` (CMake, Ninja и
MinGW из поставки Qt), `SERVER_BUILD` (каталог сборки, по умолчанию `build`).
Приставка нужна потому, что привычные `QT_DIR` и подобные часто уже заняты под
другую версию Qt.

```bat
set "SERVER_QT=C:\Qt\6.8.0\mingw_64"
set "SERVER_QT_TOOLS=C:\Qt\Tools"
build-server.bat
```

Запущенный сервер держит свой `CalendarServer.exe`, и пересобрать его на месте
нельзя — линковка упадёт. Для пробной сборки рядом с работающим сервером
задайте другой каталог: `set SERVER_BUILD=build-check`.

### Windows: Qt Creator

Откройте `CMakeLists.txt` (или `CalendarServer.pro`), выберите набор
«Desktop Qt 6.x MinGW 64-bit» и соберите. Тесты — цель `tst_server`.

### Windows: командная строка

С набором MinGW из поставки Qt:

```bat
set PATH=D:\Qt\Tools\CMake_64\bin;D:\Qt\Tools\Ninja;D:\Qt\Tools\mingw1120_64\bin;D:\Qt\6.5.3\mingw_64\bin;%PATH%
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:\Qt\6.5.3\mingw_64
cmake --build build
build\tst_server -o result.txt,txt
windeployqt --no-translations build\CalendarServer.exe
```

`windeployqt` кладёт рядом с программой нужные DLL Qt и плагины (среди них
драйвер SQLite `sqldrivers\qsqlite.dll`), после чего папку `build` можно
переносить на другой компьютер. Флаг `--release` с MinGW не нужен: с ним
`windeployqt` пропускает плагины, и сервер без Qt в `PATH` не откроет базу.

На Windows вывод QtTest в консоль иногда не виден — поэтому результат тестов
пишется в файл (`-o result.txt,txt`).

### Linux: Debian, Ubuntu

Qt 6.2+ есть в Debian 12 и Ubuntu 22.04 и новее.

```bash
sudo apt install build-essential cmake ninja-build qt6-base-dev libqt6sql6-sqlite
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build          # ставит /usr/local/bin/CalendarServer
```

### Linux: Fedora

```bash
sudo dnf install gcc-c++ cmake ninja-build qt6-qtbase-devel
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

### qmake

Сборка через qmake тоже возможна и описывает ту же программу (без тестов —
они собираются только CMake):

```bash
mkdir build-qmake && cd build-qmake
qmake ../CalendarServer.pro
make                                 # Windows + MinGW: mingw32-make release
```

## Запуск

На Windows — батником из корня проекта: он кладёт Qt в `PATH`, держит сервер
в этом же окне (журнал виден прямо в нём, Ctrl+C останавливает) и отказывается
поднимать второй экземпляр поверх работающего — второй можно только со своим
портом или своей папкой данных.

```bat
run-server.bat                     :: порт 8765, база в папке данных пользователя
run-server.bat -p 8766 -d D:\srv   :: свой порт и своя папка - можно рядом с боевым
run-server.bat --list-users        :: показать аккаунты и выйти
```

Аргументы уходят серверу как есть, кроме `-noui`. Команды, которые ничего не
слушают (`--list-users`, `--delete-user`, `--set-password`, `--help`),
работают и при запущенном сервере.

```bash
CalendarServer                         # порт 8765, база в папке данных пользователя
CalendarServer -p 9000 -d /var/lib/calendar-server
CalendarServer --config server.ini     # настройки из файла, см. deploy/server.ini.example
CalendarServer --no-registration       # новые аккаунты создавать нельзя
```

При старте сервер пишет, где лежит база и по каким адресам к нему можно
подключиться, например:

```
2026-09-21 21:33:14 CalendarServer 1.0.0 слушает порт 8765
2026-09-21 21:33:14 База данных: C:/Users/…/AppData/Roaming/CalendarPlanner/CalendarServer/server.db
2026-09-21 21:33:14 Адрес для клиентов:
2026-09-21 21:33:14     192.168.0.146:8765  (Ethernet)
```

Этот адрес вводится в планере в «Настройки → Аккаунт и синхронизация → Адрес
сервера». Кнопка «Проверить сервер» рядом с полем отправляет несколько
проверочных запросов и показывает время ответа каждого.

| Параметр | Значение по умолчанию |
|---|---|
| `-p`, `--port` | 8765 |
| `--host` | все адреса машины |
| `-d`, `--data-dir` | Windows: `%APPDATA%\CalendarPlanner\CalendarServer`, Linux: `~/.local/share/CalendarPlanner/CalendarServer` |
| `-c`, `--config` | — |
| `--no-registration` | регистрация открыта |

В файле настроек (`deploy/server.ini.example`, группа `[server]`) есть ещё
предельный размер запроса (`max_body_mb`, 64 МБ) и срок жизни ключа сеанса
(`token_lifetime_days`, 365 дней). Параметры командной строки сильнее файла.

Журнал идёт в стандартный вывод: одна строка на запрос с адресом клиента,
кодом ответа и временем обработки. Ctrl+C или SIGTERM останавливают сервер.

### Администрирование

```bash
CalendarServer --list-users                  # аккаунты, число записей и сеансов
CalendarServer --set-password anna           # новый пароль читается из stdin;
                                             # все устройства аккаунта выйдут
CalendarServer --delete-user anna            # аккаунт вместе с данными
```

Команды работают с той же базой, что и сервер (учитывают `-d` и `--config`).

### Резервная копия

Вся база — один файл `server.db` в папке данных (рядом, пока сервер работает,
лежат `server.db-wal` и `server.db-shm`). Надёжнее всего копировать его при
остановленном сервере: при остановке сервер переносит журнал WAL в сам файл.

### Брандмауэр

Телефон должен достучаться до порта сервера. Если Windows не спросила
разрешения при первом запуске, правило добавляется вручную (PowerShell от
имени администратора):

```powershell
New-NetFirewallRule -DisplayName "CalendarServer" -Direction Inbound -Protocol TCP -LocalPort 8765 -Action Allow -Profile Private
```

На Linux с ufw: `sudo ufw allow 8765/tcp`.

### Служба Linux (systemd)

```bash
sudo useradd --system --home /var/lib/calendar-server --create-home calendar
sudo cp deploy/calendar-server.service /etc/systemd/system/
sudo systemctl enable --now calendar-server
journalctl -u calendar-server -f        # журнал
```

Служба ждёт программу в `/usr/local/bin/CalendarServer` (туда её ставит
`cmake --install`) и данные в `/var/lib/calendar-server`.

### Автозапуск на Windows

Проще всего — задача планировщика, которая запускает сервер при входе в
систему (путь — к вашей сборке, разложенной `windeployqt`):

```powershell
$action  = New-ScheduledTaskAction -Execute "C:\CalendarServer\CalendarServer.exe"
$trigger = New-ScheduledTaskTrigger -AtLogOn
Register-ScheduledTask -TaskName "CalendarServer" -Action $action -Trigger $trigger
```

## Безопасность

- Пароли хранятся как PBKDF2-HMAC-SHA256 (120 000 итераций, случайная соль).
- Устройство после входа получает случайный ключ сеанса; на сервере лежит только
  его SHA-256. Ключ, которым не пользовались год, перестаёт действовать.
- Десять неверных паролей подряд с одного адреса блокируют вход на десять минут.
- Ссылки общего доступа хранятся так же — хешем. Одноразовая гаснет после
  первого перехода, и у любой ссылки может быть срок (до года).
- Сервер говорит по HTTP без шифрования. Для домашней сети этого достаточно;
  чтобы открыть его в интернет, поставьте перед ним обратный прокси с TLS
  (nginx, Caddy) и укажите в планере адрес `https://…`.

## API

Все ответы — JSON. Ошибка: `{"error": "код", "message": "текст по-русски"}`.
Кроме `ping`, `register` и `login`, запросы требуют заголовок
`Authorization: Bearer <ключ>`. Версия API — поле `api` в ответе `ping`
(сейчас 1).

| Запрос | Тело | Ответ |
|---|---|---|
| `GET /api/ping` | — | `{service, version, api, status, time, registration}` |
| `POST /api/register` | `{login, password, displayName?, device?}` | `201 {token, user}` |
| `POST /api/login` | `{login, password, device?}` | `{token, user}` |
| `POST /api/logout` | — | `{}` |
| `GET /api/me` | — | `{user, revision, records, sessions}` |
| `POST /api/profile` | `{displayName}` | `{user}` |
| `POST /api/password` | `{oldPassword, newPassword}` | `{}`; другие устройства выходят |
| `POST /api/sync` | `{since, device?, changes: [...]}` | `{revision, accepted, rejected, conflicts: [...], changes: [...]}` |
| `GET /api/record?kind=…&id=…` | — | `{found, change?: {kind, id, deleted, modified, revision, data?}}` — одна запись, как её держит этот аккаунт |
| `GET /api/share?calendar=<id>` | — | `{share}` — что владелец открыл и кому |
| `POST /api/share` | `{calendar, enabled, mode}` | `{share}` |
| `POST /api/share/member` | `{calendar, login, access}` или `{calendar, login, remove}` | `{share}` |
| `POST /api/share/link` | `{calendar, singleUse, lifeSeconds}` | `201 {token, expiresAt}` |
| `GET /api/share/peek?token=<t>` | — | `{share, calendar}` — куда ведёт ссылка |
| `POST /api/share/join` | `{token, twinId}` | `201 {share, access, revision}` |
| `POST /api/share/leave` | `{twinId}` | `{revision}` |

Запись в обмене — JSON-объект с видом, идентификатором и моментом последнего
изменения; содержимое (`data`) сервер не разбирает:

```json
{ "kind": "task", "id": "…", "modified": 1790000000000, "deleted": false, "data": { … } }
```

Для каждой записи побеждает более поздняя правка; в общем календаре к этому
добавляются проверки прав и список `conflicts` (`read_only`, `deleted`). Как
это устроено внутри — в [DesDoc.md](DesDoc.md), разделы 5–6.

## Тесты

`tests/tst_server.cpp` — 22 теста. Они поднимают сервер внутри процесса на
свободном порту и проверяют его настоящими HTTP-запросами: регистрацию и вход,
ошибки, передачу изменений между устройствами, разрешение конфликтов,
удаление, выход, смену пароля, блокировку подбора, конвейер запросов, разбор
некорректного HTTP и общие календари целиком.

```bash
ctest --test-dir build --output-on-failure
./build/tst_server -o result.txt,txt          # Windows: вывод в файл
./build/tst_server linkRulesAreKept           # одна функция
```

Со стороны планера связку проверяет его тест `syncServiceTalksToRealServer`
(`PLANNER_TEST_SERVER=127.0.0.1:8765`) — для него сервер запускается отдельно,
со своей папкой данных.

## Структура исходников

```
src/
  main.cpp          точка входа: разбор параметров, команды администрирования,
                    консоль в UTF-8, остановка по Ctrl+C и SIGTERM
  config.*          настройки: INI-файл и командная строка поверх него
  httpserver.*      HTTP/1.1 поверх QTcpServer
  api.*             JSON API: маршруты, вход, ключи сеансов, защита от подбора
  storage.*         SQLite: аккаунты, ключи, записи, сам обмен
  shares.*          общие календари: доступ, ссылки, разводка правок
tests/tst_server.cpp  тесты по HTTP
deploy/             пример server.ini и служба systemd
build-server.bat    сборка и тесты на Windows
run-server.bat      запуск на Windows
build-env.bat       пути к Qt и инструментам (SERVER_*)
```

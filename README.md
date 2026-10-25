# CalendarServer

Сервер синхронизации «Космос календаря». Хранит аккаунты пользователей и их
записи (календари, события, задачи, журнал выполнений, привычки, напоминания и
прочее, что присылает планер) и держит их одинаковыми на всех устройствах
аккаунта — компьютерах и телефонах. Он же раздаёт общие календари: кто кому
открыл календарь, ссылки, которыми доступ выдают, и разводка правок между
всеми, кто календарь делит.

Программа на Qt 6 (Core, Network, Sql) без других зависимостей и без
графического интерфейса. Работает на Windows и Linux, собирается на обеих
одинаково; на Windows установщик ставит её службой, на Linux она работает
службой systemd, а для отладки запускается и в консоли.

Рядом — **админ-панель** (`CalendarServerAdmin`, Qt Widgets): отдельная
программа администратора на той же машине. Она показывает, работает ли
сервер, запускает и останавливает его службу, показывает аккаунты, их
устройства, календари (свои, открытые другим и календари-близнецы чужих),
события, задачи и прочие записи, меняет пароли, делает резервные копии базы и
восстанавливает из них, считает статистику — сколько аккаунтов
синхронизируется за день, неделю и месяц, — и показывает журнал сервера.

**Связанные проекты.** Клиент — планер
[Calendar](https://github.com/SayxZero/Calendar) (Windows, Linux, Android):
сервер ему нужен только для аккаунтов, без аккаунта планер работает сам по
себе. Оба проекта собраны в зонтичном репозитории
[CosmosCalendar](https://github.com/SayxZero/CosmosCalendar), там же общее
описание того, как они работают вместе (`DESDOC.md`). Внутреннее устройство
сервера — в [DesDoc.md](DesDoc.md).

## Сборка

Нужны Qt 6.2 или новее с модулями Core, Network и Sql (драйвер SQLite входит в
Qt; модуль Test — для тестов, Widgets — для админ-панели), CMake 3.16+ и
компилятор C++17. Регулярно сервер собирается на Windows 10 с Qt 6.5.3 и
MinGW 11.2; сборка на Linux описана по пакетам дистрибутивов.

Общий путь на любой системе:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Тесты собираются вместе с сервером (`-DSERVER_BUILD_TESTS=OFF` их выключает),
админ-панель — если в наборе Qt есть Widgets (`-DSERVER_BUILD_ADMIN=OFF`
выключает и её; на машине без графики её просто не будет).

### Windows: батники

Проще всего — батником из корня проекта: он настроит CMake, соберёт сервер и
админ-панель и прогонит оба набора тестов, а пути к Qt возьмёт из
`build-env.bat`.

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

### Windows: установщики

```bat
build-installer.bat                 :: build-release\installer\CalendarServer-<версия>-x64.msi и -setup.exe
build-installer.bat -out D:\Pkg     :: в свою папку
```

Нужен WiX Toolset 3 (переменная `WIX` или путь к папке с `candle.exe` в
`SERVER_WIX`). Скрипт собирает сервер и админ-панель и прогоняет тесты в
`build-release` (не в `build`, где мог работать запущенный сервер),
раскладывает Qt `windeployqt` и упаковывает в MSI для 64-битной Windows и в
setup.exe. Исходники пакетов — `packaging/windows/`.

Что делает установка:

- программы — в Program Files (папку можно сменить);
- сервер становится **службой Windows** `CosmosCalendarServer`: без окна,
  от имени LocalService, запускается вместе с системой и **сразу после
  установки**, после сбоя перезапускается;
- папка данных службы — `%ProgramData%\CalendarPlanner\CalendarServer`:
  `server.ini` (ставится один раз, правки сохраняются при обновлении),
  `server.db`, журнал `server.log`, `backups\`. Открыть её могут только
  администраторы и сама служба;
- правило брандмауэра для сервера в частных сетях;
- ярлыки **админ-панели** на рабочем столе и в «Пуске» — флажки «Добавить
  иконку админ-панели на рабочий стол» и «… в меню «Пуск»», оба отмечены; в
  конце — «Открыть админ-панель сервера»;
- переход с версии 1.0, которая работала в окне консоли: такой сервер из той
  же папки установщик закрывает, а служба забирает копию его базы из
  `%APPDATA%\CalendarPlanner\CalendarServer` того, кто ставит, — пока у неё
  нет своих аккаунтов. Прежняя база остаётся на месте нетронутой.

При удалении убираются служба и правило брандмауэра; папка данных с базой
остаётся.

### Windows: Qt Creator

Откройте `CMakeLists.txt` (или `CalendarServer.pro`, для панели —
`admin/CalendarServerAdmin.pro`), выберите набор «Desktop Qt 6.x MinGW
64-bit» и соберите. Тесты — цели `tst_server` и `tst_admin`, панель —
`CalendarServerAdmin`.

### Windows: командная строка

С набором MinGW из поставки Qt:

```bat
set PATH=D:\Qt\Tools\CMake_64\bin;D:\Qt\Tools\Ninja;D:\Qt\Tools\mingw1120_64\bin;D:\Qt\6.5.3\mingw_64\bin;%PATH%
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:\Qt\6.5.3\mingw_64
cmake --build build
build\tst_server -o result.txt,txt
build\tst_admin -o result-admin.txt,txt
windeployqt --no-translations build\CalendarServer.exe
windeployqt --no-translations build\CalendarServerAdmin.exe
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

Сборка через qmake тоже возможна и описывает те же программы (без тестов —
они собираются только CMake), у сервера и панели по своему файлу:

```bash
mkdir build-qmake && cd build-qmake
qmake ../CalendarServer.pro
make                                 # Windows + MinGW: mingw32-make release

mkdir build-qmake-admin && cd build-qmake-admin
qmake ../admin/CalendarServerAdmin.pro
make
```

## Запуск

Поставленный установщиком сервер — служба: он уже работает, и запускать его
не нужно; управляет им админ-панель (ниже). Вручную сервер запускают для
разработки и проверок.

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
2026-09-21 21:33:14 CalendarServer 1.1.1 слушает порт 8765
2026-09-21 21:33:14 База данных: C:\Users\…\AppData\Roaming\CalendarPlanner\CalendarServer\server.db
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
| `--log-file` | журнал только в консоль; относительный путь — от папки данных |
| `--service` | обычный запуск в консоли |

В файле настроек (`deploy/server.ini.example`, группа `[server]`) есть ещё
журнал в файл (`log_file`), предельный размер запроса (`max_body_mb`, 64 МБ)
и срок жизни ключа сеанса (`token_lifetime_days`, 365 дней). Параметры
командной строки сильнее файла.

Журнал идёт в стандартный вывод: одна строка на запрос с адресом клиента,
кодом ответа и временем обработки. С `--log-file` (`log_file`) — ещё и в
файл; когда тот дорастает до 4 МБ, он становится `server.log.1`, и
начинается новый.
Ctrl+C или SIGTERM останавливают сервер.

`--service` — работа службой. На Windows так сервер запускает диспетчер
служб: без консоли, с журналом в `server.log` папки данных, и остановка —
командой службы; из обычной консоли с этим ключом сервер отказывается
запускаться. На Linux ключ только включает `server.log` — его читает
админ-панель, — а журнал systemd остаётся как был.

## Админ-панель

`CalendarServerAdmin` — окно администратора сервера на той же машине: в
«Пуске» и на рабочем столе — «Админ-панель сервера Космос календаря».
Без аргументов она работает с установленной службой: настройки и базу
берёт из командной строки службы, а так как база службы и сама служба
доступны только администраторам, панель при запуске спрашивает права
администратора (UAC). Работе сервера она не мешает: базу читает на своём
соединении рядом с ним.

| Страница | Что на ней |
|---|---|
| Сервер | работает ли служба, с какого времени, отвечает ли сервер и какой версии; «Запустить», «Остановить», «Перезапустить»; порт, регистрация, адреса для планеров; где база, настройки и журнал, «Изменить настройки…» |
| Пользователи | аккаунты с поиском; у выбранного — когда создан и последний обмен, обменов за 30 дней, объём; вкладки «Календари» (свой, открытый другим — с участниками и их правами, близнец чужого — чей и с каким доступом), «События», «Задачи», «Прочее» (привычки, напоминания, журнал выполнений, композиции…), «Устройства»; двойной щелчок открывает запись целиком; «Сменить пароль…» |
| Аналитика | аккаунты и новые за 30 дней, активные за 24 часа, 7 и 30 дней, обмены, устройства, записи, размер базы; графики активных пользователей (за день, 7 и 30 дней) и обменов по дням за 30, 90 дней или год, у каждого — таблица; место по видам записей и по аккаунтам, общие календари и ссылки |
| Резервные копии | копия базы на ходу, список копий с числом аккаунтов, «Восстановить из выбранной…», «Восстановить из файла…», «Сохранить копию как…» |
| Журнал | конец `server.log`, обновляется сам; только предупреждения и ошибки — флажком |

Новый пароль, как и `--set-password`, выводит из аккаунта все его
устройства. Восстановление останавливает сервер на время замены, откладывает
текущую базу в папку копий (`before-restore-…`) и запускает сервер снова;
устройства об откате сами не узнают, и то, что на них менялось после копии,
планер отправит заново командой «Выгрузить всё заново…» в меню аккаунта.

```bat
CalendarServerAdmin                          :: установленная служба (спросит UAC)
CalendarServerAdmin --config D:\srv\server.ini   :: сервер другой папки - без службы и UAC
CalendarServerAdmin -d D:\srv                :: то же по папке данных
```

Со своей папкой панель службой не управляет, а только показывает её; так её
наводят на тестовый сервер. Если службы нет, панель открывает папку данных
сервера в консоли (`%APPDATA%\CalendarPlanner\CalendarServer`).

Статистика активности ведётся с версии 1.1: сервер считает каждый обмен в
таблицу `activity` своей базы. У базы прежней версии история начинается с
того дня, когда каждое её устройство заходило в последний раз.

### Администрирование

```bash
CalendarServer --list-users                  # аккаунты, число записей и сеансов
CalendarServer --set-password anna           # новый пароль читается из stdin;
                                             # все устройства аккаунта выйдут
CalendarServer --delete-user anna            # аккаунт вместе с данными
```

Команды работают с той же базой, что и сервер (учитывают `-d` и `--config`).

`--import-db <файл>` переносит базу из файла в папку данных сервера — если в
его собственной базе ещё нет аккаунтов — и выходит. Так установщик переносит
в службу базу прежнего сервера; исходный файл только читается.

### Резервная копия

Копии делает админ-панель, на ходу. Вручную: вся база — один файл `server.db`
в папке данных (рядом, пока сервер работает, лежат `server.db-wal` и
`server.db-shm`). Копировать его руками надёжно только при остановленном
сервере: при остановке сервер переносит журнал WAL в сам файл.

### Брандмауэр

Телефон должен достучаться до порта сервера. Установщик добавляет правило
для сервера в частных сетях сам. Для сервера, запущенного не из установки,
если Windows не спросила разрешения при первом запуске, правило добавляется
вручную (PowerShell от имени администратора):

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
`cmake --install`) и данные в `/var/lib/calendar-server`. Админ-панель на
Linux — `sudo CalendarServerAdmin`: она спрашивает systemd о службе
`calendar-server`, читает базу из её папки и запускает и останавливает её
через `systemctl` (на Linux панель пока не проверялась).

### Служба Windows без установщика

Установщик делает всё сам. Для своей сборки, разложенной `windeployqt`, —
PowerShell от имени администратора; папка данных и её `server.ini` (с
`data_dir=.` и `log_file=server.log`, как `packaging/windows/service.ini`)
должны быть доступны службе на запись:

```powershell
New-Service -Name CosmosCalendarServer -StartupType Automatic `
    -DisplayName "Космос календарь: сервер синхронизации" `
    -BinaryPathName '"C:\CalendarServer\CalendarServer.exe" --service --config "C:\ProgramData\CalendarPlanner\CalendarServer\server.ini"'
Start-Service CosmosCalendarServer
```

Без `-Credential` служба работает от имени LocalSystem; установщик ставит её
от LocalService.

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

`tests/tst_server.cpp` — 27 тестов. Они поднимают сервер внутри процесса на
свободном порту и проверяют его настоящими HTTP-запросами: регистрацию и вход,
ошибки, передачу изменений между устройствами, разрешение конфликтов,
удаление, выход, смену пароля, блокировку подбора, конвейер запросов, разбор
некорректного HTTP и общие календари целиком; а ещё счёт обменов, копию базы
на ходу, перенос базы (`--import-db`), отказ открыть базу более новой схемы
и историю у базы прежней версии.

`tests/tst_admin.cpp` — 10 тестов админ-панели без окна: что она читает из
базы (аккаунты, календари с близнецами, записи, статистику и историю по
дням), новый пароль, копию и восстановление, командную строку службы и
чтение записей планера.

```bash
ctest --test-dir build --output-on-failure
./build/tst_server -o result.txt,txt          # Windows: вывод в файл
./build/tst_admin -o result-admin.txt,txt
./build/tst_server linkRulesAreKept           # одна функция
```

Со стороны планера связку проверяет его тест `syncServiceTalksToRealServer`
(`PLANNER_TEST_SERVER=127.0.0.1:8765`) — для него сервер запускается отдельно,
со своей папкой данных.

## Структура исходников

```
src/
  main.cpp          точка входа: разбор параметров, команды администрирования,
                    журнал в консоль и в файл, остановка по Ctrl+C и SIGTERM
  service.*         служба Windows: ответы диспетчеру служб
  config.*          настройки: INI-файл и командная строка поверх него (общие
                    с админ-панелью), адреса для планеров
  httpserver.*      HTTP/1.1 поверх QTcpServer
  api.*             JSON API: маршруты, вход, ключи сеансов, защита от подбора
  storage.*         SQLite: аккаунты, ключи, записи, сам обмен, счёт обменов,
                    копия и перенос базы
  shares.*          общие календари: доступ, ссылки, разводка правок
admin/
  main.cpp          админ-панель: какую базу открыть, права администратора
  admindb.*         чтение базы для панели и новый пароль
  backups.*         копии базы и восстановление
  records.*         поля записей планера, которые панель показывает
  servicecontrol.*  служба: диспетчер служб Windows или systemd
  adminsession.*    общее для страниц: настройки, служба, база, пинг
  mainwindow.*      окно со списком страниц
  serverpage.*, userspage.*, statspage.*, backuppage.*, logpage.*  страницы
  chart.*           графики аналитики (без Qt Charts)
  look.*, page.*    оформление и основа страницы
  admin.rc          значок и имя программы для Windows
tests/
  tst_server.cpp    тесты сервера по HTTP
  tst_admin.cpp     тесты данных админ-панели
deploy/             пример server.ini и служба systemd
packaging/windows/  установщики WiX: MSI, setup.exe, его русские строки,
                    service.ini (настройки службы), firewall-ru.wxl
packaging/linux/    заглушка: пакеты Linux будут собираться в Docker
build-installer.bat установщики Windows
build-server.bat    сборка и тесты на Windows
run-server.bat      запуск на Windows
build-env.bat       пути к Qt и инструментам (SERVER_*)
```

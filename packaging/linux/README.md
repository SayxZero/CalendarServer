# Linux: сборка в Docker

`Dockerfile` здесь собирает сервер на Ubuntu 26.04 LTS с Qt из её
репозиториев (6.10) и делает из этого три вещи:

- **архив** `CalendarServer-<v>-ubuntu26.04-x64.tar.gz`: `bin/` — сервер,
  бенчмарк и админ-панель, `deploy/` — служба systemd и пример `server.ini`,
  `INSTALL.txt` — что поставить и как запустить;
- **отчёты тестов** `tests/tst_server.txt`, `tst_bench.txt`, `tst_admin.txt`.
  Тесты идут внутри сборки образа, и архив выгружается только после них;
- **образ сервера** `runtime`: Ubuntu 26.04 с библиотеками Qt, сервер не от
  root (`calendar`), база в томе `/data`, порт 8765, проверка здоровья —
  `GET /api/ping`.

Контекст сборки — корень проекта; что в него попадает, перечисляет
`Dockerfile.dockerignore` (только исходники: в `build*` лежат сборки Windows
вместе с Qt).

```bash
docker build -f packaging/linux/Dockerfile --target dist --output type=local,dest=out .
docker build -f packaging/linux/Dockerfile --target runtime -t calendar-server .
docker run -d -p 8765:8765 -v calendar-data:/data calendar-server
```

Ключи сервера дописываются после имени образа (`--no-registration`,
`--trusted-proxies …`), папку данных и порт задаёт сам образ.

На машине разработки (Windows) всё это запускает `build-linux.ps1`
зонтичного репозитория CosmosCalendar — Docker Engine в WSL, результат в
`Build/Linux`; там же, в `PACKAGING.md`, — установка Docker и что дальше:
`.deb` с `sysusers.d` и `tmpfiles.d`, сборки для других дистрибутивов.

`tst_admin` на Linux проходит без одного теста: проверка службы Windows
пропускается (`QSKIP`), systemd с юнитом в контейнере нет.

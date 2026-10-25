# Пакеты Linux — заглушка

Сборка пакетов сервера для Linux ещё не настроена. Её место — здесь; общий
`build-packages.bat` зонтичного репозитория пока пропускает этот шаг и
оставляет в `Build/Linux/` только записку.

Задумано (подробнее — `PACKAGING.md` зонтичного репозитория CosmosCalendar):

- сборка в Docker: образ на Debian 12 с `qt6-base-dev`, `libqt6sql6-sqlite`,
  CMake и Ninja;
- `.deb` через CPack DEB: `/usr/bin/calendar-server`, unit systemd (из
  `deploy/calendar-server.service`, путь к программе — через `configure_file`),
  пользователь через `sysusers.d`, папка данных `/var/lib/calendar-server`
  через `tmpfiles.d`, пример `/etc/calendar-server/server.ini`; `remove` и
  `purge` базу не трогают;
- Docker-образ сервера (`debian:12-slim`, не root, `VOLUME /data`,
  `EXPOSE 8765`, проверка по `GET /api/ping`) — для развёртывания где угодно.

Результат — в `Build/Linux/` зонтичного репозитория, как у Windows и Android.

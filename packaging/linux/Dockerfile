# Linux build of the sync server on Ubuntu 26.04 with the Qt of the
# distribution: the server, the benchmark and the admin panel, their tests,
# an archive of the programs and an image to run the server in. The context
# is the root of the project:
#
#   docker build -f packaging/linux/Dockerfile --target dist --output type=local,dest=<folder> .
#   docker build -f packaging/linux/Dockerfile --target runtime -t calendar-server .
#
# dist holds the archive and the reports of the tests, and it is made only
# after the tests have passed. build-linux.ps1 of the CosmosCalendar umbrella
# runs both from Windows (Docker Engine in WSL).

ARG UBUNTU=26.04

# ---- the compiler, CMake and Qt from the archive of the distribution --------
FROM ubuntu:${UBUNTU} AS toolchain
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++ cmake ninja-build \
        qt6-base-dev libqt6sql6-sqlite \
 && rm -rf /var/lib/apt/lists/*

# ---- build ------------------------------------------------------------------
FROM toolchain AS build
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build

# ---- tests: a failed one stops the build, and nothing is exported ----------
# tst_admin needs no window, the platform is given anyway: a test that opens
# one must not look for a display.
FROM build AS test
ENV QT_QPA_PLATFORM=offscreen
RUN mkdir /reports && cd /build \
 && for t in tst_server tst_bench tst_admin; do \
        ./$t -o /reports/$t.txt,txt -o -,txt || exit 1; \
    done

# ---- the archive --------------------------------------------------------------
# The texts lose a CR a Windows checkout may have given them: systemd reads it
# as part of the value.
FROM build AS package
ARG UBUNTU
COPY --from=test /reports /dist/tests
RUN set -e; \
    version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' /build/CMakeCache.txt); \
    name=CalendarServer-$version-ubuntu$UBUNTU-x64; \
    root=/stage/$name; \
    install -d $root/bin $root/deploy; \
    for p in CalendarServer CalendarServerBench CalendarServerAdmin; do \
        if [ -f /build/$p ]; then install -m 755 -s /build/$p $root/bin/; fi; \
    done; \
    for f in deploy/calendar-server.service deploy/server.ini.example; do \
        sed 's/\r$//' $f > $root/$f; \
    done; \
    sed 's/\r$//' packaging/linux/INSTALL.txt > $root/INSTALL.txt; \
    tar -C /stage --owner=0 --group=0 --numeric-owner -czf /dist/$name.tar.gz $name

FROM scratch AS dist
COPY --from=package /dist/ /

# ---- the image of the server ----------------------------------------------------
# Not root, the base in the volume /data, the port of the planners. The check
# asks /api/ping with bash alone: the image has no curl.
FROM ubuntu:${UBUNTU} AS runtime
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libqt6network6 libqt6sql6-sqlite \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --home-dir /data --no-create-home calendar \
 && install -d -o calendar -g calendar /data
COPY --from=build /build/CalendarServer /usr/local/bin/CalendarServer
USER calendar
VOLUME /data
EXPOSE 8765
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s CMD \
    bash -c 'exec 3<>/dev/tcp/127.0.0.1/8765 && printf "GET /api/ping HTTP/1.0\r\n\r\n" >&3 && head -n 1 <&3 | grep -q " 200 "'
ENTRYPOINT ["/usr/local/bin/CalendarServer", "--data-dir", "/data"]

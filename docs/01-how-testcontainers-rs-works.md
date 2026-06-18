# Как устроен testcontainers-rs и что нужно для порта на нативный C++

> Анализ исходников `testcontainers-rs` v0.27.3 (склонированы в `_research/testcontainers-rs`).
> Документ — основа для реализации `testcontainers-cpp`.

## 1. Суть библиотеки

testcontainers — это обёртка над **Docker Engine HTTP API**. Она не «эмулирует» Docker и не
вызывает CLI `docker`. Она открывает соединение с демоном Docker и шлёт ему HTTP-запросы
(create/start/inspect/logs/exec/remove контейнеров и сетей). Всё, что делает библиотека:

1. Найти, где слушает Docker (сокет/pipe/tcp).
2. Создать контейнер из образа с нужной конфигурацией.
3. Запустить и **дождаться готовности** (wait strategies).
4. Узнать, на какие **хостовые порты** Docker опубликовал порты контейнера.
5. Отдать пользователю «ручку» (handle), через которую он ходит в контейнер.
6. **Удалить** контейнер, когда ручка уничтожается (RAII / Drop).

В Rust за HTTP-общение с Docker отвечает крейт **bollard**. В C++ аналога «из коробки» нет —
его придётся написать самим (это основная работа порта).

## 2. Транспорт: как соединяемся с Docker

`core/client/bollard_client.rs` выбирает транспорт по схеме URL хоста:

| Схема    | Транспорт                        | Платформа |
|----------|----------------------------------|-----------|
| `unix://`| Unix-domain socket `/var/run/docker.sock` | Linux/Mac |
| `npipe://`| Named pipe `//./pipe/docker_engine` | Windows  |
| `tcp://`/`http://` | обычный TCP                | все       |
| `https://`| TCP + TLS (key/cert/ca .pem)    | все       |

Поверх любого транспорта идёт **HTTP/1.1**. Запросы версионируются префиксом `/v1.x/...`.
На каждый запрос добавляется заголовок `User-Agent: tc-rust/<version>`. Таймаут по умолчанию 120 c.

### Резолвинг хоста (порядок приоритетов, `core/env/config.rs`)
1. `tc.host` из `~/.testcontainers.properties`
2. env `DOCKER_HOST`
3. `docker.host` из `~/.testcontainers.properties`
4. дефолтный сокет `/var/run/docker.sock` (если существует)
5. rootless-сокеты по очереди:
   - `${XDG_RUNTIME_DIR}/.docker/run/docker.sock`
   - `${HOME}/.docker/run/docker.sock`
   - `${HOME}/.docker/desktop/docker.sock`
6. жёсткий дефолт: `unix:///var/run/docker.sock` (Unix) или `npipe:////./pipe/docker_engine` (Windows)

Доп. env: `DOCKER_TLS_VERIFY`, `DOCKER_CERT_PATH`, `TESTCONTAINERS_COMMAND` (`keep`/`remove`),
`DOCKER_DEFAULT_PLATFORM`.

## 3. Полный перечень вызовов Docker Engine API

Это «контракт», который C++ HTTP-клиент должен покрыть.

| Операция | HTTP метод + путь | Что важно |
|---|---|---|
| Создать контейнер | `POST /containers/create?name=&platform=` | тело: Image, Cmd, Env, Labels, ExposedPorts, HostConfig{PortBindings/PublishAllPorts, NetworkMode, Mounts, Privileged, Ulimits, ...}, Healthcheck |
| Запустить | `POST /containers/{id}/start` | — |
| Инспект | `GET /containers/{id}/json` | читаем `State.Running/ExitCode/Health.Status`, `NetworkSettings.Ports` |
| Список | `GET /containers/json?all=true&filters=` | фильтр по label/name/network (для reuse) |
| Остановить | `POST /containers/{id}/stop?t=` | — |
| Удалить | `DELETE /containers/{id}?force=true&v=true` | force + удалить анонимные тома |
| Pause/Unpause | `POST /containers/{id}/pause`/`/unpause` | — |
| Логи | `GET /containers/{id}/logs?follow&stdout&stderr&tail=all` | **мультиплекс-фрейминг**, см. §6 |
| Копировать в | `PUT /containers/{id}/archive?path=/` | тело = tar, `Content-Type: application/x-tar` |
| Копировать из | `GET /containers/{id}/archive?path=` | ответ = tar-поток |
| Exec create | `POST /containers/{id}/exec` | тело: Cmd, AttachStdout/Stderr, Env |
| Exec start | `POST /exec/{id}/start` | Detach=false → мультиплекс-поток в ответ |
| Exec inspect | `GET /exec/{id}/json` | читаем `ExitCode` |
| Pull образа | `POST /images/create?fromImage=&platform=` | заголовок `X-Registry-Auth` (base64 JSON), ответ — поток JSON-прогресса |
| Inspect образа | `GET /images/{name}/json` | 404 ⇒ образа нет |
| Build образа | `POST /build?dockerfile=&t=&version=2&...` | тело = tar контекста |
| Remove образа | `DELETE /images/{name}` | — |
| Create сети | `POST /networks/create` | тело: Name |
| Inspect сети | `GET /networks/{id}/json` | gateway IP (для запуска внутри контейнера) |
| List сетей | `GET /networks` | поиск по имени |
| Remove сети | `DELETE /networks/{id}` | — |

**Pull ленивый:** образ тянется не заранее, а если `create` вернул 404 — тогда `POST /images/create`,
потом повтор `create`.

## 4. Жизненный цикл `.start()` (по `runners/async_runner.rs`)

1. Свернуть `Image` + builder-вызовы в один `ContainerRequest`.
2. Получить общий клиент Docker (ленивый синглтон).
3. Собрать labels (всегда добавляется `org.testcontainers.managed-by=testcontainers`).
4. (reuse) если включён — найти существующий контейнер по label/name и переиспользовать.
5. Собрать тело `POST /containers/create` (см. §5 — модель данных).
6. Создать сеть, если задана (`POST /networks/create`, с дедупликацией по имени).
7. `create_container`; при 404 → pull образа → повтор.
8. Скопировать файлы в контейнер (`PUT .../archive`).
9. Запуск + ожидание готовности под общим таймаутом (по умолчанию **60 c**):
   - `POST /containers/{id}/start`
   - выполнить `exec_before_ready`-команды
   - прогнать **wait strategies** по порядку (см. §7)
   - выполнить `exec_after_start`-команды
10. Вернуть handle (`ContainerAsync`). При его уничтожении → `DELETE /containers/{id}`.

## 5. Модель данных контейнера (`ContainerRequest`)

Главные поля (то, что нужно как builder-API в C++):

- image (name, tag), container_name, platform
- env_vars, cmd (override), entrypoint, working_dir, user
- exposed_ports, ports (явный маппинг host↔container)
- network, hostname, hosts (extra `/etc/hosts`)
- labels
- mounts (bind / volume / tmpfs), copy_to_sources (файлы/данные внутрь)
- privileged, cap_add/cap_drop, readonly_rootfs, security_opts, shm_size,
  cgroupns_mode, userns_mode, ulimits, device_requests, open_stdin
- startup_timeout, ready_conditions (override), health_check
- log_consumers, host_config_modifier (escape hatch — колбэк, правящий HostConfig напрямую)

`Image` — это «шаблон с дефолтами»: обязательны `name()`, `tag()`, `ready_conditions()`;
опционально env/cmd/entrypoint/expose_ports/mounts/exec-хуки. `GenericImage` — готовая
реализация: `GenericImage::new("redis","7.2.4").with_exposed_port(6379.tcp()).with_wait_for(...)`.

## 6. Мультиплекс-формат логов (КРИТИЧНО для порта)

Когда у контейнера нет TTY, `GET /logs` и `POST /exec/{id}/start` отдают поток кадров:

```
[ 1 байт: тип потока (1=stdout, 2=stderr) ]
[ 3 байта: нули ]
[ 4 байта: длина payload, big-endian uint32 ]
[ N байт: собственно данные ]
```

Нужно парсить инкрементально: прочитать 8-байтовый заголовок, затем N байт, повторять.
Стратегия «ждать строку в логах» — это **поиск подстроки** (не regex!) по байтам с `tail=all`
(история тоже сканируется).

## 7. Wait strategies (как понять, что контейнер готов)

Прогоняются последовательно (логическое И), общий таймаут 60 c, у самих стратегий своего
таймаута нет (только интервал опроса).

| Стратегия | Что ждёт | Механизм |
|---|---|---|
| `Log` | подстрока в stdout/stderr N раз | стримит `GET /logs?follow&tail=all`, ищет подстроку |
| `Http` | HTTP-ответ удовлетворяет matcher | сам ходит на **хостовый** порт; ошибки соединения = «ещё не готов», опрос 100 мс |
| `Healthcheck` | `State.Health.Status == healthy` | опрос `inspect` 100 мс; `unhealthy` → fail fast; нет healthcheck → ошибка |
| `Exit` | контейнер вышел (опц. с кодом) | опрос `inspect.State.Running` 100 мс |
| `Duration` | просто поспать | sleep |

Healthcheck определяется отдельно (в теле `create`, поля в наносекундах, `test=["CMD"/"CMD-SHELL"/"NONE"/[], ...]`),
а ожидание `healthy` — отдельная стратегия.

## 8. Порты: как узнать хостовый порт

1. На `create` ставим `HostConfig.PublishAllPorts=true` (или явный `PortBindings`).
2. После старта `GET /containers/{id}/json` → `NetworkSettings.Ports`:
   ```json
   "8333/tcp": [ {"HostIp":"0.0.0.0","HostPort":"33077"}, {"HostIp":"::","HostPort":"49718"} ]
   ```
3. Разбираем ключ `"<порт>/<proto>"`, по `HostIp` раскладываем на ipv4/ipv6 маппинги.
4. Хост для подключения: для tcp/http — хост из URL; для unix/npipe — `localhost`
   (или gateway bridge-сети, если сами крутимся внутри контейнера — детект по `/.dockerenv`).

## 9. Очистка контейнеров — ВАЖНОЕ РАСХОЖДЕНИЕ

В этой Rust-версии **нет Ryuk** (ни одного упоминания в коде). Очистка держится на:
1. **RAII / Drop** — деструктор контейнера зовёт `DELETE /containers/{id}`.
2. Опциональный **watchdog** (фича, по умолчанию выключена) — ловит SIGTERM/SIGINT/SIGQUIT
   и удаляет зарегистрированные контейнеры.

Проблема обоих: при `SIGKILL`/краше/`abort()` деструктор не вызывается → **осиротевшие контейнеры**.
Канонические testcontainers (java/go/python) решают это через **Ryuk** — sidecar-контейнер-реапер:
- запускают образ `testcontainers/ryuk` (privileged, с бинд-маунтом docker.sock), порт 8080;
- по TCP шлют ему строки-фильтры `label=org.testcontainers.session-id=<uuid>\n`;
- когда TCP-соединение рвётся (процесс умер) — Ryuk сам сносит всё по фильтру.

**Рекомендация для C++:** деструкторы C++ так же не вызываются при `SIGKILL`/`abort`, поэтому
для надёжной очистки лучше сразу заложить **Ryuk-модель** (плюс RAII как быстрый путь),
а не только RAII+сигналы. Все ресурсы тегировать `org.testcontainers.managed-by` и session-id label.

## 10. Что нужно реализовать на нативном C++ (чек-лист)

**Зависимости (Rust → C++ аналог):**

| Что делает | Rust крейт | C++ вариант |
|---|---|---|
| HTTP к Docker | bollard | свой HTTP/1.1 поверх сокета/pipe; или libcurl (умеет `--unix-socket`), Boost.Beast/Asio |
| TLS | rustls/ring | OpenSSL |
| JSON | serde_json | nlohmann/json или RapidJSON |
| tar (copy/build) | astral-tokio-tar | libarchive или свой генератор USTAR |
| Unix socket / npipe | tokio | Asio (есть и то, и другое) / WinAPI named pipe |
| Поиск подстроки в логах | memchr | std::string::find / std::search |
| Base64 (X-Registry-Auth) | — | любая мелкая реализация |
| Креды Docker | docker_credential | парсинг `~/.docker/config.json` (auths/credHelpers) |

**Минимальный план реализации (по слоям):**

1. **DockerClient** — HTTP/1.1 поверх unix-socket / named pipe / tcp(+TLS); резолвинг хоста (§2);
   методы под все эндпоинты (§3); парсер мультиплекс-потока (§6).
2. **Модель** — `ContainerRequest` (§5), `Image`/`GenericImage`, сериализация тела `create` в JSON.
3. **Runner** — `start()` (§4): create→(pull при 404)→copy→start→wait→handle.
4. **Wait strategies** (§7) — Log/Http/Healthcheck/Exit/Duration; общий таймаут 60 c.
5. **Порты** (§8) — разбор `NetworkSettings.Ports`, `get_host_port(...)`.
6. **Очистка** — RAII-handle (деструктор → remove) + **Ryuk** для краш-устойчивости (§9).
7. Доп.: сети, mounts, copy-to/from (tar), exec, логи-консьюмеры, реестр-аутентификация.

**Первый вертикальный срез (MVP), чтобы проверить подход:**
поднять `GenericImage("redis","7.2.4")` с `expose 6379`, `WaitFor::log("Ready to accept connections")`,
получить хостовый порт, подключиться, в деструкторе удалить контейнер. Это задействует
create/start/inspect/logs/remove — ядро всего остального.

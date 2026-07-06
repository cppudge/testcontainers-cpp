# Зависимости testcontainers-cpp (решение)

> Дата решения: 2026-06-19. Источник версий: публичный ConanCenter (`center2.conan.io`), Conan 2.26.2.

## Транспортная база: Boost.Beast + Boost.Asio

Выбрана как **единственный** вариант в ConanCenter, который нативно покрывает все три
транспорта Docker Engine API одновременно:

| Транспорт | Тип в Asio | Зачем |
|---|---|---|
| unix-сокет (Linux/Mac) | `boost::asio::local::stream_protocol::socket` | дефолт `/var/run/docker.sock` |
| **named pipe (Windows)** | `boost::asio::windows::stream_handle` | дефолт `//./pipe/docker_engine` |
| TCP / TLS | `tcp::socket` / `boost::asio::ssl::stream` | удалённый Docker, CI |

Boost.Beast поверх любого из этих стримов даёт корректный HTTP/1.1-парсер (включая chunked),
а низкоуровневый доступ к стриму — стриминг логов/`pull` и **hijack** соединения для
`POST /exec/{id}/start` (полнодуплекс по тому же сокету). Это ровно то, что не умеют
libcurl/cpr/cpp-httplib на Windows (нет named pipe) и в части hijack.

Важно: нужны только **header-only** части Boost (Beast, Asio, System), поэтому в conanfile
стоит `boost/*:header_only=True` — сам Boost не компилируется, из бинарных зависимостей
остаётся только OpenSSL.

## Полный набор (pinned)

| Пакет | Версия | Назначение |
|---|---|---|
| `boost` | 1.91.0 | Beast (HTTP) + Asio (транспорт: unix/npipe/tcp), header-only |
| `openssl` | 3.6.3 | TLS для `https://`/`tcp+tls` Docker; стабильная ветка 3.x (4.0.1 пока слишком новый). **Опционален** |
| `nlohmann_json` | 3.12.0 | сериализация тел `create`/exec и разбор `inspect`/`Ports`/`Health` |
| `libarchive` | 3.8.7 | tar для copy-to/from контейнера (`PUT/GET .../archive`) и build-контекста |
| `libssh2` | 1.11.1 | SSH-туннель для `with_exposed_host_port` (sshd-сайдкар); крипто-бэкенд — OpenSSL. **Опционален** |

Опциональность: CMake-опция `TC_TLS` / conan-опция `tls` выключает TLS-транспорт, а
`TC_HOST_PORT_FORWARDING` / `host_port_forwarding` — sshd-туннель (и с ним libssh2).
Обе по умолчанию ON; выключение **обеих** полностью убирает OpenSSL из графа зависимостей
(одного `tls=False` мало — libcrypto остаётся бэкендом libssh2). Отключённая фича
падает громко: `https://`-хост или `with_exposed_host_port` бросают `DockerError`
с именем опции.

## Почему не остальные (кратко)

- **libcurl 8.20.0 / cpr 1.14.2** — нет Windows named pipe (дефолтный канал Docker Desktop);
  hijack для exec — через `CURLOPT_CONNECT_ONLY`, костыльно. Отлично только на Linux/Mac.
- **cpp-httplib 0.47.0** — нет named pipe, слабый стрим/hijack (хотя header-only и приятный).
- **poco 1.15.2 / drogon 1.9.13** — нет named pipe, тяжёлые.
- **curlpp / civetweb** — обёртка curl / в основном сервер.
- **restclient-cpp, mongoose** — в публичном ConanCenter отсутствуют.

## Заметки по окружению

- В `conan remote list` публичного ConanCenter не было — добавлен вручную:
  `conan remote add conancenter https://center2.conan.io` (убрать: `conan remote remove conancenter`).
- Для корпоративной сборки эти 4 пакета нужно будет также иметь в `extsdk`-мирроре (art.iss.ru).
- Standalone `asio/1.38.0` остаётся запасным вариантом (легче по зависимостям, но HTTP/chunked
  пришлось бы писать самим) — если Boost окажется нежелательным.

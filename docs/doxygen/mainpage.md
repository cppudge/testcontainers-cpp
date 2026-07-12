# API reference {#mainpage}

Generated from the public headers of
[testcontainers-cpp](https://github.com/cppudge/testcontainers-cpp) — the native C++20 port
of Testcontainers. Guides, quick starts, and module walkthroughs live on the
[documentation site](../index.html).

Everything below lives in `namespace testcontainers`; include
`testcontainers/testcontainers.hpp` for the whole core API, or the individual headers named
on each page.

## Core entry points

- testcontainers::GenericImage — the copyable container builder; `start()` returns a running
  testcontainers::Container (move-only RAII handle).
- testcontainers::GenericBuildableImage — build an image from a Dockerfile + context, then
  run it.
- The `testcontainers::wait_for` namespace — the seven readiness strategies accepted by
  `GenericImage::with_wait`.
- testcontainers::Network / testcontainers::Volume — user-defined networks and named
  volumes, RAII and session-labeled.
- testcontainers::DockerComposeContainer — drive a whole compose stack.
- testcontainers::DockerClient — the underlying Docker Engine API client, usable directly
  for anything the typed surface doesn't cover.
- testcontainers::DockerError and its subtypes — the exception hierarchy every failure is
  reported through.

## Modules

Prebuilt technology wrappers (link `testcontainers::modules`, include
`testcontainers/modules.hpp` or a per-module header): testcontainers::modules::RedisImage,
testcontainers::modules::PostgreSQLImage, testcontainers::modules::MySQLImage,
testcontainers::modules::MariaDBImage, testcontainers::modules::KafkaImage,
testcontainers::modules::RabbitMQImage, testcontainers::modules::MongoDBImage,
testcontainers::modules::NATSImage, testcontainers::modules::MosquittoImage — each with its
started-handle twin (`RedisContainer`, …).

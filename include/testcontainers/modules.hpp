#pragma once

/// Umbrella header for the ecosystem modules (namespace
/// testcontainers::modules; link testcontainers::modules). The core API has
/// its own umbrella, testcontainers/testcontainers.hpp — the module headers
/// pull in only the core pieces they build on.

#include "testcontainers/modules/ClickHouse.hpp"
#include "testcontainers/modules/Kafka.hpp"
#include "testcontainers/modules/MariaDB.hpp"
#include "testcontainers/modules/MinIO.hpp"
#include "testcontainers/modules/MongoDB.hpp"
#include "testcontainers/modules/Mosquitto.hpp"
#include "testcontainers/modules/MySQL.hpp"
#include "testcontainers/modules/NATS.hpp"
#include "testcontainers/modules/OpenSearch.hpp"
#include "testcontainers/modules/PostgreSQL.hpp"
#include "testcontainers/modules/RabbitMQ.hpp"
#include "testcontainers/modules/Redis.hpp"
#include "testcontainers/modules/RustFS.hpp"
#include "testcontainers/modules/ScyllaDB.hpp"

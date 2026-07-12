#pragma once

/// Umbrella header for the ecosystem modules (namespace
/// testcontainers::modules; link testcontainers::modules). The core API has
/// its own umbrella, testcontainers/testcontainers.hpp — the module headers
/// pull in only the core pieces they build on.

#include "testcontainers/modules/KafkaContainer.hpp"
#include "testcontainers/modules/MariaDBContainer.hpp"
#include "testcontainers/modules/MongoDBContainer.hpp"
#include "testcontainers/modules/MySQLContainer.hpp"
#include "testcontainers/modules/PostgreSQLContainer.hpp"
#include "testcontainers/modules/RabbitMQContainer.hpp"
#include "testcontainers/modules/RedisContainer.hpp"

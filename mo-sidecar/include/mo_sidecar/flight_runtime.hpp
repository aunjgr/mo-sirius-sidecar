// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "mo_sidecar/config.hpp"

#include <memory>

namespace duckdb {
class DatabaseInstance;
}

namespace matrixone::sidecar {

class flight_runtime {
public:
	flight_runtime(duckdb::DatabaseInstance &database, runtime_config config);
	~flight_runtime() noexcept;

	flight_runtime(const flight_runtime &) = delete;
	flight_runtime &operator=(const flight_runtime &) = delete;

	void start();
	void stop() noexcept;

private:
	class impl;
	std::unique_ptr<impl> impl_;
};

} // namespace matrixone::sidecar

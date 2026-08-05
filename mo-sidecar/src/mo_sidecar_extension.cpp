// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#define DUCKDB_EXTENSION_MAIN

#include "mo_sidecar/mo_sidecar_extension.hpp"

#include "mo_sidecar/config.hpp"
#include "mo_sidecar/flight_runtime.hpp"
#include "mo_sidecar/protocol.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {
namespace {

class runtime_owner final : public ExtensionCallback {
public:
	explicit runtime_owner(std::unique_ptr<matrixone::sidecar::flight_runtime> runtime) : runtime_(std::move(runtime)) {
	}

	~runtime_owner() override {
		runtime_->stop();
	}

private:
	std::unique_ptr<matrixone::sidecar::flight_runtime> runtime_;
};

void load_internal(ExtensionLoader &loader) {
	const auto config = matrixone::sidecar::load_runtime_config();
	if (!config) {
		return;
	}

	auto runtime = std::make_unique<matrixone::sidecar::flight_runtime>(loader.GetDatabaseInstance(), *config);
	runtime->start();
	DBConfig::GetConfig(loader.GetDatabaseInstance())
	    .GetCallbackManager()
	    .Register(make_shared_ptr<runtime_owner>(std::move(runtime)));
}

} // namespace

void MoSidecarExtension::Load(ExtensionLoader &loader) {
	load_internal(loader);
}

std::string MoSidecarExtension::Name() {
	return "mo_sidecar";
}

std::string MoSidecarExtension::Version() const {
	return "0.1.0";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(mo_sidecar, loader) {
	duckdb::load_internal(loader);
}

DUCKDB_EXTENSION_API const char *mo_sidecar_version() {
	return "0.1.0";
}

} // extern "C"

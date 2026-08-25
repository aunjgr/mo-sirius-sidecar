// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "mo_sidecar/config.hpp"
#include "mo_sidecar/stream_input.hpp"
#include "offload/tae_read_resolver.hpp"

#include <duckdb/main/connection.hpp>

namespace matrixone::sidecar {

class matrixone_tae_read_resolver final : public sirius::offload::tae_read_resolver {
public:
	matrixone_tae_read_resolver(duckdb::Connection &connection, const runtime_config &config, std::string query_id,
	                            std::uint64_t account_id, stream_input_registry &stream_inputs);

	std::unique_ptr<sirius::offload::resolved_tae_read>
	resolve(const sirius::offload::tae_read &request, const ::substrait::NamedStruct &requested_schema) override;
	std::unique_ptr<sirius::offload::resolved_stream_read>
	resolve(const sirius::offload::stream_read &request, const ::substrait::NamedStruct &requested_schema) override;

private:
	duckdb::Connection &connection_;
	const runtime_config &config_;
	std::string query_id_;
	std::uint64_t account_id_;
	stream_input_registry &stream_inputs_;
};

} // namespace matrixone::sidecar

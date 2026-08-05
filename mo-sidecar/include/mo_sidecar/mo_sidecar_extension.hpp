// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "duckdb/main/extension.hpp"

namespace duckdb {

class MoSidecarExtension final : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb

// SPDX-License-Identifier: GPL-3.0-only
// Checksum interface derived from dfrood/SA2-Save-Converter,
// Checksum.cs at commit 05f3a606283e4aac131eaa6b87ac1c1ef25e7b3a.
#pragma once

#include "ChaoPoolOverlay.h"

namespace garden_pools
{
	bool HasValidChaoSaveChecksum(const ChaoSaveBlock& chaoSaveBlock);
	void UpdateChaoSaveChecksum(ChaoSaveBlock& chaoSaveBlock);
}

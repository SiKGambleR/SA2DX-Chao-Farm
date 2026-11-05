#pragma once

#include <SA2ModLoader.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace sa2lvl
{
	// The UV coordinates (Signed16 pairs, 256 = one texture repeat) of one GC
	// attach, with the texture IDs its meshes use. The plugin scrolls these
	// in place to animate water.
	struct GcUvSet
	{
		std::int16_t* Data = nullptr;
		std::uint32_t PairCount = 0;
		std::vector<std::uint16_t> TextureIds;
		std::vector<std::int16_t> Base;
	};

	// A relocated SA2 land table read from a locally generated .sa2lvl file.
	// The image owns every model the land table points at.
	struct LoadedLandTable
	{
		std::vector<std::uint8_t> Image;
		LandTable* Table = nullptr;
		std::vector<GcUvSet> UvSets;
	};

	// Throws std::runtime_error when the file is missing or malformed.
	LoadedLandTable Load(const std::filesystem::path& path);
}

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace garden_pools
{
	inline constexpr std::size_t AddedGardenCount = 3;
	inline constexpr std::size_t MaxChaoPerGarden = 8;
	inline constexpr std::size_t MaxRecordBytes = 4096;
	// The garden ground-item array inside the Chao save block: 86 slots of 20
	// bytes (type, area, two flag words, x/y/z floats) at block offset 0xC8.
	inline constexpr std::size_t GardenItemRegionSize = 0x6B8;

	enum class AddedGarden : std::size_t
	{
		StationSquare,
		MysticRuins,
		EggCarrier
	};

	enum class SADXChaoStageNumber : int
	{
		StationSquare = 4,
		EggCarrier = 5,
		MysticRuins = 6
	};

	using ChaoRecord = std::vector<std::uint8_t>;
	using GardenPool = std::vector<ChaoRecord>;
	using AddedGardenPools = std::array<GardenPool, AddedGardenCount>;
	// Per-garden copy of the ground-item array; empty = never visited (the
	// garden starts with no items on the ground).
	using ItemRegion = std::vector<std::uint8_t>;
	using AddedGardenItemRegions = std::array<ItemRegion, AddedGardenCount>;

	inline constexpr std::array<AddedGarden, AddedGardenCount> AddedGardens{
		AddedGarden::StationSquare,
		AddedGarden::MysticRuins,
		AddedGarden::EggCarrier
	};

	const char* SADXStageName(AddedGarden garden);
	SADXChaoStageNumber SADXStageNumber(AddedGarden garden);
	std::optional<AddedGarden> AddedGardenForSADXStageNumber(int stageNumber);
	std::string ChaoSaveFilenameFromBuffer(
		const char* filenameBuffer,
		std::size_t bufferSize);
	bool ChaoSavePathMatches(
		std::string_view activeSavePath,
		std::string_view writerPath) noexcept;
	std::filesystem::path SidecarPathForSave(
		const std::filesystem::path& saveDirectory,
		std::string_view chaoSaveFilename);
	GardenPool& PoolFor(AddedGardenPools& gardens, AddedGarden garden);
	const GardenPool& PoolFor(const AddedGardenPools& gardens, AddedGarden garden);
	ItemRegion& ItemsFor(AddedGardenItemRegions& items, AddedGarden garden);
	const ItemRegion& ItemsFor(const AddedGardenItemRegions& items, AddedGarden garden);
	std::vector<std::uint8_t> Encode(const AddedGardenPools& gardens);
	std::vector<std::uint8_t> Encode(
		const AddedGardenPools& gardens,
		const AddedGardenItemRegions& items);
	AddedGardenPools Decode(const std::vector<std::uint8_t>& bytes);
	void DecodeAll(
		const std::vector<std::uint8_t>& bytes,
		AddedGardenPools& gardens,
		AddedGardenItemRegions& items);
	void Save(const std::filesystem::path& path, const AddedGardenPools& gardens);
	void Save(
		const std::filesystem::path& path,
		const AddedGardenPools& gardens,
		const AddedGardenItemRegions& items);
	AddedGardenPools Load(const std::filesystem::path& path);
	void LoadAll(
		const std::filesystem::path& path,
		AddedGardenPools& gardens,
		AddedGardenItemRegions& items);

	class AddedGardenPoolTransaction
	{
	public:
		explicit AddedGardenPoolTransaction(std::filesystem::path path);

		void Load();
		AddedGardenPools& WorkingPools();
		const AddedGardenPools& WorkingPools() const;
		AddedGardenItemRegions& WorkingItems();
		const AddedGardenItemRegions& WorkingItems() const;
		bool IsDirty() const;
		void Commit();
		void Rollback();

	private:
		void EnsureLoaded() const;

		std::filesystem::path path_;
		AddedGardenPools committedPools_;
		AddedGardenPools workingPools_;
		AddedGardenItemRegions committedItems_;
		AddedGardenItemRegions workingItems_;
		bool loaded_ = false;
	};
}

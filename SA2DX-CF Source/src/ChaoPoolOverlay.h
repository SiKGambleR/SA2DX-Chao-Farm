#pragma once

#include "GardenPoolStore.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace garden_pools
{
	inline constexpr std::size_t ChaoSaveRecordSize = 0x800;
	inline constexpr std::size_t ChaoSaveRecordCount = 24;
	inline constexpr std::size_t ChaoSaveRecordsOffset = 0xA64;
	// Garden ground-item array inside the Chao save block (fruit, seeds,
	// eggshells/masks lying in the gardens; held items live elsewhere).
	inline constexpr std::size_t ChaoSaveItemsOffset = 0xC8;
	inline constexpr std::size_t ChaoSaveItemSlotSize = 0x14;
	inline constexpr std::size_t ChaoSaveChecksumSize = 8;
	inline constexpr std::size_t ChaoSaveBlockSize = 0xCA6C;
	inline constexpr std::size_t ChaoSaveFileBlockOffset = 0x3040;
	inline constexpr std::size_t ChaoTypeOffset = 0x80;
	inline constexpr std::size_t ChaoGardenOffset = 0x81;
	inline constexpr std::uint8_t EmptyChaoType = 0;
	inline constexpr std::uint8_t NeutralGardenId = 1;

	using ChaoSaveBlock = std::vector<std::uint8_t>;
	using ChaoSaveRecordTable = std::vector<std::uint8_t>;

	ChaoSaveBlock CaptureChaoSaveBlockFromFileBuffer(
		const void* fileBuffer,
		std::size_t fileSize);
	ChaoSaveRecordTable CaptureChaoSaveRecordTable(const ChaoSaveBlock& chaoSaveBlock);
	void ReplaceChaoSaveRecordTable(
		ChaoSaveBlock& chaoSaveBlock,
		const ChaoSaveRecordTable& records);
	void ReplaceChaoSaveBlockInFileBuffer(
		void* fileBuffer,
		std::size_t fileSize,
		const ChaoSaveBlock& chaoSaveBlock);
	class ChaoSaveWriteBufferGuard
	{
	public:
		ChaoSaveWriteBufferGuard(
			void* fileBuffer,
			std::size_t fileSize,
			const ChaoSaveBlock& replacementBlock);
		~ChaoSaveWriteBufferGuard();

		ChaoSaveWriteBufferGuard(const ChaoSaveWriteBufferGuard&) = delete;
		ChaoSaveWriteBufferGuard& operator=(const ChaoSaveWriteBufferGuard&) = delete;

		const ChaoSaveBlock& CapturedBlock() const noexcept;
		void Restore() noexcept;
		bool IsActive() const noexcept;

	private:
		void* fileBuffer_ = nullptr;
		ChaoSaveBlock capturedBlock_;
		bool active_ = false;
	};
	GardenPool CaptureGardenPool(
		const ChaoSaveRecordTable& records,
		std::uint8_t gardenId);
	ChaoSaveRecordTable OverlayPoolInNeutralGarden(
		const ChaoSaveRecordTable& records,
		const GardenPool& addedPool);
	// An item region with every slot cleared (type -1, area -1, zeros), the
	// state of a garden with nothing lying on the ground.
	ItemRegion EmptyGardenItemRegion();
	// Garden id stored in each occupied item slot's second word.
	inline constexpr std::uint16_t NeutralGardenItemArea = 1;
	// Sentinel for "the player is not holding an item inside the region".
	inline constexpr std::size_t NoHeldItemSlot = static_cast<std::size_t>(-1);
	bool ItemSlotOccupied(const ItemRegion& region, std::size_t slotOffset);
	// Region containing only the occupied slots tagged with gardenId (held
	// slot excluded), each kept at its original offset.
	ItemRegion ExtractGardenItems(
		const ItemRegion& region,
		std::uint16_t gardenId,
		std::size_t heldSlotOffset);
	// Copy of live with gardenId slots replaced by gardenItems. The held slot
	// is never cleared or overwritten; items that no longer fit are dropped.
	ItemRegion MergeGardenItems(
		const ItemRegion& live,
		const ItemRegion& gardenItems,
		std::uint16_t gardenId,
		std::size_t heldSlotOffset);

	class ChaoPoolOverlaySession
	{
	public:
		ChaoSaveRecordTable Begin(
			const ChaoSaveRecordTable& vanillaRecords,
			const GardenPool& addedPool);
		GardenPool End(
			const ChaoSaveRecordTable& liveRecords,
			ChaoSaveRecordTable& restoredVanillaRecords);
		bool IsActive() const noexcept;

	private:
		ChaoSaveRecordTable VanillaRecords;
		bool Active = false;
	};

	class AddedGardenPoolOverlaySession
	{
	public:
		explicit AddedGardenPoolOverlaySession(std::filesystem::path sidecarPath);

		void Load();
		ChaoSaveRecordTable Begin(
			AddedGarden garden,
			const ChaoSaveRecordTable& vanillaRecords);
		// Items follow the same session, slot-merged rather than wholesale:
		// BeginItems hides Neutral's garden-tagged ground items and lays out
		// the garden's own (empty on first visit), leaving every other slot -
		// lobby items, other areas, and the held slot - untouched, so carried
		// items survive. CheckpointItems/EndItems capture the live garden-
		// tagged slots back (held slot excluded, so a carried-out item leaves
		// with the player); EndItems returns the region to restore. BeginItems
		// must follow Begin, and EndItems must precede End (End commits).
		ItemRegion BeginItems(
			const ItemRegion& vanillaItems,
			std::size_t heldSlotOffset);
		void CheckpointItems(
			const ItemRegion& liveItems,
			std::size_t heldSlotOffset);
		ItemRegion EndItems(
			const ItemRegion& liveItems,
			std::size_t heldSlotOffset);
		GardenPool Checkpoint(const ChaoSaveRecordTable& liveRecords);
		GardenPool End(
			const ChaoSaveRecordTable& liveRecords,
			ChaoSaveRecordTable& restoredVanillaRecords);
		void Cancel(const ChaoSaveRecordTable& restoredVanillaRecords);
		bool IsActive() const noexcept;

	private:
		AddedGardenPoolTransaction PoolTransaction;
		ChaoPoolOverlaySession OverlaySession;
		std::optional<AddedGarden> ActiveGarden;
		ItemRegion VanillaGardenItems;
		bool ItemsActive = false;
		bool Loaded = false;
	};
}

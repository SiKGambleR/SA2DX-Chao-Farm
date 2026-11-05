#include "ChaoPoolOverlay.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
	constexpr std::size_t ChaoSaveRecordsSize =
		garden_pools::ChaoSaveRecordSize * garden_pools::ChaoSaveRecordCount;
	constexpr std::size_t ChaoSaveFileBlockEnd =
		garden_pools::ChaoSaveFileBlockOffset + garden_pools::ChaoSaveBlockSize;

	void ValidateRecordTable(const garden_pools::ChaoSaveRecordTable& records)
	{
		if (records.size() != ChaoSaveRecordsSize)
		{
			throw std::invalid_argument("The Chao save record table must contain exactly 24 records.");
		}
	}

	void ValidateFileBuffer(const void* fileBuffer, std::size_t fileSize)
	{
		if (fileBuffer == nullptr)
		{
			throw std::invalid_argument("The Chao save file buffer cannot be null.");
		}
		if (fileSize < ChaoSaveFileBlockEnd)
		{
			throw std::invalid_argument("The Chao save file buffer does not contain a complete Chao save block.");
		}
	}

	void ValidateGardenId(std::uint8_t gardenId)
	{
		if (gardenId < 1 || gardenId > 3)
		{
			throw std::invalid_argument("The Chao garden ID must be between 1 and 3.");
		}
	}

	std::size_t RecordOffset(std::size_t index)
	{
		return index * garden_pools::ChaoSaveRecordSize;
	}

	bool IsEmpty(const garden_pools::ChaoSaveRecordTable& records, std::size_t index)
	{
		return records[RecordOffset(index) + garden_pools::ChaoTypeOffset]
			== garden_pools::EmptyChaoType;
	}
}

namespace garden_pools
{
	ChaoSaveBlock CaptureChaoSaveBlockFromFileBuffer(
		const void* fileBuffer,
		std::size_t fileSize)
	{
		ValidateFileBuffer(fileBuffer, fileSize);
		const auto* const bytes = static_cast<const std::uint8_t*>(fileBuffer);
		return ChaoSaveBlock(
			bytes + ChaoSaveFileBlockOffset,
			bytes + ChaoSaveFileBlockEnd);
	}


	ChaoSaveRecordTable CaptureChaoSaveRecordTable(const ChaoSaveBlock& chaoSaveBlock)
	{
		if (chaoSaveBlock.size() != ChaoSaveBlockSize)
		{
			throw std::invalid_argument("A Chao save block must contain exactly 0xCA6C bytes.");
		}

		return ChaoSaveRecordTable(
			chaoSaveBlock.begin() + static_cast<std::ptrdiff_t>(ChaoSaveRecordsOffset),
			chaoSaveBlock.begin() + static_cast<std::ptrdiff_t>(
				ChaoSaveRecordsOffset + ChaoSaveRecordSize * ChaoSaveRecordCount));
	}

	void ReplaceChaoSaveRecordTable(
		ChaoSaveBlock& chaoSaveBlock,
		const ChaoSaveRecordTable& records)
	{
		if (chaoSaveBlock.size() != ChaoSaveBlockSize)
		{
			throw std::invalid_argument("A Chao save block must contain exactly 0xCA6C bytes.");
		}
		ValidateRecordTable(records);

		std::copy(
			records.begin(),
			records.end(),
			chaoSaveBlock.begin() + static_cast<std::ptrdiff_t>(ChaoSaveRecordsOffset));
	}

	void ReplaceChaoSaveBlockInFileBuffer(
		void* fileBuffer,
		std::size_t fileSize,
		const ChaoSaveBlock& chaoSaveBlock)
	{
		if (chaoSaveBlock.size() != ChaoSaveBlockSize)
		{
			throw std::invalid_argument("A Chao save block must contain exactly 0xCA6C bytes.");
		}
		ValidateFileBuffer(fileBuffer, fileSize);
		auto* const bytes = static_cast<std::uint8_t*>(fileBuffer);
		std::copy(
			chaoSaveBlock.begin(),
			chaoSaveBlock.end(),
			bytes + ChaoSaveFileBlockOffset);
	}

	ChaoSaveWriteBufferGuard::ChaoSaveWriteBufferGuard(
		void* fileBuffer,
		std::size_t fileSize,
		const ChaoSaveBlock& replacementBlock)
		: fileBuffer_(fileBuffer)
	{
		if (replacementBlock.size() != ChaoSaveBlockSize)
		{
			throw std::invalid_argument("A Chao save block must contain exactly 0xCA6C bytes.");
		}

		capturedBlock_ = CaptureChaoSaveBlockFromFileBuffer(fileBuffer, fileSize);
		ReplaceChaoSaveBlockInFileBuffer(fileBuffer, fileSize, replacementBlock);
		active_ = true;
	}

	ChaoSaveWriteBufferGuard::~ChaoSaveWriteBufferGuard()
	{
		Restore();
	}

	const ChaoSaveBlock& ChaoSaveWriteBufferGuard::CapturedBlock() const noexcept
	{
		return capturedBlock_;
	}

	void ChaoSaveWriteBufferGuard::Restore() noexcept
	{
		if (!active_)
		{
			return;
		}

		auto* const bytes = static_cast<std::uint8_t*>(fileBuffer_);
		std::copy(
			capturedBlock_.begin(),
			capturedBlock_.end(),
			bytes + ChaoSaveFileBlockOffset);
		active_ = false;
	}

	bool ChaoSaveWriteBufferGuard::IsActive() const noexcept
	{
		return active_;
	}

	GardenPool CaptureGardenPool(
		const ChaoSaveRecordTable& records,
		std::uint8_t gardenId)
	{
		ValidateRecordTable(records);
		ValidateGardenId(gardenId);

		GardenPool pool;
		for (std::size_t index = 0; index < ChaoSaveRecordCount; ++index)
		{
			const auto offset = RecordOffset(index);
			if (records[offset + ChaoTypeOffset] == EmptyChaoType
				|| records[offset + ChaoGardenOffset] != gardenId)
			{
				continue;
			}

			if (pool.size() == MaxChaoPerGarden)
			{
				throw std::runtime_error("The Chao save contains more than eight Chao in one garden.");
			}

			pool.emplace_back(
				records.begin() + static_cast<std::ptrdiff_t>(offset),
				records.begin() + static_cast<std::ptrdiff_t>(offset + ChaoSaveRecordSize));
		}
		return pool;
	}

	ChaoSaveRecordTable OverlayPoolInNeutralGarden(
		const ChaoSaveRecordTable& records,
		const GardenPool& addedPool)
	{
		ValidateRecordTable(records);
		if (addedPool.size() > MaxChaoPerGarden)
		{
			throw std::invalid_argument("An added garden cannot contain more than eight Chao.");
		}

		std::vector<std::size_t> neutralSlots;
		std::vector<std::size_t> replaceableSlots;
		for (std::size_t index = 0; index < ChaoSaveRecordCount; ++index)
		{
			const auto offset = RecordOffset(index);
			const auto type = records[offset + ChaoTypeOffset];
			if (type == EmptyChaoType)
			{
				replaceableSlots.push_back(index);
			}
			else if (records[offset + ChaoGardenOffset] == NeutralGardenId)
			{
				neutralSlots.push_back(index);
				replaceableSlots.push_back(index);
			}
		}

		if (addedPool.size() > replaceableSlots.size())
		{
			throw std::runtime_error("There are not enough free Chao save records for this garden pool.");
		}

		for (const auto& record : addedPool)
		{
			if (record.size() != ChaoSaveRecordSize
				|| record[ChaoTypeOffset] == EmptyChaoType)
			{
				throw std::invalid_argument("An added-garden record must be one initialized 0x800-byte Chao record.");
			}
		}

		auto overlay = records;
		for (const auto index : neutralSlots)
		{
			const auto offset = RecordOffset(index);
			std::fill_n(
				overlay.begin() + static_cast<std::ptrdiff_t>(offset),
				ChaoSaveRecordSize,
				std::uint8_t{});
		}

		for (std::size_t index = 0; index < addedPool.size(); ++index)
		{
			const auto slot = replaceableSlots[index];
			const auto offset = RecordOffset(slot);
			std::copy(
				addedPool[index].begin(),
				addedPool[index].end(),
				overlay.begin() + static_cast<std::ptrdiff_t>(offset));
			overlay[offset + ChaoGardenOffset] = NeutralGardenId;
		}

		return overlay;
	}

	ChaoSaveRecordTable ChaoPoolOverlaySession::Begin(
		const ChaoSaveRecordTable& vanillaRecords,
		const GardenPool& addedPool)
	{
		if (Active)
		{
			throw std::logic_error("A Chao pool overlay session is already active.");
		}

		auto overlay = OverlayPoolInNeutralGarden(vanillaRecords, addedPool);
		VanillaRecords = vanillaRecords;
		Active = true;
		return overlay;
	}

	GardenPool ChaoPoolOverlaySession::End(
		const ChaoSaveRecordTable& liveRecords,
		ChaoSaveRecordTable& restoredVanillaRecords)
	{
		if (!Active)
		{
			throw std::logic_error("There is no active Chao pool overlay session.");
		}

		auto addedPool = CaptureGardenPool(liveRecords, NeutralGardenId);
		restoredVanillaRecords = std::move(VanillaRecords);
		Active = false;
		return addedPool;
	}

	bool ChaoPoolOverlaySession::IsActive() const noexcept
	{
		return Active;
	}

	ItemRegion EmptyGardenItemRegion()
	{
		// Cleared slot layout (matches the game's own clearing code at
		// 0x52EAE9): int16 type = -1, int16 area = -1, two zero words, and
		// three zero position floats.
		ItemRegion region(GardenItemRegionSize, 0);
		for (std::size_t offset = 0; offset < region.size(); offset += ChaoSaveItemSlotSize)
		{
			region[offset] = 0xFF;
			region[offset + 1] = 0xFF;
			region[offset + 2] = 0xFF;
			region[offset + 3] = 0xFF;
		}
		return region;
	}

	namespace
	{
		void ValidateItemRegion(const ItemRegion& region)
		{
			if (region.size() != GardenItemRegionSize)
			{
				throw std::invalid_argument(
					"The garden item region has an unexpected size.");
			}
		}

		std::uint16_t ItemSlotArea(const ItemRegion& region, std::size_t offset)
		{
			return static_cast<std::uint16_t>(
				region[offset + 2] | (region[offset + 3] << 8));
		}

		void ClearItemSlot(ItemRegion& region, std::size_t offset)
		{
			std::fill(
				region.begin() + static_cast<std::ptrdiff_t>(offset),
				region.begin() + static_cast<std::ptrdiff_t>(offset + ChaoSaveItemSlotSize),
				static_cast<std::uint8_t>(0));
			region[offset] = 0xFF;
			region[offset + 1] = 0xFF;
			region[offset + 2] = 0xFF;
			region[offset + 3] = 0xFF;
		}

		void CopyItemSlot(
			ItemRegion& target,
			std::size_t targetOffset,
			const ItemRegion& source,
			std::size_t sourceOffset)
		{
			std::copy(
				source.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
				source.begin() + static_cast<std::ptrdiff_t>(sourceOffset + ChaoSaveItemSlotSize),
				target.begin() + static_cast<std::ptrdiff_t>(targetOffset));
		}
	}

	bool ItemSlotOccupied(const ItemRegion& region, std::size_t slotOffset)
	{
		const auto type = static_cast<std::int16_t>(
			region[slotOffset] | (region[slotOffset + 1] << 8));
		return type >= 0;
	}

	ItemRegion ExtractGardenItems(
		const ItemRegion& region,
		std::uint16_t gardenId,
		std::size_t heldSlotOffset)
	{
		ValidateItemRegion(region);
		auto extracted = EmptyGardenItemRegion();
		for (std::size_t offset = 0; offset < region.size(); offset += ChaoSaveItemSlotSize)
		{
			if (offset != heldSlotOffset
				&& ItemSlotOccupied(region, offset)
				&& ItemSlotArea(region, offset) == gardenId)
			{
				CopyItemSlot(extracted, offset, region, offset);
			}
		}
		return extracted;
	}

	ItemRegion MergeGardenItems(
		const ItemRegion& live,
		const ItemRegion& gardenItems,
		std::uint16_t gardenId,
		std::size_t heldSlotOffset)
	{
		ValidateItemRegion(live);
		ValidateItemRegion(gardenItems);
		auto merged = live;
		for (std::size_t offset = 0; offset < merged.size(); offset += ChaoSaveItemSlotSize)
		{
			if (offset != heldSlotOffset
				&& ItemSlotOccupied(merged, offset)
				&& ItemSlotArea(merged, offset) == gardenId)
			{
				ClearItemSlot(merged, offset);
			}
		}
		for (std::size_t offset = 0; offset < gardenItems.size(); offset += ChaoSaveItemSlotSize)
		{
			if (!ItemSlotOccupied(gardenItems, offset) || offset == heldSlotOffset)
			{
				continue;
			}
			auto target = offset;
			if (ItemSlotOccupied(merged, target) || target == heldSlotOffset)
			{
				target = merged.size();
				for (std::size_t free = 0; free < merged.size(); free += ChaoSaveItemSlotSize)
				{
					if (free != heldSlotOffset && !ItemSlotOccupied(merged, free))
					{
						target = free;
						break;
					}
				}
				if (target == merged.size())
				{
					continue; // no free slot: the item is dropped
				}
			}
			CopyItemSlot(merged, target, gardenItems, offset);
		}
		return merged;
	}

	AddedGardenPoolOverlaySession::AddedGardenPoolOverlaySession(
		std::filesystem::path sidecarPath)
		: PoolTransaction(std::move(sidecarPath))
	{
	}

	void AddedGardenPoolOverlaySession::Load()
	{
		if (Loaded)
		{
			throw std::logic_error(
				"The added-garden pool overlay session is already loaded.");
		}

		PoolTransaction.Load();
		Loaded = true;
	}

	ChaoSaveRecordTable AddedGardenPoolOverlaySession::Begin(
		AddedGarden garden,
		const ChaoSaveRecordTable& vanillaRecords)
	{
		if (!Loaded)
		{
			throw std::logic_error(
				"The added-garden pool overlay session is not loaded.");
		}
		if (ActiveGarden.has_value())
		{
			throw std::logic_error(
				"An added-garden pool overlay session is already active.");
		}

		const auto& addedPool = PoolFor(PoolTransaction.WorkingPools(), garden);
		auto overlay = OverlaySession.Begin(vanillaRecords, addedPool);
		ActiveGarden = garden;
		return overlay;
	}

	ItemRegion AddedGardenPoolOverlaySession::BeginItems(
		const ItemRegion& vanillaItems,
		std::size_t heldSlotOffset)
	{
		if (!Loaded || !ActiveGarden.has_value())
		{
			throw std::logic_error(
				"There is no active added-garden pool overlay session.");
		}
		if (ItemsActive)
		{
			throw std::logic_error(
				"The added-garden item overlay is already active.");
		}
		if (vanillaItems.size() != GardenItemRegionSize)
		{
			throw std::invalid_argument(
				"The live garden item region has an unexpected size.");
		}

		VanillaGardenItems = ExtractGardenItems(
			vanillaItems,
			NeutralGardenItemArea,
			heldSlotOffset);
		const auto& stored = ItemsFor(PoolTransaction.WorkingItems(), *ActiveGarden);
		const auto pool = stored.size() == GardenItemRegionSize
			? stored
			: EmptyGardenItemRegion();
		ItemsActive = true;
		return MergeGardenItems(
			vanillaItems,
			pool,
			NeutralGardenItemArea,
			heldSlotOffset);
	}

	void AddedGardenPoolOverlaySession::CheckpointItems(
		const ItemRegion& liveItems,
		std::size_t heldSlotOffset)
	{
		if (!Loaded || !ActiveGarden.has_value() || !ItemsActive)
		{
			throw std::logic_error(
				"There is no active added-garden item overlay.");
		}
		ItemsFor(PoolTransaction.WorkingItems(), *ActiveGarden) = ExtractGardenItems(
			liveItems,
			NeutralGardenItemArea,
			heldSlotOffset);
	}

	ItemRegion AddedGardenPoolOverlaySession::EndItems(
		const ItemRegion& liveItems,
		std::size_t heldSlotOffset)
	{
		CheckpointItems(liveItems, heldSlotOffset);
		ItemsActive = false;
		auto vanilla = std::move(VanillaGardenItems);
		VanillaGardenItems.clear();
		return MergeGardenItems(
			liveItems,
			vanilla,
			NeutralGardenItemArea,
			heldSlotOffset);
	}

	GardenPool AddedGardenPoolOverlaySession::Checkpoint(
		const ChaoSaveRecordTable& liveRecords)
	{
		if (!Loaded || !ActiveGarden.has_value())
		{
			throw std::logic_error(
				"There is no active added-garden pool overlay session.");
		}

		auto capturedPool = CaptureGardenPool(
			liveRecords,
			NeutralGardenId);
		PoolFor(PoolTransaction.WorkingPools(), *ActiveGarden) = capturedPool;
		PoolTransaction.Commit();
		return capturedPool;
	}

	GardenPool AddedGardenPoolOverlaySession::End(
		const ChaoSaveRecordTable& liveRecords,
		ChaoSaveRecordTable& restoredVanillaRecords)
	{
		if (!Loaded || !ActiveGarden.has_value())
		{
			throw std::logic_error(
				"There is no active added-garden pool overlay session.");
		}

		auto capturedPool = Checkpoint(liveRecords);

		auto restoredRecords = ChaoSaveRecordTable{};
		auto completedPool = OverlaySession.End(liveRecords, restoredRecords);
		if (completedPool != capturedPool)
		{
			throw std::logic_error(
				"The captured added-garden pool changed during restoration.");
		}

		restoredVanillaRecords = std::move(restoredRecords);
		ActiveGarden.reset();
		ItemsActive = false;
		VanillaGardenItems.clear();
		return completedPool;
	}

	void AddedGardenPoolOverlaySession::Cancel(
		const ChaoSaveRecordTable& restoredVanillaRecords)
	{
		if (!Loaded || !ActiveGarden.has_value())
		{
			throw std::logic_error(
				"There is no active added-garden pool overlay session.");
		}

		ChaoSaveRecordTable discardedPool;
		OverlaySession.End(restoredVanillaRecords, discardedPool);
		PoolTransaction.Rollback();
		ActiveGarden.reset();
		ItemsActive = false;
		VanillaGardenItems.clear();
	}

	bool AddedGardenPoolOverlaySession::IsActive() const noexcept
	{
		return ActiveGarden.has_value();
	}
}

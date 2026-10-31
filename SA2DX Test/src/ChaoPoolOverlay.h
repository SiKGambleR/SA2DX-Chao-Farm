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
		bool Loaded = false;
	};
}

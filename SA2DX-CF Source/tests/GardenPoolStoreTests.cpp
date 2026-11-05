#include "ChaoPoolOverlay.h"
#include "ChaoSaveChecksum.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <Windows.h>

namespace
{
	garden_pools::ChaoRecord MakeRecord(std::uint8_t seed, std::size_t size)
	{
		garden_pools::ChaoRecord record(size);
		for (std::size_t index = 0; index < record.size(); ++index)
		{
			record[index] = static_cast<std::uint8_t>(seed + index);
		}
		return record;
	}

	garden_pools::ChaoRecord MakeChaoRecord(
		std::uint8_t seed,
		std::uint8_t type,
		std::uint8_t garden)
	{
		auto record = MakeRecord(seed, garden_pools::ChaoSaveRecordSize);
		record[garden_pools::ChaoTypeOffset] = type;
		record[garden_pools::ChaoGardenOffset] = garden;
		return record;
	}

	garden_pools::ChaoSaveRecordTable MakeRecordTable()
	{
		garden_pools::ChaoSaveRecordTable records(
			garden_pools::ChaoSaveRecordCount * garden_pools::ChaoSaveRecordSize);
		for (std::size_t index = 0; index < garden_pools::ChaoSaveRecordCount; ++index)
		{
			const auto seed = static_cast<std::uint8_t>(index + 0x40);
			const auto offset = index * garden_pools::ChaoSaveRecordSize;
			std::fill_n(
				records.begin() + static_cast<std::ptrdiff_t>(offset),
				garden_pools::ChaoSaveRecordSize,
				seed);
			records[offset + garden_pools::ChaoTypeOffset] = garden_pools::EmptyChaoType;
			records[offset + garden_pools::ChaoGardenOffset] = 0;
		}
		return records;
	}

	void SetRecord(
		garden_pools::ChaoSaveRecordTable& records,
		std::size_t index,
		const garden_pools::ChaoRecord& record)
	{
		assert(record.size() == garden_pools::ChaoSaveRecordSize);
		const auto offset = index * garden_pools::ChaoSaveRecordSize;
		std::copy(record.begin(), record.end(), records.begin() + static_cast<std::ptrdiff_t>(offset));
	}

	garden_pools::ChaoRecord RecordAt(
		const garden_pools::ChaoSaveRecordTable& records,
		std::size_t index)
	{
		const auto offset = index * garden_pools::ChaoSaveRecordSize;
		return garden_pools::ChaoRecord(
			records.begin() + static_cast<std::ptrdiff_t>(offset),
			records.begin() + static_cast<std::ptrdiff_t>(
				offset + garden_pools::ChaoSaveRecordSize));
	}

	template <typename Action>
	void ExpectRuntimeError(Action action)
	{
		bool threw = false;
		try
		{
			action();
		}
		catch (const std::runtime_error&)
		{
			threw = true;
		}
		assert(threw);
	}

	template <typename Action>
	void ExpectInvalidArgumentAt(Action action, int sourceLine)
	{
		bool threw = false;
		try
		{
			action();
		}
		catch (const std::invalid_argument&)
		{
			threw = true;
		}
		if (!threw)
		{
			std::fprintf(stderr, "Expected invalid_argument at line %d did not throw.\n", sourceLine);
		}
		assert(threw);
	}

	template <typename Action>
	void ExpectLogicError(Action action)
	{
		bool threw = false;
		try
		{
			action();
		}
		catch (const std::logic_error&)
		{
			threw = true;
		}
		assert(threw);
	}

	int VerifyKnownChaoSaveFile(const std::filesystem::path& path)
	{
		const auto fileSize = std::filesystem::file_size(path);
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			throw std::runtime_error("Could not open the Chao save file.");
		}

		std::vector<std::uint8_t> file(static_cast<std::size_t>(fileSize));
		input.read(
			reinterpret_cast<char*>(file.data()),
			static_cast<std::streamsize>(file.size()));
		if (!input || input.peek() != std::char_traits<char>::eof())
		{
			throw std::runtime_error("Could not read the complete Chao save file.");
		}

		const auto block = garden_pools::CaptureChaoSaveBlockFromFileBuffer(
			file.data(),
			file.size());
		if (!garden_pools::HasValidChaoSaveChecksum(block))
		{
			throw std::runtime_error(
				"The Chao save block checksum does not match.");
		}

		auto modifiedCopy = block;
		modifiedCopy[garden_pools::ChaoSaveRecordsOffset] ^= 0x01;
		garden_pools::UpdateChaoSaveChecksum(modifiedCopy);
		if (!garden_pools::HasValidChaoSaveChecksum(modifiedCopy))
		{
			throw std::runtime_error(
				"The updated checksum did not validate on the modified copy.");
		}

		std::fprintf(
			stdout,
			"Verified the Chao checksum in %s and recalculated a modified in-memory copy; the file was not changed.\n",
			path.string().c_str());
		return 0;
	}
}

#define ExpectInvalidArgument(action) ExpectInvalidArgumentAt(action, __LINE__)

int main(int argc, char** argv)
{
	if (argc == 3 && std::string(argv[1]) == "--verify-chao-save")
	{
		try
		{
			return VerifyKnownChaoSaveFile(
				std::filesystem::u8path(argv[2]));
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "Chao save verification failed: %s\n", error.what());
			return 1;
		}
	}
	if (argc != 1)
	{
		std::fprintf(
			stderr,
			"Usage: GardenPoolStoreTests.exe [--verify-chao-save <path>]\n");
		return 2;
	}

	assert(std::string(garden_pools::SADXStageName(garden_pools::AddedGarden::StationSquare)) == "AL_GARDEN00");
	assert(std::string(garden_pools::SADXStageName(garden_pools::AddedGarden::MysticRuins)) == "AL_GARDEN01");
	assert(std::string(garden_pools::SADXStageName(garden_pools::AddedGarden::EggCarrier)) == "AL_GARDEN02");
	assert(garden_pools::SADXStageNumber(garden_pools::AddedGarden::StationSquare)
		== garden_pools::SADXChaoStageNumber::StationSquare);
	assert(garden_pools::SADXStageNumber(garden_pools::AddedGarden::EggCarrier)
		== garden_pools::SADXChaoStageNumber::EggCarrier);
	assert(garden_pools::SADXStageNumber(garden_pools::AddedGarden::MysticRuins)
		== garden_pools::SADXChaoStageNumber::MysticRuins);
	assert(garden_pools::AddedGardenForSADXStageNumber(4)
		== garden_pools::AddedGarden::StationSquare);
	assert(garden_pools::AddedGardenForSADXStageNumber(5)
		== garden_pools::AddedGarden::EggCarrier);
	assert(garden_pools::AddedGardenForSADXStageNumber(6)
		== garden_pools::AddedGarden::MysticRuins);
	assert(!garden_pools::AddedGardenForSADXStageNumber(3).has_value());
	assert(!garden_pools::AddedGardenForSADXStageNumber(7).has_value());
	assert(!garden_pools::AddedGardenForSADXStageNumber((std::numeric_limits<int>::min)()).has_value());
	assert(!garden_pools::AddedGardenForSADXStageNumber((std::numeric_limits<int>::max)()).has_value());
	ExpectInvalidArgument([&] {
		garden_pools::SADXStageNumber(static_cast<garden_pools::AddedGarden>(99));
	});

	const auto saveDirectory = std::filesystem::path(L"save");
	const auto alfSidecar = garden_pools::SidecarPathForSave(
		saveDirectory,
		"SONIC2B__ALF");
	const auto alternateChaoSaveSidecar = garden_pools::SidecarPathForSave(
		saveDirectory,
		".\\resource\\gd_PC\\SAVEDATA\\SONIC2B__ALT");
	assert(alfSidecar
		== saveDirectory / L"SA2SixGardens-SONIC2B__ALF.dat");
	assert(alternateChaoSaveSidecar
		== saveDirectory / L"SA2SixGardens-SONIC2B__ALT.dat");
	assert(alfSidecar != alternateChaoSaveSidecar);
	assert(garden_pools::ChaoSavePathMatches(
		"./resource/gd_PC/SAVEDATA/SONIC2B__ALF",
		"./resource/gd_PC/SAVEDATA/SONIC2B__ALF"));
	assert(garden_pools::ChaoSavePathMatches(
		"./resource/gd_PC/SAVEDATA/SONIC2B__ALF",
		".\\resource\\gd_PC\\SAVEDATA\\sonic2b__alf"));
	assert(!garden_pools::ChaoSavePathMatches(
		"./resource/gd_PC/SAVEDATA/SONIC2B__ALF",
		"./resource/gd_PC/SAVEDATA/SONIC2B__S01"));
	assert(!garden_pools::ChaoSavePathMatches(
		"./resource/gd_PC/SAVEDATA/SONIC2B__ALF",
		"./other/SAVEDATA/SONIC2B__ALF"));
	assert(!garden_pools::ChaoSavePathMatches(
		"SONIC2B__ALF",
		"./resource/gd_PC/SAVEDATA/SONIC2B__ALF.bak"));
	assert(!garden_pools::ChaoSavePathMatches(
		"SONIC2B__ALF",
		"./resource/gd_PC/SAVEDATA/.."));
	assert(!garden_pools::ChaoSavePathMatches(
		"SONIC2B__ALF",
		"./resource/gd_PC/SAVEDATA/"));
	assert(!garden_pools::ChaoSavePathMatches(".", "."));
	assert(!garden_pools::ChaoSavePathMatches("", "SONIC2B__ALF"));
	ExpectInvalidArgument([&] {
		garden_pools::SidecarPathForSave(saveDirectory, "");
	});
	ExpectInvalidArgument([&] {
		garden_pools::SidecarPathForSave(saveDirectory, "./SAVEDATA/..");
	});
	ExpectInvalidArgument([&] {
		garden_pools::SidecarPathForSave(saveDirectory, "./SAVEDATA/SAVE:ALT");
	});
	ExpectInvalidArgument([&] {
		garden_pools::SidecarPathForSave({}, "SONIC2B__ALF");
	});
	const char activeSaveName[] = "SONIC2B__ALT";
	assert(garden_pools::ChaoSaveFilenameFromBuffer(
		activeSaveName,
		sizeof(activeSaveName)) == "SONIC2B__ALT");
	ExpectInvalidArgument([&] {
		garden_pools::ChaoSaveFilenameFromBuffer(nullptr, 1);
	});
	ExpectInvalidArgument([&] {
		garden_pools::ChaoSaveFilenameFromBuffer("", 1);
	});
	const char unterminatedSaveName[] = { 'S', 'A', 'V', 'E' };
	ExpectInvalidArgument([&] {
		garden_pools::ChaoSaveFilenameFromBuffer(
			unterminatedSaveName,
			sizeof(unterminatedSaveName));
	});

	auto vanillaRecords = MakeRecordTable();
	SetRecord(vanillaRecords, 1, MakeChaoRecord(0x11, 2, 1));
	SetRecord(vanillaRecords, 3, MakeChaoRecord(0x22, 5, 2));
	SetRecord(vanillaRecords, 5, MakeChaoRecord(0x33, 7, 3));
	SetRecord(vanillaRecords, 8, MakeChaoRecord(0x44, 8, 1));
	SetRecord(vanillaRecords, 13, MakeChaoRecord(0x55, 9, 1));

	const auto neutralPool = garden_pools::CaptureGardenPool(vanillaRecords, 1);
	assert(neutralPool.size() == 3);
	assert(garden_pools::CaptureGardenPool(vanillaRecords, 2).size() == 1);
	assert(garden_pools::CaptureGardenPool(vanillaRecords, 3).size() == 1);
	ExpectInvalidArgument([&] {
		garden_pools::CaptureGardenPool(vanillaRecords, 0);
	});

	garden_pools::GardenPool addedPool{
		MakeChaoRecord(0x66, 10, 2),
		MakeChaoRecord(0x77, 11, 3),
		MakeChaoRecord(0x88, 12, 1),
		MakeChaoRecord(0x99, 13, 2)
	};
	auto normalizedAddedPool = addedPool;
	for (auto& record : normalizedAddedPool)
	{
		record[garden_pools::ChaoGardenOffset] = garden_pools::NeutralGardenId;
	}
	const auto overlay = garden_pools::OverlayPoolInNeutralGarden(vanillaRecords, addedPool);
	assert(overlay.size() == vanillaRecords.size());
	assert(RecordAt(overlay, 3) == RecordAt(vanillaRecords, 3));
	assert(RecordAt(overlay, 5) == RecordAt(vanillaRecords, 5));
	assert(RecordAt(overlay, 0) == normalizedAddedPool[0]);
	assert(RecordAt(overlay, 1) == normalizedAddedPool[1]);
	assert(RecordAt(overlay, 2) == normalizedAddedPool[2]);
	assert(RecordAt(overlay, 4) == normalizedAddedPool[3]);
	const auto overlaidPool = garden_pools::CaptureGardenPool(overlay, 1);
	assert(overlaidPool == normalizedAddedPool);
	for (const auto& record : overlaidPool)
	{
		assert(record[garden_pools::ChaoGardenOffset] == garden_pools::NeutralGardenId);
	}

	garden_pools::ChaoPoolOverlaySession overlaySession;
	auto liveOverlay = overlaySession.Begin(vanillaRecords, addedPool);
	assert(overlaySession.IsActive());
	ExpectLogicError([&] { overlaySession.Begin(vanillaRecords, addedPool); });
	liveOverlay[garden_pools::ChaoSaveRecordSize + 0x90] ^= 0x5A;
	auto restoredVanillaRecords = garden_pools::ChaoSaveRecordTable{};
	auto invalidLiveRecords = liveOverlay;
	for (std::size_t index = 6; index < 11; ++index)
	{
		SetRecord(
			invalidLiveRecords,
			index,
			MakeChaoRecord(static_cast<std::uint8_t>(0xB0 + index), 1, 1));
	}
	ExpectRuntimeError([&] {
		overlaySession.End(invalidLiveRecords, restoredVanillaRecords);
	});
	assert(overlaySession.IsActive());
	assert(restoredVanillaRecords.empty());
	const auto changedPool = overlaySession.End(liveOverlay, restoredVanillaRecords);
	assert(!overlaySession.IsActive());
	assert(restoredVanillaRecords == vanillaRecords);
	assert(changedPool.size() == addedPool.size());
	assert(changedPool[1][0x90]
		== static_cast<std::uint8_t>(normalizedAddedPool[1][0x90] ^ 0x5A));
	ExpectLogicError([&] {
		overlaySession.End(liveOverlay, restoredVanillaRecords);
	});

	const auto poolSessionPath = std::filesystem::temp_directory_path()
		/ (L"sa2-garden-pool-session-"
			+ std::to_wstring(GetCurrentProcessId())
			+ L".dat");
	auto independentPools = garden_pools::AddedGardenPools{};
	independentPools[static_cast<std::size_t>(
		garden_pools::AddedGarden::StationSquare)] = {
			MakeChaoRecord(0x91, 1, 1)
		};
	independentPools[static_cast<std::size_t>(
		garden_pools::AddedGarden::MysticRuins)] = {
			MakeChaoRecord(0xA1, 1, 1)
		};
	garden_pools::Save(poolSessionPath, independentPools);

	garden_pools::AddedGardenPoolOverlaySession poolSession(poolSessionPath);
	ExpectLogicError([&] {
		poolSession.Begin(
			garden_pools::AddedGarden::StationSquare,
			vanillaRecords);
	});
	poolSession.Load();
	auto stationSquareOverlay = poolSession.Begin(
		garden_pools::AddedGarden::StationSquare,
		vanillaRecords);
	ExpectLogicError([&] {
		poolSession.Begin(
			garden_pools::AddedGarden::MysticRuins,
			vanillaRecords);
	});
	stationSquareOverlay[0x90] ^= 0x23;
	const auto stationSquareLivePool = garden_pools::CaptureGardenPool(
		stationSquareOverlay,
		garden_pools::NeutralGardenId);
	assert(poolSession.Checkpoint(stationSquareOverlay)
		== stationSquareLivePool);
	assert(poolSession.IsActive());
	assert(garden_pools::Load(poolSessionPath)[static_cast<std::size_t>(
		garden_pools::AddedGarden::StationSquare)] == stationSquareLivePool);
	auto stationSquareRestored = garden_pools::ChaoSaveRecordTable{};
	assert(poolSession.End(stationSquareOverlay, stationSquareRestored)
		== stationSquareLivePool);
	assert(!poolSession.IsActive());
	assert(stationSquareRestored == vanillaRecords);
	auto persistedPools = garden_pools::Load(poolSessionPath);
	assert(persistedPools[static_cast<std::size_t>(
		garden_pools::AddedGarden::StationSquare)] == stationSquareLivePool);
	assert(persistedPools[static_cast<std::size_t>(
		garden_pools::AddedGarden::MysticRuins)]
		== independentPools[static_cast<std::size_t>(
			garden_pools::AddedGarden::MysticRuins)]);

	auto mysticRuinsOverlay = poolSession.Begin(
		garden_pools::AddedGarden::MysticRuins,
		vanillaRecords);
	mysticRuinsOverlay[garden_pools::ChaoSaveRecordSize + 0x90] ^= 0x41;
	const auto mysticRuinsLivePool = garden_pools::CaptureGardenPool(
		mysticRuinsOverlay,
		garden_pools::NeutralGardenId);
	auto mysticRuinsRestored = garden_pools::ChaoSaveRecordTable{};
	assert(poolSession.End(mysticRuinsOverlay, mysticRuinsRestored)
		== mysticRuinsLivePool);
	assert(mysticRuinsRestored == vanillaRecords);
	persistedPools = garden_pools::Load(poolSessionPath);
	assert(persistedPools[static_cast<std::size_t>(
		garden_pools::AddedGarden::StationSquare)] == stationSquareLivePool);
	assert(persistedPools[static_cast<std::size_t>(
		garden_pools::AddedGarden::MysticRuins)] == mysticRuinsLivePool);

	// Ground-item regions: slot-merged per garden. Neutral's garden-tagged
	// slots are hidden, everything else (lobby items, held slot) is preserved,
	// and each garden's own items persist independently.
	{
		const auto slotSize = garden_pools::ChaoSaveItemSlotSize;
		const auto setSlot = [&](
			garden_pools::ItemRegion& region,
			std::size_t slot,
			std::uint16_t type,
			std::uint16_t area,
			std::uint8_t tag)
		{
			const auto offset = slot * slotSize;
			region[offset] = static_cast<std::uint8_t>(type & 0xFF);
			region[offset + 1] = static_cast<std::uint8_t>(type >> 8);
			region[offset + 2] = static_cast<std::uint8_t>(area & 0xFF);
			region[offset + 3] = static_cast<std::uint8_t>(area >> 8);
			region[offset + 4] = tag;
		};
		const auto slotBytes = [&](const garden_pools::ItemRegion& region, std::size_t slot)
		{
			const auto offset = slot * slotSize;
			return garden_pools::ItemRegion(
				region.begin() + static_cast<std::ptrdiff_t>(offset),
				region.begin() + static_cast<std::ptrdiff_t>(offset + slotSize));
		};
		const auto emptyRegion = garden_pools::EmptyGardenItemRegion();
		assert(!garden_pools::ItemSlotOccupied(emptyRegion, 0 * slotSize));

		auto live = garden_pools::EmptyGardenItemRegion();
		setSlot(live, 0, 3, 1, 0xAA); // neutral ground item
		setSlot(live, 1, 5, 7, 0xBB); // lobby item (survives untouched)

		auto itemOverlayRecords = poolSession.Begin(
			garden_pools::AddedGarden::StationSquare,
			vanillaRecords);
		ExpectLogicError([&] {
			poolSession.CheckpointItems(emptyRegion, garden_pools::NoHeldItemSlot);
		});
		ExpectInvalidArgument([&] {
			poolSession.BeginItems(
				garden_pools::ItemRegion(4, 0),
				garden_pools::NoHeldItemSlot);
		});
		auto itemOverlay = poolSession.BeginItems(live, garden_pools::NoHeldItemSlot);
		assert(!garden_pools::ItemSlotOccupied(itemOverlay, 0 * slotSize)); // neutral item hidden
		assert(slotBytes(itemOverlay, 1) == slotBytes(live, 1)); // lobby item kept

		setSlot(itemOverlay, 2, 9, 1, 0xCC); // item left in the virtual garden
		auto restored = poolSession.EndItems(itemOverlay, garden_pools::NoHeldItemSlot);
		assert(slotBytes(restored, 0) == slotBytes(live, 0)); // neutral item back
		assert(slotBytes(restored, 1) == slotBytes(live, 1)); // lobby item kept
		assert(!garden_pools::ItemSlotOccupied(restored, 2 * slotSize)); // garden item gone
		auto itemRestoredRecords = garden_pools::ChaoSaveRecordTable{};
		poolSession.End(itemOverlayRecords, itemRestoredRecords);

		auto roundTripPools = garden_pools::AddedGardenPools{};
		auto roundTripItems = garden_pools::AddedGardenItemRegions{};
		garden_pools::LoadAll(poolSessionPath, roundTripPools, roundTripItems);
		const auto& storedItems = garden_pools::ItemsFor(
			roundTripItems,
			garden_pools::AddedGarden::StationSquare);
		assert(storedItems.size() == garden_pools::GardenItemRegionSize);
		assert(slotBytes(storedItems, 2) == slotBytes(itemOverlay, 2));
		assert(!garden_pools::ItemSlotOccupied(storedItems, 0 * slotSize));
		assert(!garden_pools::ItemSlotOccupied(storedItems, 1 * slotSize));
		assert(garden_pools::ItemsFor(
			roundTripItems,
			garden_pools::AddedGarden::MysticRuins).empty());

		// The stored item returns on the next visit, and carrying it out
		// (held slot) removes it from the garden and keeps the live bytes.
		auto secondVisitRecords = poolSession.Begin(
			garden_pools::AddedGarden::StationSquare,
			vanillaRecords);
		auto secondOverlay = poolSession.BeginItems(live, garden_pools::NoHeldItemSlot);
		assert(slotBytes(secondOverlay, 2) == slotBytes(itemOverlay, 2));
		const auto heldOffset = 2 * slotSize;
		auto heldRestored = poolSession.EndItems(secondOverlay, heldOffset);
		assert(slotBytes(heldRestored, 2) == slotBytes(secondOverlay, 2));
		auto secondRestoredRecords = garden_pools::ChaoSaveRecordTable{};
		poolSession.End(secondVisitRecords, secondRestoredRecords);
		garden_pools::LoadAll(poolSessionPath, roundTripPools, roundTripItems);
		assert(garden_pools::ItemsFor(
			roundTripItems,
			garden_pools::AddedGarden::StationSquare) == emptyRegion);

		// Pool-only encodes decode with empty item regions.
		auto legacyPools = garden_pools::AddedGardenPools{};
		auto legacyItems = garden_pools::AddedGardenItemRegions{};
		garden_pools::DecodeAll(
			garden_pools::Encode(independentPools),
			legacyPools,
			legacyItems);
		assert(legacyPools == independentPools);
		assert(legacyItems == garden_pools::AddedGardenItemRegions{});
	}

	const auto cancelPoolSessionPath = std::filesystem::temp_directory_path()
		/ (L"sa2-garden-pool-session-cancel-"
			+ std::to_wstring(GetCurrentProcessId())
			+ L".dat");
	garden_pools::Save(cancelPoolSessionPath, independentPools);
	garden_pools::AddedGardenPoolOverlaySession cancelPoolSession(
		cancelPoolSessionPath);
	cancelPoolSession.Load();
	const auto canceledOverlay = cancelPoolSession.Begin(
		garden_pools::AddedGarden::StationSquare,
		vanillaRecords);
	auto cancelLiveRecords = canceledOverlay;
	cancelLiveRecords[0x90] ^= 0x72;
	cancelPoolSession.Cancel(vanillaRecords);
	assert(!cancelPoolSession.IsActive());
	assert(garden_pools::Load(cancelPoolSessionPath) == independentPools);
	ExpectLogicError([&] {
		cancelPoolSession.End(cancelLiveRecords, restoredVanillaRecords);
	});

	const auto retryPoolSessionPath = std::filesystem::temp_directory_path()
		/ (L"sa2-garden-pool-session-retry-"
			+ std::to_wstring(GetCurrentProcessId())
			+ L".dat");
	garden_pools::AddedGardenPoolOverlaySession retryPoolSession(
		retryPoolSessionPath);
	assert(!std::filesystem::exists(retryPoolSessionPath));
	retryPoolSession.Load();
	auto retryOverlay = retryPoolSession.Begin(
		garden_pools::AddedGarden::EggCarrier,
		vanillaRecords);
	assert(garden_pools::CaptureGardenPool(
		retryOverlay,
		garden_pools::NeutralGardenId).empty());
	assert(!std::filesystem::exists(retryPoolSessionPath));
	auto retryFreeSlot = garden_pools::ChaoSaveRecordCount;
	for (std::size_t index = 0;
		index < garden_pools::ChaoSaveRecordCount;
		++index)
	{
		const auto offset = index * garden_pools::ChaoSaveRecordSize
			+ garden_pools::ChaoTypeOffset;
		if (retryOverlay[offset] == garden_pools::EmptyChaoType)
		{
			retryFreeSlot = index;
			break;
		}
	}
	assert(retryFreeSlot < garden_pools::ChaoSaveRecordCount);
	SetRecord(
		retryOverlay,
		retryFreeSlot,
		MakeChaoRecord(0xB1, 1, garden_pools::NeutralGardenId));
	const auto retryPool = garden_pools::CaptureGardenPool(
		retryOverlay,
		garden_pools::NeutralGardenId);
	std::filesystem::create_directory(retryPoolSessionPath);
	auto retryRestored = garden_pools::ChaoSaveRecordTable{};
	bool poolCommitFailed = false;
	try
	{
		retryPoolSession.Checkpoint(retryOverlay);
	}
	catch (const std::exception&)
	{
		poolCommitFailed = true;
	}
	assert(poolCommitFailed);
	assert(retryPoolSession.IsActive());
	assert(retryRestored.empty());
	std::filesystem::remove(retryPoolSessionPath);
	assert(retryPoolSession.Checkpoint(retryOverlay) == retryPool);
	assert(retryPoolSession.IsActive());
	assert(retryPoolSession.End(retryOverlay, retryRestored) == retryPool);
	assert(!retryPoolSession.IsActive());
	assert(retryRestored == vanillaRecords);
	assert(garden_pools::Load(retryPoolSessionPath)[static_cast<std::size_t>(
		garden_pools::AddedGarden::EggCarrier)] == retryPool);

	std::vector<std::uint8_t> saveFile(0x10000);
	for (std::size_t index = 0; index < saveFile.size(); ++index)
	{
		saveFile[index] = static_cast<std::uint8_t>(index * 17);
	}
	const auto saveFileBeforePatch = saveFile;
	const auto savedNeutralBlock =
		garden_pools::CaptureChaoSaveBlockFromFileBuffer(
			saveFile.data(),
			saveFile.size());
	assert(savedNeutralBlock.size() == garden_pools::ChaoSaveBlockSize);
	auto checksumBlock = garden_pools::ChaoSaveBlock(
		garden_pools::ChaoSaveBlockSize);
	for (std::size_t index = 0; index < checksumBlock.size(); ++index)
	{
		checksumBlock[index] = static_cast<std::uint8_t>(index * 17);
	}
	checksumBlock[0xCA65] = 0x5A;
	checksumBlock[0xCA68] = 0xA5;
	checksumBlock[0xCA6A] = 0xC3;
	checksumBlock[0xCA67] = 0x7E;
	assert(!garden_pools::HasValidChaoSaveChecksum(checksumBlock));
	garden_pools::UpdateChaoSaveChecksum(checksumBlock);
	assert(garden_pools::HasValidChaoSaveChecksum(checksumBlock));
	assert(checksumBlock[0xCA69] == 0xF9);
	assert(checksumBlock[0xCA64] == 0x83);
	assert(checksumBlock[0xCA6B] == 0x8A);
	assert(checksumBlock[0xCA66] == 0xBC);
	assert(checksumBlock[0xCA65] == 0x5A);
	assert(checksumBlock[0xCA68] == 0xA5);
	assert(checksumBlock[0xCA6A] == 0xC3);
	assert(checksumBlock[0xCA67] == 0x7E);
	checksumBlock[garden_pools::ChaoSaveRecordsOffset] ^= 0x01;
	assert(!garden_pools::HasValidChaoSaveChecksum(checksumBlock));
	garden_pools::UpdateChaoSaveChecksum(checksumBlock);
	assert(garden_pools::HasValidChaoSaveChecksum(checksumBlock));
	ExpectInvalidArgument([&] {
		garden_pools::HasValidChaoSaveChecksum(garden_pools::ChaoSaveBlock(4));
	});
	ExpectInvalidArgument([&] {
		auto invalidChecksumBlock = garden_pools::ChaoSaveBlock(4);
		garden_pools::UpdateChaoSaveChecksum(invalidChecksumBlock);
	});

	auto recordBlock = savedNeutralBlock;
	auto blockRecords =
		garden_pools::CaptureChaoSaveRecordTable(recordBlock);
	assert(blockRecords.size()
		== garden_pools::ChaoSaveRecordCount * garden_pools::ChaoSaveRecordSize);
	const auto blockBeforeRecordReplace = recordBlock;
	const auto replacementRecords = MakeRecordTable();
	garden_pools::ReplaceChaoSaveRecordTable(recordBlock, replacementRecords);
	assert(garden_pools::CaptureChaoSaveRecordTable(recordBlock) == replacementRecords);
	assert(std::equal(
		blockBeforeRecordReplace.begin(),
		blockBeforeRecordReplace.begin()
			+ static_cast<std::ptrdiff_t>(garden_pools::ChaoSaveRecordsOffset),
		recordBlock.begin()));
	assert(std::equal(
		blockBeforeRecordReplace.begin()
			+ static_cast<std::ptrdiff_t>(
				garden_pools::ChaoSaveRecordsOffset
				+ garden_pools::ChaoSaveRecordCount * garden_pools::ChaoSaveRecordSize),
		blockBeforeRecordReplace.end(),
		recordBlock.begin()
			+ static_cast<std::ptrdiff_t>(
				garden_pools::ChaoSaveRecordsOffset
				+ garden_pools::ChaoSaveRecordCount * garden_pools::ChaoSaveRecordSize)));
	ExpectInvalidArgument([&] {
		garden_pools::CaptureChaoSaveRecordTable(garden_pools::ChaoSaveBlock(4));
	});
	ExpectInvalidArgument([&] {
		auto invalidRecords = replacementRecords;
		invalidRecords.pop_back();
		garden_pools::ReplaceChaoSaveRecordTable(recordBlock, invalidRecords);
	});

	auto originalOverlayBlock = savedNeutralBlock;
	std::fill(
		originalOverlayBlock.begin(),
		originalOverlayBlock.end(),
		std::uint8_t{ 0xA5 });
	garden_pools::ReplaceChaoSaveBlockInFileBuffer(
		saveFile.data(),
		saveFile.size(),
		originalOverlayBlock);
	assert(std::equal(
		originalOverlayBlock.begin(),
		originalOverlayBlock.end(),
		saveFile.begin()
			+ static_cast<std::ptrdiff_t>(garden_pools::ChaoSaveFileBlockOffset)));
	assert(std::equal(
		saveFileBeforePatch.begin(),
		saveFileBeforePatch.begin()
			+ static_cast<std::ptrdiff_t>(garden_pools::ChaoSaveFileBlockOffset),
		saveFile.begin()));
	assert(std::equal(
		saveFileBeforePatch.begin()
			+ static_cast<std::ptrdiff_t>(
				garden_pools::ChaoSaveFileBlockOffset
				+ garden_pools::ChaoSaveBlockSize),
		saveFileBeforePatch.end(),
		saveFile.begin()
			+ static_cast<std::ptrdiff_t>(
				garden_pools::ChaoSaveFileBlockOffset
				+ garden_pools::ChaoSaveBlockSize)));
	assert(garden_pools::CaptureChaoSaveBlockFromFileBuffer(
		saveFile.data(),
		saveFile.size()) == originalOverlayBlock);

	auto liveGardenFile = saveFileBeforePatch;
	{
		garden_pools::ChaoSaveWriteBufferGuard saveGuard(
			liveGardenFile.data(),
			liveGardenFile.size(),
			savedNeutralBlock);
		assert(saveGuard.IsActive());
		assert(saveGuard.CapturedBlock() == savedNeutralBlock);
		assert(garden_pools::CaptureChaoSaveBlockFromFileBuffer(
			liveGardenFile.data(),
			liveGardenFile.size()) == savedNeutralBlock);
		saveGuard.Restore();
		assert(!saveGuard.IsActive());
		assert(liveGardenFile == saveFileBeforePatch);
		saveGuard.Restore();
		assert(liveGardenFile == saveFileBeforePatch);
	}

	auto exceptionFile = saveFileBeforePatch;
	try
	{
		garden_pools::ChaoSaveWriteBufferGuard saveGuard(
			exceptionFile.data(),
			exceptionFile.size(),
			savedNeutralBlock);
		assert(garden_pools::CaptureChaoSaveBlockFromFileBuffer(
			exceptionFile.data(),
			exceptionFile.size()) == savedNeutralBlock);
		std::fill(
			exceptionFile.begin()
				+ static_cast<std::ptrdiff_t>(
					garden_pools::ChaoSaveFileBlockOffset),
			exceptionFile.begin()
				+ static_cast<std::ptrdiff_t>(
					garden_pools::ChaoSaveFileBlockOffset
					+ garden_pools::ChaoSaveBlockSize),
			std::uint8_t{ 0xD4 });
		throw std::runtime_error("Simulated writer exception.");
	}
	catch (const std::runtime_error&)
	{
	}
	assert(exceptionFile == saveFileBeforePatch);

	const auto saveBlockEnd =
		garden_pools::ChaoSaveFileBlockOffset
		+ garden_pools::ChaoSaveBlockSize;
	const auto saveFileBeforeInvalidGuard = saveFile;
	ExpectInvalidArgument([&] {
		garden_pools::ChaoSaveWriteBufferGuard saveGuard(
			nullptr,
			saveFile.size(),
			savedNeutralBlock);
	});
	auto shortGuardFile = saveFileBeforePatch;
	ExpectInvalidArgument([&] {
		garden_pools::ChaoSaveWriteBufferGuard saveGuard(
			shortGuardFile.data(),
			saveBlockEnd - 1,
			savedNeutralBlock);
	});
	assert(shortGuardFile == saveFileBeforePatch);
	ExpectInvalidArgument([&] {
		garden_pools::ChaoSaveWriteBufferGuard saveGuard(
			saveFile.data(),
			saveFile.size(),
			garden_pools::ChaoSaveBlock(4));
	});
	assert(saveFile == saveFileBeforeInvalidGuard);

	garden_pools::ReplaceChaoSaveBlockInFileBuffer(
		saveFile.data(),
		saveFile.size(),
		savedNeutralBlock);
	assert(saveFile == saveFileBeforePatch);
	ExpectInvalidArgument([&] {
		garden_pools::CaptureChaoSaveBlockFromFileBuffer(nullptr, saveFile.size());
	});
	ExpectInvalidArgument([&] {
		garden_pools::CaptureChaoSaveBlockFromFileBuffer(
			saveFile.data(),
			garden_pools::ChaoSaveFileBlockOffset
				+ garden_pools::ChaoSaveBlockSize - 1);
	});
	ExpectInvalidArgument([&] {
		garden_pools::ReplaceChaoSaveBlockInFileBuffer(
			saveFile.data(),
			saveFile.size(),
			garden_pools::ChaoSaveBlock(4));
	});
	const auto saveFileBeforeInvalidPatch = saveFile;
	ExpectInvalidArgument([&] {
		garden_pools::ReplaceChaoSaveBlockInFileBuffer(
			saveFile.data(),
			garden_pools::ChaoSaveFileBlockOffset
				+ garden_pools::ChaoSaveBlockSize - 1,
			savedNeutralBlock);
	});
	assert(saveFile == saveFileBeforeInvalidPatch);

	ExpectInvalidArgument([&] {
		garden_pools::OverlayPoolInNeutralGarden(
			garden_pools::ChaoSaveRecordTable(32),
			addedPool);
	});
	ExpectInvalidArgument([&] {
		auto invalidPool = addedPool;
		while (invalidPool.size() <= garden_pools::MaxChaoPerGarden)
		{
			invalidPool.push_back(MakeChaoRecord(
				static_cast<std::uint8_t>(0xAA + invalidPool.size()),
				1,
				1));
		}
		garden_pools::OverlayPoolInNeutralGarden(vanillaRecords, invalidPool);
	});
	ExpectInvalidArgument([&] {
		auto invalidPool = addedPool;
		invalidPool[0].clear();
		garden_pools::OverlayPoolInNeutralGarden(vanillaRecords, invalidPool);
	});
	ExpectInvalidArgument([&] {
		auto invalidPool = addedPool;
		invalidPool[0][garden_pools::ChaoTypeOffset] = garden_pools::EmptyChaoType;
		garden_pools::OverlayPoolInNeutralGarden(vanillaRecords, invalidPool);
	});
	auto overfullGarden = MakeRecordTable();
	for (std::size_t index = 0; index < garden_pools::ChaoSaveRecordCount; ++index)
	{
		SetRecord(overfullGarden, index, MakeChaoRecord(
			static_cast<std::uint8_t>(index),
			2,
			index == 0 ? 1 : 2));
	}
	ExpectRuntimeError([&] {
		garden_pools::OverlayPoolInNeutralGarden(
			overfullGarden,
			garden_pools::GardenPool{
				MakeChaoRecord(0xAA, 3, 1),
				MakeChaoRecord(0xBB, 4, 1)
			});
	});

	garden_pools::AddedGardenPools gardens;
	garden_pools::PoolFor(gardens, garden_pools::AddedGarden::StationSquare)
		.push_back(MakeRecord(0x10, 4));
	assert(garden_pools::PoolFor(gardens, garden_pools::AddedGarden::StationSquare).size() == 1);
	assert(garden_pools::PoolFor(gardens, garden_pools::AddedGarden::MysticRuins).empty());
	assert(garden_pools::PoolFor(gardens, garden_pools::AddedGarden::EggCarrier).empty());
	ExpectInvalidArgument([&] {
		garden_pools::SADXStageName(static_cast<garden_pools::AddedGarden>(99));
	});
	ExpectInvalidArgument([&] {
		garden_pools::PoolFor(gardens, static_cast<garden_pools::AddedGarden>(99));
	});

	for (std::size_t gardenIndex = 0;
		gardenIndex < garden_pools::AddedGardens.size();
		++gardenIndex)
	{
		auto& garden = garden_pools::PoolFor(gardens, garden_pools::AddedGardens[gardenIndex]);
		for (std::size_t chao = garden.size();
			chao < garden_pools::MaxChaoPerGarden;
			++chao)
		{
			garden.push_back(MakeRecord(
				static_cast<std::uint8_t>(gardenIndex * 31 + chao),
				64 + gardenIndex + chao));
		}
	}
	garden_pools::PoolFor(gardens, garden_pools::AddedGarden::EggCarrier)[0]
		= MakeRecord(0x21, garden_pools::ChaoSaveRecordSize);

	const auto mysticRuinsBeforeStationSquareEdit =
		garden_pools::PoolFor(gardens, garden_pools::AddedGarden::MysticRuins);
	const auto eggCarrierBeforeStationSquareEdit =
		garden_pools::PoolFor(gardens, garden_pools::AddedGarden::EggCarrier);
	garden_pools::PoolFor(gardens, garden_pools::AddedGarden::StationSquare)[0][0] ^= 0xFF;
	assert(garden_pools::PoolFor(gardens, garden_pools::AddedGarden::MysticRuins)
		== mysticRuinsBeforeStationSquareEdit);
	assert(garden_pools::PoolFor(gardens, garden_pools::AddedGarden::EggCarrier)
		== eggCarrierBeforeStationSquareEdit);

	const auto encoded = garden_pools::Encode(gardens);
	const auto decoded = garden_pools::Decode(encoded);
	assert(decoded == gardens);
	for (const auto garden : garden_pools::AddedGardens)
	{
		assert(garden_pools::PoolFor(decoded, garden) == garden_pools::PoolFor(gardens, garden));
	}

	auto corrupted = encoded;
	corrupted.back() ^= 0x80;
	ExpectRuntimeError([&] { garden_pools::Decode(corrupted); });
	ExpectRuntimeError([&] {
		garden_pools::Decode(std::vector<std::uint8_t>(encoded.begin(), encoded.end() - 1));
	});

	auto tooMany = gardens;
	tooMany[0].push_back(MakeRecord(0, 1));
	ExpectInvalidArgument([&] { garden_pools::Encode(tooMany); });

	auto oversized = gardens;
	oversized[0][0] = MakeRecord(0, garden_pools::MaxRecordBytes + 1);
	ExpectInvalidArgument([&] { garden_pools::Encode(oversized); });

	auto emptyRecord = gardens;
	emptyRecord[0][0].clear();
	ExpectInvalidArgument([&] { garden_pools::Encode(emptyRecord); });

	wchar_t tempDirectory[MAX_PATH]{};
	const auto tempDirectoryLength = GetTempPathW(MAX_PATH, tempDirectory);
	assert(tempDirectoryLength > 0 && tempDirectoryLength < MAX_PATH);
	const auto path = std::filesystem::path(tempDirectory)
		/ (L"sa2-garden-pool-test-" + std::to_wstring(GetCurrentProcessId()) + L".dat");

	garden_pools::Save(path, gardens);
	assert(garden_pools::Load(path) == gardens);
	garden_pools::Save(path, {});
	assert(garden_pools::Load(path) == garden_pools::AddedGardenPools{});

	const auto transactionPath = path.wstring() + L".transaction";
	garden_pools::AddedGardenPoolTransaction transaction(transactionPath);
	ExpectLogicError([&] { transaction.WorkingPools(); });
	transaction.Load();
	assert(!transaction.IsDirty());
	transaction.Commit();
	assert(!std::filesystem::exists(transactionPath));

	auto& workingStationSquare = garden_pools::PoolFor(
		transaction.WorkingPools(),
		garden_pools::AddedGarden::StationSquare);
	workingStationSquare.push_back(MakeRecord(0x71, 16));
	assert(transaction.IsDirty());
	transaction.Commit();
	assert(!transaction.IsDirty());
	assert(garden_pools::Load(transactionPath)
		== transaction.WorkingPools());

	garden_pools::PoolFor(
		transaction.WorkingPools(),
		garden_pools::AddedGarden::MysticRuins)
		.push_back(MakeRecord(0x72, 16));
	assert(transaction.IsDirty());
	transaction.Rollback();
	assert(!transaction.IsDirty());
	assert(garden_pools::PoolFor(
		transaction.WorkingPools(),
		garden_pools::AddedGarden::MysticRuins).empty());
	assert(garden_pools::PoolFor(
		transaction.WorkingPools(),
		garden_pools::AddedGarden::StationSquare).size() == 1);

	const auto failedCommitPath = path.wstring() + L".failed-commit";
	garden_pools::AddedGardenPoolTransaction failedCommit(failedCommitPath);
	failedCommit.Load();
	garden_pools::PoolFor(
		failedCommit.WorkingPools(),
		garden_pools::AddedGarden::EggCarrier)
		.push_back(MakeRecord(0x73, 16));
	std::filesystem::create_directory(failedCommitPath);
	bool commitFailed = false;
	try
	{
		failedCommit.Commit();
	}
	catch (const std::exception&)
	{
		commitFailed = true;
	}
	assert(commitFailed);
	assert(failedCommit.IsDirty());
	failedCommit.Rollback();
	assert(!failedCommit.IsDirty());
	assert(garden_pools::PoolFor(
		failedCommit.WorkingPools(),
		garden_pools::AddedGarden::EggCarrier).empty());

	std::filesystem::remove_all(failedCommitPath);
	std::filesystem::remove(retryPoolSessionPath);
	std::filesystem::remove(cancelPoolSessionPath);
	std::filesystem::remove(poolSessionPath);
	std::filesystem::remove(transactionPath);
	std::filesystem::remove(path);
}

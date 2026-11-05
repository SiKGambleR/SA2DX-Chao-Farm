#include "ChaoPoolOverlay.h"
#include "GardenGroundGrids.h"
#include "Sa2LvlLoader.h"
#include "StageSelectSites.h"

#include <SA2ModLoader.h>

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
	constexpr std::uintptr_t CurrentChaoSaveFilenameAddress = 0x136604C;
	constexpr std::uintptr_t ChaoMainConstructorPatchSiteAddress = 0x52B370;
	constexpr std::uintptr_t NeutralGardenSpawnCallSiteAddress = 0x54C9C2;
	constexpr std::uintptr_t SpawnAllChaoInGardenAddress = 0x531B10;
	constexpr std::uintptr_t ChaoStageRequestAddress = 0x52B5B0;
	constexpr std::uintptr_t ChaoStageActivationCallSiteAddress = 0x52B3D2;
	constexpr std::uintptr_t ChaoStageActivationRoutineAddress = 0x52B030;
	constexpr std::array<std::uintptr_t, 5> PlayerGroundQueryCallSites{
		0x45FAB6,
		0x45FC86,
		0x45FCDB,
		0x45D819,
		0x45D94D
	};
	constexpr std::uintptr_t PlayerGroundQueryRoutineAddress = 0x45A870;
	constexpr std::uintptr_t LandManagerMainAddress = 0x47C180;
	constexpr std::uintptr_t ChaoMainObjectFunctionAddress = 0x54FE20;
	constexpr std::uintptr_t ChaoDeleteObjectFunctionAddress = 0x54FF30;
	constexpr std::uintptr_t ChaoSaveStartAddress = 0x19F6460;
	constexpr std::uintptr_t ChaoSlotsAddress = 0x19F6EC4;
	constexpr const wchar_t* TestPoolOverlayMarkerFilename =
		L"EnableVirtualGardenPoolTest.flag";
	constexpr const wchar_t* TestPoolSaveGuardMarkerFilename =
		L"DeferStationSquareOverlayRestoreForSaveTest.flag";
	constexpr const wchar_t* TestLobbyGateMarkerFilename =
		L"EnableVirtualGardenGateSpawnTest.flag";
	constexpr const wchar_t* MysticGroundProbeMarkerFilename =
		L"EnableMysticGroundProbeSpawnTest.flag";
	constexpr std::size_t ChaoSaveFilenameBufferScanLimit = 64;
	constexpr std::size_t MaxObjectListTraversal = 1024;
	constexpr std::array<std::uintptr_t, 3> ChaoSaveWriteCallSites{
		0x52E2AC,
		0x52FECB,
		0x532483
	};
	constexpr std::array<std::uintptr_t, 4> PreviousGardenStageRequestCallSites{
		0x5A6DA6,
		0x5A6E97,
		0x5A7342,
		0x5A741D
	};
	constexpr std::array<std::uintptr_t, 14> OtherGardenStageRequestCallSites{
		0x533287,
		0x533326,
		0x542EA6,
		0x54E53D,
		0x55AC07,
		0x55ACD7,
		0x57D9E3,
		0x57EF1C,
		0x57F79C,
		0x57F968,
		0x57FDB9,
		0x58191B,
		0x581937,
		0x581A01
	};
	constexpr std::array<std::uint8_t, 16> ReferencePatchSiteSignature{
		0x85, 0xFF, 0x0F, 0x85, 0xE2, 0x01, 0x00, 0x00,
		0x5B, 0x5F, 0x5E, 0xE9, 0x70, 0x5B, 0x00, 0x00
	};
	std::array<void*, ChaoSaveWriteCallSites.size()> PreviousChaoSaveWriteTargets{};
	std::array<void*, PreviousGardenStageRequestCallSites.size()>
		PreviousGardenStageRequestTargets{};
	std::array<void*, OtherGardenStageRequestCallSites.size()>
		OtherGardenStageRequestTargets{};
	void* PreviousNeutralGardenSpawnTarget = nullptr;
	void* PreviousChaoStageActivationTarget = nullptr;
	void* PreviousPlayerGroundQueryTarget = nullptr;
	std::filesystem::path ModDirectory;
	std::filesystem::path ChaoSaveDirectory;
	std::string LastCheckedChaoSaveFilename;
	std::string LastChaoSaveIdentityError;
	std::string TestPoolChaoSaveFilename;
	std::unique_ptr<garden_pools::AddedGardenPoolOverlaySession>
		TestPoolOverlaySession;
	garden_pools::AddedGarden SelectedAddedGarden =
		garden_pools::AddedGarden::StationSquare;
	garden_pools::AddedGarden ActiveTestGarden =
		garden_pools::AddedGarden::StationSquare;
	garden_pools::ChaoSaveRecordTable TestPoolVanillaRecords;
	garden_pools::ChaoSaveBlock TestPoolVanillaSaveBlock;
	garden_pools::ItemRegion TestPoolVanillaItems;
	garden_pools::ChaoSaveRecordTable TestPoolOverlayBeginRecords;
	bool HasLoggedPatchSite = false;
	bool ChaoSaveWriteHooksAttempted = false;
	bool ChaoSaveWriteHooksInstalled = false;
	bool ChaoSaveWriteHookDeferralLogged = false;
	bool NeutralGardenSpawnObserverAttempted = false;
	bool PreviousGardenStageRequestObserversAttempted = false;
	bool OtherGardenStageRequestObserversAttempted = false;
	bool ChaoStageActivationObserverAttempted = false;
	bool PlayerGroundQueryObserverAttempted = false;
	bool TestPoolOverlayProtectsSave = false;
	bool TestPoolRestorePending = false;
	bool TestPoolRestoreFailureLogged = false;
	bool TestPoolSidecarCommitted = false;
	std::unique_ptr<sa2lvl::LoadedLandTable> ActiveGardenLand;
	std::vector<std::unique_ptr<sa2lvl::LoadedLandTable>> RetiredGardenLand;
	LandTable SavedNeutralLand = {};
	int SavedLandTableSA2BModels = 0;
	bool NeutralLandSwapped = false;
	bool TestPoolCheckpointFailureLogged = false;
	bool TestLobbyGateSpawned = false;
	// Facing is the unit direction the gate looks toward (the lobby centre).
	struct LobbyGateSlot
	{
		float x, y, z;
		float FacingX, FacingZ;
	};
	// Gates sit in the corridor walls; the trigger lies 20 units behind each.
	const std::array<LobbyGateSlot, 3> LobbyGateSlots{{
		{ -211.0f, 0.03f, -59.5f, 0.0f, 1.0f },
		{ -288.0f, 0.03f, 0.0f, 1.0f, 0.0f },
		{ -211.0f, 0.03f, 59.5f, 0.0f, -1.0f } }};
	bool LobbyGateNearby = false;
	int LobbyReturnGateIndex = -1;
	int LobbyFramesSinceEntry = 0;
	bool LobbyReturnHandled = false;
	bool TestLobbyGateFailureLogged = false;
	bool MysticGroundProbeSpawnApplied = false;
	NJS_TEXNAME MysticTextureNames[48] = {};
	NJS_TEXLIST MysticTextureList = { MysticTextureNames, 45 };
	char MysticTextureName[16] = "AL_MR_TEX";

	// Per-garden settings for the converted SADX environments. Coordinates are
	// in the converted map's space (SADX units).
	struct GardenProfile
	{
		const char* Stage;
		const char* TextureArchive;
		int TextureCount;
		int PoolTexture;
		int WaterfallTexture;
		NJS_VECTOR Door;
		NJS_VECTOR TriggerCenter;
		NJS_VECTOR TriggerHalfSize;
		NJS_VECTOR Spawn;
		bool MysticFixes;
	};
	const GardenProfile GardenProfiles[] = {
		{ "AL_GARDEN00", "AL_SS_TEX", 41, 27, -1,
			{ -71.0f, 0.0f, 262.0f }, { -71.0f, -1.0f, 250.0f }, { 16.0f, 15.0f, 8.0f },
			{ -71.0f, 3.0f, 190.0f }, false },
		{ "AL_GARDEN01", "AL_MR_TEX", 45, 44, 17,
			{ 126.0f, 15.0f, -159.0f }, { 130.0f, 14.0f, -159.0f }, { 6.0f, 15.0f, 10.0f },
			{ -20.036854f, -5.676484f, -19.995054f }, true },
		{ "AL_GARDEN02", "AL_EC_TEX", 44, 43, -1,
			{ -288.0f, 4.5f, -99.0f }, { -288.0f, 3.5f, -99.0f }, { 8.0f, 15.0f, 16.0f },
			{ -258.0f, 7.0f, -99.0f }, false } };
	const GardenProfile* ActiveGardenProfile = nullptr;
	bool MysticGroundProbeMarkerFailureLogged = false;
	std::array<bool, garden_pools::AddedGardenCount> TestGardenSelectionKeyWasDown{};
	volatile LONG NeutralGardenSpawnObserverLogged = 0;
	volatile LONG OtherGardenStageRequestLogged = 0;
	unsigned int GardenEnvironmentDiagnosticFrame = 0;
	volatile LONG PlayerGroundQueryObservations = 0;
	std::array<volatile LONG, PlayerGroundQueryCallSites.size()>
		PlayerGroundQueryLastLoggedFrames{ { -1, -1, -1, -1, -1 } };
	std::array<volatile LONG, PlayerGroundQueryCallSites.size()>
		PlayerGroundQueryPreviousStates{ { -1, -1, -1, -1, -1 } };
	std::array<volatile LONG, PreviousGardenStageRequestCallSites.size()>
		PreviousGardenStageRequestLogged{};
	bool HasPreviousChaoState = false;
	int PreviousSaveIndex = -1;
	int PreviousLastChaoArea = -1;
	int PreviousCurrentChaoArea = -1;
	int PreviousNextChaoArea = -1;
	bool HasPreviousChaoObjectCounts = false;
	bool PreviousChaoObjectScanComplete = false;
	std::size_t PreviousChaoMainObjectCount = 0;
	std::size_t PreviousChaoDeleteObjectCount = 0;

	static_assert(sizeof(ChaoData) == garden_pools::ChaoSaveRecordSize);
	static_assert(offsetof(ChaoDataBase, Type) == garden_pools::ChaoTypeOffset);
	static_assert(offsetof(ChaoDataBase, Garden) == garden_pools::ChaoGardenOffset);
	static_assert(garden_pools::ChaoSaveRecordsOffset
		+ garden_pools::ChaoSaveRecordCount * garden_pools::ChaoSaveRecordSize
		+ garden_pools::ChaoSaveChecksumSize
		== garden_pools::ChaoSaveBlockSize);
	static_assert(
		ChaoSlotsAddress - ChaoSaveStartAddress
		== garden_pools::ChaoSaveRecordsOffset);

	void Log(const std::string& message) noexcept;
	std::string FormatAddress(std::uintptr_t address);
	enum class TestPoolOverlayResult
	{
		NotRequested,
		Applied,
		Unsafe
	};
	struct ChaoObjectCounts
	{
		std::size_t MainSubCount;
		std::size_t DeleteSubCount;
		bool Complete;
	};
	ChaoObjectCounts CountChaoObjects();
	void LogLiveChaoObjectCountChanges();
	garden_pools::ChaoSaveBlock ReadChaoSaveBlockFromMemory();
	garden_pools::ChaoSaveRecordTable ReadChaoSaveRecordsFromMemory();
	void WriteChaoSaveRecordsToMemory(
		const garden_pools::ChaoSaveRecordTable& records);
	TestPoolOverlayResult TryBeginTestPoolOverlay();
	void TrySpawnTestLobbyGate();
	void UpdateLobbyGateSelection();
	void ApplyLobbyReturnPosition();
	bool TestLobbyGateRequested();
	bool TestPoolOverlayRequested();
	void TryRestorePendingTestPoolOverlay();
	void TryRestoreTestPoolOverlayAfterReturn(
		const ChaoObjectCounts& chaoObjectsAfter);
	void CompleteTestPoolOverlay();
	void LogGardenLandManagerState(const char* phase);
	void ApplyGardenEnvironment(const char* stageName);
	void RestoreGardenEnvironment();
	void LogGardenEnvironmentRuntimeState();
	void LogGardenObjectSurvey();
	constexpr std::uint32_t GateOriginalTexlist = 0x1236160;

	void ApplyGardenObjectFixes(unsigned int frame);
	void KeepStationDecorOnGround(const GardenProfile& profile);
	void RemoveStockExitDoor();
	void PlaceGardenCameraBehind(const NJS_VECTOR& spawn);
	struct LobbyObjectInfo;
	void TrackGardenObjectChanges(unsigned int frame);
	void SetGateTexlist(std::uint32_t value);
	bool GardenExitDoorSpawned = false;
	int __cdecl SaveChaoFileHookWorker(
		std::size_t hookIndex,
		char* path,
		void* buffer,
		std::size_t count);
	int __cdecl SaveChaoFileHookWorker0(char* path, void* buffer, std::size_t count);
	int __cdecl SaveChaoFileHookWorker1(char* path, void* buffer, std::size_t count);
	int __cdecl SaveChaoFileHookWorker2(char* path, void* buffer, std::size_t count);
	void __cdecl SaveChaoFileHook0();
	void __cdecl SaveChaoFileHook1();
	void __cdecl SaveChaoFileHook2();
	int __cdecl NeutralGardenSpawnObserver();
	int __cdecl PreviousGardenStageRequestObserverWorker(
		std::size_t hookIndex,
		int destination);
	int __cdecl PreviousGardenStageRequestObserver0(int destination);
	int __cdecl PreviousGardenStageRequestObserver1(int destination);
	int __cdecl PreviousGardenStageRequestObserver2(int destination);
	int __cdecl PreviousGardenStageRequestObserver3(int destination);
	int __cdecl OtherGardenStageRequestObserver(int destination);
	void __cdecl ChaoStageActivationObserver();
	void TryInstallPlayerGroundQueryObserver();
	int __cdecl BeginPlayerGroundQueryObservation(
		std::uintptr_t eaxValue,
		std::uintptr_t argument1,
		std::uintptr_t argument2,
		std::uintptr_t argument3,
		std::uintptr_t callSite);
	void __cdecl EndPlayerGroundQueryObservation(
		int sample,
		std::uintptr_t argument1,
		std::uintptr_t argument2,
		std::uintptr_t argument3,
		int result);
	int __cdecl PlayerGroundQueryObserver();

	std::string ReadCurrentChaoSaveFilename()
	{
		const auto address = CurrentChaoSaveFilenameAddress;
		const auto* const buffer = reinterpret_cast<const char*>(address);
		MEMORY_BASIC_INFORMATION memoryInfo{};
		if (VirtualQuery(buffer, &memoryInfo, sizeof(memoryInfo)) != sizeof(memoryInfo)
			|| memoryInfo.State != MEM_COMMIT
			|| (memoryInfo.Protect & PAGE_GUARD) != 0)
		{
			throw std::runtime_error(
				"The game's current Chao save filename buffer is not readable.");
		}

		const DWORD pageProtection = memoryInfo.Protect & 0xFF;
		if (pageProtection != PAGE_READONLY
			&& pageProtection != PAGE_READWRITE
			&& pageProtection != PAGE_WRITECOPY
			&& pageProtection != PAGE_EXECUTE_READ
			&& pageProtection != PAGE_EXECUTE_READWRITE
			&& pageProtection != PAGE_EXECUTE_WRITECOPY)
		{
			throw std::runtime_error(
				"The game's current Chao save filename buffer is not readable.");
		}

		const auto regionStart =
			reinterpret_cast<std::uintptr_t>(memoryInfo.BaseAddress);
		const auto regionSize = static_cast<std::size_t>(memoryInfo.RegionSize);
		if (address < regionStart || address - regionStart >= regionSize)
		{
			throw std::runtime_error(
				"The game's current Chao save filename buffer is outside its memory region.");
		}

		const auto remainingRegionBytes =
			regionSize - static_cast<std::size_t>(address - regionStart);
		const auto scanSize = remainingRegionBytes < ChaoSaveFilenameBufferScanLimit
			? remainingRegionBytes
			: ChaoSaveFilenameBufferScanLimit;
		return garden_pools::ChaoSaveFilenameFromBuffer(
			buffer,
			scanSize);
	}

	garden_pools::ChaoSaveBlock ReadChaoSaveBlockFromMemory()
	{
		garden_pools::ChaoSaveBlock block(garden_pools::ChaoSaveBlockSize);
		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(
			GetCurrentProcess(),
			reinterpret_cast<const void*>(ChaoSaveStartAddress),
			block.data(),
			block.size(),
			&bytesRead)
			|| bytesRead != block.size())
		{
			throw std::runtime_error(
				"Could not read the live Chao save block for the test overlay.");
		}
		return block;
	}

	garden_pools::ChaoSaveRecordTable ReadChaoSaveRecordsFromMemory()
	{
		constexpr std::size_t recordTableSize =
			garden_pools::ChaoSaveRecordCount * garden_pools::ChaoSaveRecordSize;
		garden_pools::ChaoSaveRecordTable records(recordTableSize);
		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(
			GetCurrentProcess(),
			reinterpret_cast<const void*>(ChaoSlotsAddress),
			records.data(),
			records.size(),
			&bytesRead)
			|| bytesRead != records.size())
		{
			throw std::runtime_error(
				"Could not read the live Chao record table for the test overlay.");
		}
		return records;
	}

	void WriteChaoSaveRecordsToMemory(
		const garden_pools::ChaoSaveRecordTable& records)
	{
		constexpr std::size_t recordTableSize =
			garden_pools::ChaoSaveRecordCount * garden_pools::ChaoSaveRecordSize;
		if (records.size() != recordTableSize)
		{
			throw std::invalid_argument(
				"The live Chao record table must contain exactly 24 records.");
		}

		SIZE_T bytesWritten = 0;
		if (!WriteProcessMemory(
			GetCurrentProcess(),
			reinterpret_cast<void*>(ChaoSlotsAddress),
			records.data(),
			records.size(),
			&bytesWritten)
			|| bytesWritten != records.size())
		{
			throw std::runtime_error(
				"Could not write the live Chao record table for the test overlay.");
		}
	}

	garden_pools::ItemRegion ReadChaoSaveItemsFromMemory()
	{
		garden_pools::ItemRegion items(garden_pools::GardenItemRegionSize);
		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(
			GetCurrentProcess(),
			reinterpret_cast<const void*>(
				ChaoSaveStartAddress + garden_pools::ChaoSaveItemsOffset),
			items.data(),
			items.size(),
			&bytesRead)
			|| bytesRead != items.size())
		{
			throw std::runtime_error(
				"Could not read the live garden item region for the test overlay.");
		}
		return items;
	}

	void WriteChaoSaveItemsToMemory(const garden_pools::ItemRegion& items)
	{
		if (items.size() != garden_pools::GardenItemRegionSize)
		{
			throw std::invalid_argument(
				"The live garden item region has an unexpected size.");
		}

		SIZE_T bytesWritten = 0;
		if (!WriteProcessMemory(
			GetCurrentProcess(),
			reinterpret_cast<void*>(
				ChaoSaveStartAddress + garden_pools::ChaoSaveItemsOffset),
			items.data(),
			items.size(),
			&bytesWritten)
			|| bytesWritten != items.size())
		{
			throw std::runtime_error(
				"Could not write the live garden item region for the test overlay.");
		}
	}

	// The game tracks the item the player is carrying with a pointer to its
	// save slot (0x19F6454; the spawner at 0x52E920 skips that slot). When it
	// points into the swapped region, that slot is preserved verbatim and
	// excluded from garden captures, so carried items survive both directions.
	std::size_t HeldGardenItemSlotOffset()
	{
		const auto held = *reinterpret_cast<const std::uintptr_t*>(0x19F6454);
		const auto regionStart =
			ChaoSaveStartAddress + garden_pools::ChaoSaveItemsOffset;
		if (held < regionStart
			|| held >= regionStart + garden_pools::GardenItemRegionSize)
		{
			return garden_pools::NoHeldItemSlot;
		}
		return (held - regionStart)
			/ garden_pools::ChaoSaveItemSlotSize
			* garden_pools::ChaoSaveItemSlotSize;
	}

	// The game keeps a pointer to the record of the chao the player is
	// carrying at 0x1A5CA5C (SpawnAllChaoInGarden reads its garden byte
	// through it). When it points into the record table, that chao is in the
	// player's hands and must survive the overlay swaps.
	constexpr std::uintptr_t HeldChaoPointerAddress = 0x1A5CA5C;

	std::size_t HeldChaoRecordSlot()
	{
		const auto held = *reinterpret_cast<const std::uintptr_t*>(HeldChaoPointerAddress);
		constexpr auto tableSize =
			garden_pools::ChaoSaveRecordCount * garden_pools::ChaoSaveRecordSize;
		if (held < ChaoSlotsAddress || held >= ChaoSlotsAddress + tableSize)
		{
			return garden_pools::NoHeldItemSlot;
		}
		return (held - ChaoSlotsAddress) / garden_pools::ChaoSaveRecordSize;
	}

	// Copy of the live records whose held-chao slot is tagged with a garden id
	// the capture never matches, so a carried chao is not committed to the
	// garden's pool (it leaves with the player).
	garden_pools::ChaoSaveRecordTable ExcludeHeldChaoFromCapture(
		const garden_pools::ChaoSaveRecordTable& liveRecords,
		std::size_t heldChaoSlot)
	{
		auto adjusted = liveRecords;
		if (heldChaoSlot != garden_pools::NoHeldItemSlot
			&& (heldChaoSlot + 1) * garden_pools::ChaoSaveRecordSize <= adjusted.size())
		{
			adjusted[heldChaoSlot * garden_pools::ChaoSaveRecordSize
				+ garden_pools::ChaoGardenOffset] = 0x7F;
		}
		return adjusted;
	}

	TestPoolOverlayResult TryBeginTestPoolOverlay()
	{
		if (TestPoolOverlayProtectsSave
			|| (TestPoolOverlaySession != nullptr
				&& TestPoolOverlaySession->IsActive()))
		{
			return TestPoolOverlayResult::Applied;
		}
		if (ModDirectory.empty())
		{
			return TestPoolOverlayResult::NotRequested;
		}

		std::error_code error;
		const bool testOverlayRequested =
			true;
		if (error)
		{
			Log("Could not check the virtual-garden pool test marker: "
				+ error.message());
			return TestPoolOverlayResult::NotRequested;
		}
		if (!testOverlayRequested
			|| (TestLobbyGateRequested() && !LobbyGateNearby))
		{
			return TestPoolOverlayResult::NotRequested;
		}
		if (!ChaoSaveWriteHooksInstalled)
		{
			Log("The virtual-garden pool test was not started because the "
				"Chao save-write guard is unavailable.");
			return TestPoolOverlayResult::NotRequested;
		}
		if (CurrentChaoArea != garden_pools::NeutralGardenId)
		{
			Log("The virtual-garden pool test was not started because "
				"Neutral Garden is not the active stage.");
			return TestPoolOverlayResult::NotRequested;
		}
		const auto existingChaoObjects = CountChaoObjects();
		if (!existingChaoObjects.Complete
			|| existingChaoObjects.MainSubCount != 0
			|| existingChaoObjects.DeleteSubCount != 0)
		{
			Log("The virtual-garden pool test was not started because the "
				"pre-spawn Chao object scan was incomplete or nonempty.");
			return TestPoolOverlayResult::NotRequested;
		}

		try
		{
			const auto chaoSaveFilename = ReadCurrentChaoSaveFilename();
			const auto sidecarPath = garden_pools::SidecarPathForSave(
				ChaoSaveDirectory,
				chaoSaveFilename);

			const auto vanillaSaveBlock = ReadChaoSaveBlockFromMemory();
			const auto vanillaRecords = ReadChaoSaveRecordsFromMemory();
			if (garden_pools::CaptureChaoSaveRecordTable(vanillaSaveBlock)
				!= vanillaRecords)
			{
				throw std::runtime_error(
					"The live Chao record table does not match the live save block.");
			}

			TestPoolVanillaRecords = vanillaRecords;
			TestPoolVanillaSaveBlock = vanillaSaveBlock;
			TestPoolOverlaySession =
				std::make_unique<garden_pools::AddedGardenPoolOverlaySession>(
					sidecarPath);
			TestPoolOverlaySession->Load();
			auto overlay = TestPoolOverlaySession->Begin(
				SelectedAddedGarden,
				vanillaRecords);
			// A chao in the player's hands keeps its record through the swap:
			// its slot gets the vanilla bytes back, and any pool chao the
			// overlay placed there moves to a free slot.
			const auto heldChaoSlot = HeldChaoRecordSlot();
			if (heldChaoSlot != garden_pools::NoHeldItemSlot)
			{
				using namespace garden_pools;
				const auto offset = heldChaoSlot * ChaoSaveRecordSize;
				if (std::memcmp(overlay.data() + offset, vanillaRecords.data() + offset,
					ChaoSaveRecordSize) != 0)
				{
					std::vector<std::uint8_t> displaced(
						overlay.begin() + static_cast<std::ptrdiff_t>(offset),
						overlay.begin() + static_cast<std::ptrdiff_t>(offset + ChaoSaveRecordSize));
					std::memcpy(overlay.data() + offset,
						vanillaRecords.data() + offset, ChaoSaveRecordSize);
					if (displaced[ChaoTypeOffset] != EmptyChaoType)
					{
						bool placed = false;
						for (std::size_t free = 0; free < ChaoSaveRecordCount; ++free)
						{
							auto* candidate = overlay.data() + free * ChaoSaveRecordSize;
							if (free != heldChaoSlot
								&& candidate[ChaoTypeOffset] == EmptyChaoType)
							{
								std::memcpy(candidate, displaced.data(), ChaoSaveRecordSize);
								placed = true;
								break;
							}
						}
						Log(placed
							? "Protected the held chao's record; a garden chao moved slots."
							: "Protected the held chao's record; a garden chao had no "
							  "free slot and stays only in the sidecar.");
					}
					else
					{
						Log("Protected the held chao's record during the overlay.");
					}
				}
			}
			const auto vanillaItems = ReadChaoSaveItemsFromMemory();
			const auto overlayItems = TestPoolOverlaySession->BeginItems(
				vanillaItems,
				HeldGardenItemSlotOffset());
			TestPoolVanillaItems = vanillaItems;
			TestPoolOverlayBeginRecords = overlay;
			ActiveTestGarden = SelectedAddedGarden;
			TestPoolChaoSaveFilename = chaoSaveFilename;
			TestPoolSidecarCommitted = false;
			TestPoolCheckpointFailureLogged = false;
			TestPoolOverlayProtectsSave = true;
			try
			{
				WriteChaoSaveRecordsToMemory(overlay);
				WriteChaoSaveItemsToMemory(overlayItems);
			}
			catch (const std::exception& writeError)
			{
				try
				{
					WriteChaoSaveRecordsToMemory(TestPoolVanillaRecords);
					WriteChaoSaveItemsToMemory(TestPoolVanillaItems);
					TestPoolOverlaySession->Cancel(TestPoolVanillaRecords);
					TestPoolOverlaySession.reset();
					TestPoolOverlayProtectsSave = false;
					TestPoolRestorePending = false;
					TestPoolRestoreFailureLogged = false;
					TestPoolSidecarCommitted = false;
					TestPoolCheckpointFailureLogged = false;
					TestPoolVanillaRecords.clear();
					TestPoolVanillaSaveBlock.clear();
					TestPoolVanillaItems.clear();
					TestPoolOverlayBeginRecords.clear();
					TestPoolChaoSaveFilename.clear();
					Log("The virtual-garden test overlay was not applied; vanilla "
						"Chao records were restored after the write error: "
						+ std::string(writeError.what()));
					return TestPoolOverlayResult::NotRequested;
				}
				catch (const std::exception& restoreError)
				{
					TestPoolRestorePending = true;
					TestPoolRestoreFailureLogged = true;
					Log("Virtual-garden test overlay write and immediate "
						"restoration both failed; the save guard remains active "
						"while restoration is retried: "
						+ std::string(restoreError.what()));
					return TestPoolOverlayResult::Unsafe;
				}
			}

			Log("Applied the opt-in "
				+ std::string(garden_pools::SADXStageName(ActiveTestGarden))
				+ " test pool in memory; its updated records will be committed "
				"to the sidecar after teardown.");
			return TestPoolOverlayResult::Applied;
		}
		catch (const std::exception& error)
		{
			if (!TestPoolOverlayProtectsSave)
			{
				TestPoolOverlaySession.reset();
				TestPoolVanillaRecords.clear();
				TestPoolVanillaSaveBlock.clear();
				TestPoolVanillaItems.clear();
				TestPoolOverlayBeginRecords.clear();
				TestPoolChaoSaveFilename.clear();
			}
			Log("Virtual-garden test overlay could not be loaded; Neutral's "
				"original records will be used: " + std::string(error.what()));
			return TestPoolOverlayResult::NotRequested;
		}
	}

	bool IsTestPoolSaveGuardTestRequested()
	{
		std::error_code error;
		const auto markerPath = ModDirectory / TestPoolSaveGuardMarkerFilename;
		const bool markerExists =
			std::filesystem::exists(markerPath, error);
		if (error)
		{
			Log("Could not check the Station Square save-guard test marker: "
				+ error.message());
			return false;
		}
		return markerExists;
	}

	void UpdateTestGardenSelection()
	{
		if (ModDirectory.empty())
		{
			return;
		}

		std::error_code error;
		const bool testControlsEnabled = std::filesystem::exists(
			ModDirectory / TestPoolOverlayMarkerFilename,
			error);
		if (error)
		{
			Log("Could not check the virtual-garden test marker: "
				+ error.message());
			return;
		}

		if (!testControlsEnabled
			|| CurrentChaoArea != 7
			|| NextChaoArea != 7)
		{
			for (std::size_t index = 0;
				index < TestGardenSelectionKeyWasDown.size();
				++index)
			{
				const int key = VK_NUMPAD1 + static_cast<int>(index);
				TestGardenSelectionKeyWasDown[index] =
					(GetAsyncKeyState(key) & 0x8000) != 0;
			}
			return;
		}

		for (std::size_t index = 0;
			index < TestGardenSelectionKeyWasDown.size();
			++index)
		{
			const int key = VK_NUMPAD1 + static_cast<int>(index);
			const bool isDown = (GetAsyncKeyState(key) & 0x8000) != 0;
			const bool wasDown = TestGardenSelectionKeyWasDown[index];
			TestGardenSelectionKeyWasDown[index] = isDown;
			if (isDown && !wasDown)
			{
				SelectedAddedGarden = garden_pools::AddedGardens[index];
				Log("Selected virtual garden "
					+ std::string(
						garden_pools::SADXStageName(SelectedAddedGarden))
					+ " (Numpad " + std::to_string(index + 1)
					+ "); use the existing Neutral Garden entrance.");
			}
		}
	}

	// Test aid: with EnableLobbyAutoApproachTest.flag present, stand the player
	// just in front of the third gate once the lobby is ready.
	void AutoApproachLobbyGate()
	{
		static int readyFrames = 0;
		static bool done = false;
		if (CurrentChaoArea != 7 || NextChaoArea != 7)
		{
			readyFrames = 0;
			done = false;
			return;
		}
		std::error_code error;
		if (done || ModDirectory.empty()
			|| !std::filesystem::exists(
				ModDirectory / "EnableLobbyAutoApproachTest.flag", error))
		{
			return;
		}
		if (++readyFrames < 240)
		{
			return;
		}
		done = true;
		const auto& gate = LobbyGateSlots[2];
		auto& player = *MainCharObj1[0];
		player.Position.x = gate.x + gate.FacingX * 14.0f;
		player.Position.y = gate.y;
		player.Position.z = gate.z + gate.FacingZ * 14.0f;
		constexpr double radiansToBams = 65536.0 / (2.0 * 3.14159265358979);
		player.Rotation.y = static_cast<int>(
			std::atan2(-gate.FacingX, -gate.FacingZ) * radiansToBams);
		Log("Auto-approach test placed the player near lobby gate 2.");
	}

	void UpdateLobbyGateSelection()
	{
		if (CurrentChaoArea == 7 && NextChaoArea == 7
			&& MainCharObj1[0] != nullptr && LobbyReturnHandled)
		{
			AutoApproachLobbyGate();
		}
		if (CurrentChaoArea != 7 || NextChaoArea != 7
			|| !TestLobbyGateSpawned || MainCharObj1[0] == nullptr
			|| !LobbyReturnHandled)
		{
			return;
		}
		constexpr float entranceRadius = 30.0f;
		const auto& player = MainCharObj1[0]->Position;
		bool nearby = false;
		LobbyReturnGateIndex = -1;
		for (std::size_t index = 0; index < LobbyGateSlots.size(); ++index)
		{
			const float dx = player.x - LobbyGateSlots[index].x;
			const float dz = player.z - LobbyGateSlots[index].z;
			if (dx * dx + dz * dz <= entranceRadius * entranceRadius)
			{
				nearby = true;
				LobbyReturnGateIndex = static_cast<int>(index);
				static int nearFrames = 0;
				if (++nearFrames % 30 == 1)
				{
					Log("Near lobby entrance " + std::to_string(index)
						+ " player=(" + std::to_string(player.x) + ", "
						+ std::to_string(player.y) + ", " + std::to_string(player.z)
						+ ").");
				}
				if (SelectedAddedGarden != garden_pools::AddedGardens[index]
					|| !LobbyGateNearby)
				{
					Log("Lobby entrance selected: "
						+ std::string(garden_pools::SADXStageName(
							garden_pools::AddedGardens[index])) + ".");
				}
				SelectedAddedGarden = garden_pools::AddedGardens[index];
				break;
			}
		}
		LobbyGateNearby = nearby;
	}

	bool TestLobbyGateRequested()
	{
		std::error_code error;
		return !ModDirectory.empty()
			&& true;
	}

	bool TestPoolOverlayRequested()
	{
		std::error_code error;
		const bool requested = !ModDirectory.empty()
			&& true;
		if (error)
		{
			Log("Could not check the virtual-garden pool test marker: "
				+ error.message());
		}
		return requested && !error;
	}

	void TrySpawnTestLobbyGate()
	{
		if (CurrentChaoArea != 7 || NextChaoArea != 7)
		{
			TestLobbyGateSpawned = false;
			TestLobbyGateFailureLogged = false;
			return;
		}
		if (ModDirectory.empty() || TestLobbyGateSpawned)
		{
			return;
		}

		std::error_code error;
		const bool testGateRequested = true;
		if (error)
		{
			if (!TestLobbyGateFailureLogged)
			{
				TestLobbyGateFailureLogged = true;
				Log("Could not check the virtual-garden gate test marker: "
					+ error.message());
			}
			return;
		}
		if (!testGateRequested)
		{
			return;
		}

		try
		{
			for (std::size_t index = 0; index < LobbyGateSlots.size(); ++index)
			{
				auto position = NJS_VECTOR{
					LobbyGateSlots[index].x, LobbyGateSlots[index].y, LobbyGateSlots[index].z };
				auto* gate = ALO_LobbyGateNeutExecutor_Load(&position);
				if (gate == nullptr || gate->Data1.Entity == nullptr)
				{
					throw std::runtime_error(
						"The Neutral Lobby gate loader returned no object.");
				}
				// Face along the slot's facing direction (the vanilla gates face the centre).
				constexpr double radiansToBams = 65536.0 / (2.0 * 3.14159265358979);
				gate->Data1.Entity->Rotation.y = static_cast<int>(
					std::atan2(LobbyGateSlots[index].FacingX,
						LobbyGateSlots[index].FacingZ) * radiansToBams);
				// Hide the generic gate model; its trigger logic keeps running.
				gate->DisplaySub = nullptr;
			}

			TestLobbyGateSpawned = true;
			TestLobbyGateFailureLogged = false;
			Log("Spawned three marker-gated Neutral Lobby entrance prototypes; "
				"each uses the existing Neutral gate behavior.");
		}
		catch (const std::exception& error)
		{
			if (!TestLobbyGateFailureLogged)
			{
				TestLobbyGateFailureLogged = true;
				Log("Could not spawn the marker-gated Neutral Lobby entrance "
					"prototype: " + std::string(error.what()));
			}
		}
	}

	void TryRestorePendingTestPoolOverlay()
	{
		if (!TestPoolOverlayProtectsSave
			|| !TestPoolRestorePending
			|| IsTestPoolSaveGuardTestRequested())
		{
			return;
		}

		const auto counts = CountChaoObjects();
		if (!counts.Complete
			|| counts.MainSubCount != 0
			|| counts.DeleteSubCount != 0)
		{
			return;
		}

		try
		{
			CompleteTestPoolOverlay();
		}
		catch (const std::exception& error)
		{
			if (!TestPoolRestoreFailureLogged)
			{
				TestPoolRestoreFailureLogged = true;
				Log("Could not restore the original Neutral Chao records yet; "
					"the test save guard remains active: "
					+ std::string(error.what()));
			}
		}
	}

	// Builds the record table to restore after a virtual-garden visit. Slots
	// the game modified during the visit are inspected so chao carried across
	// the boundary survive:
	// - the chao in the player's hands keeps its live record (relocated to a
	//   free slot, with the held pointer updated, when its slot hides one of
	//   Neutral's own chao);
	// - a pool chao whose garden byte left the hosted garden (the player
	//   carried it out) migrates into a free slot of the real table;
	// - a vanilla chao that joined the hosted garden (carried in) was captured
	//   into the sidecar pool, so its real slot is cleared;
	// - an untouched slot whose record changed (e.g. a lobby chao's data)
	//   keeps its live bytes.
	garden_pools::ChaoSaveRecordTable MergeCarriedChaoIntoVanilla(
		const garden_pools::ChaoSaveRecordTable& liveRecords,
		std::size_t heldChaoSlot)
	{
		using namespace garden_pools;
		auto merged = TestPoolVanillaRecords;
		if (TestPoolOverlayBeginRecords.size() != liveRecords.size()
			|| merged.size() != liveRecords.size())
		{
			return merged;
		}
		const auto record = [&](const ChaoSaveRecordTable& table, std::size_t index)
		{
			return table.data() + index * ChaoSaveRecordSize;
		};
		const auto placeInFreeSlot = [&](const std::uint8_t* liveRecord) -> std::size_t
		{
			for (std::size_t free = 0; free < ChaoSaveRecordCount; ++free)
			{
				auto* candidate = merged.data() + free * ChaoSaveRecordSize;
				if (candidate[ChaoTypeOffset] == EmptyChaoType)
				{
					std::memcpy(candidate, liveRecord, ChaoSaveRecordSize);
					return free;
				}
			}
			return NoHeldItemSlot;
		};
		for (std::size_t index = 0; index < ChaoSaveRecordCount; ++index)
		{
			const auto* live = record(liveRecords, index);
			const auto* begin = record(TestPoolOverlayBeginRecords, index);
			const auto* vanilla = record(TestPoolVanillaRecords, index);
			const auto liveType = live[ChaoTypeOffset];
			const auto liveGarden = live[ChaoGardenOffset];
			const bool slotWasOverlaid =
				std::memcmp(begin, vanilla, ChaoSaveRecordSize) != 0;
			auto* target = merged.data() + index * ChaoSaveRecordSize;
			if (index == heldChaoSlot && liveType != EmptyChaoType)
			{
				// the chao in the player's hands leaves the garden with them
				if (!slotWasOverlaid)
				{
					std::memcpy(target, live, ChaoSaveRecordSize);
					Log("The held chao kept save slot " + std::to_string(index) + ".");
					continue;
				}
				const auto moved = placeInFreeSlot(live);
				if (moved != NoHeldItemSlot)
				{
					*reinterpret_cast<std::uintptr_t*>(HeldChaoPointerAddress) =
						ChaoSlotsAddress + moved * ChaoSaveRecordSize;
					Log("The held chao moved from the garden into save slot "
						+ std::to_string(moved) + ".");
				}
				else
				{
					std::memcpy(target, live, ChaoSaveRecordSize);
					Log("No free save slot for the held chao; it replaced the "
						"hidden chao in slot " + std::to_string(index) + ".");
				}
				continue;
			}
			if (std::memcmp(live, begin, ChaoSaveRecordSize) == 0)
			{
				continue; // unchanged during the visit
			}
			if (liveType != EmptyChaoType && liveGarden != NeutralGardenId)
			{
				if (!slotWasOverlaid)
				{
					// a real chao's record changed (e.g. lobby chao data)
					std::memcpy(target, live, ChaoSaveRecordSize);
					Log("Kept the changed non-garden chao record in slot "
						+ std::to_string(index) + ".");
					continue;
				}
				// pool chao carried out: find a free real slot for it
				const auto moved = placeInFreeSlot(live);
				if (moved != NoHeldItemSlot)
				{
					Log("A chao carried out of the virtual garden moved into "
						"save slot " + std::to_string(moved) + ".");
				}
				else
				{
					Log("A chao was carried out of the virtual garden but no "
						"save slot is free; it stays in the garden's pool.");
				}
			}
			else if (liveType != EmptyChaoType && liveGarden == NeutralGardenId
				&& !slotWasOverlaid && vanilla[ChaoTypeOffset] != EmptyChaoType)
			{
				// vanilla chao carried into the virtual garden: it is now in
				// the sidecar pool, so it leaves the real table
				std::memset(target, 0, ChaoSaveRecordSize);
				Log("A chao carried into the virtual garden left save slot "
					+ std::to_string(index) + ".");
			}
		}
		return merged;
	}

	void CompleteTestPoolOverlay()
	{
		if (!TestPoolSidecarCommitted
			&& TestPoolOverlaySession != nullptr
			&& TestPoolOverlaySession->IsActive())
		{
			const auto liveRecords = ReadChaoSaveRecordsFromMemory();
			const auto liveItems = ReadChaoSaveItemsFromMemory();
			const auto heldSlot = HeldGardenItemSlotOffset();
			const auto heldChaoSlot = HeldChaoRecordSlot();
			auto restoredItems = TestPoolOverlaySession->EndItems(liveItems, heldSlot);
			garden_pools::ChaoSaveRecordTable restoredRecords;
			const auto capturedTestPool = TestPoolOverlaySession->End(
				ExcludeHeldChaoFromCapture(liveRecords, heldChaoSlot),
				restoredRecords);
			TestPoolSidecarCommitted = true;
			if (restoredRecords != TestPoolVanillaRecords)
			{
				throw std::runtime_error(
					"The saved Neutral Chao records changed during the virtual-garden visit.");
			}
			TestPoolVanillaRecords = MergeCarriedChaoIntoVanilla(liveRecords, heldChaoSlot);
			TestPoolVanillaItems = std::move(restoredItems);
			Log("Committed " + std::to_string(capturedTestPool.size())
				+ " " + garden_pools::SADXStageName(ActiveTestGarden)
				+ " Chao and the garden's ground items to the save-scoped sidecar.");
		}

		WriteChaoSaveRecordsToMemory(TestPoolVanillaRecords);
		if (TestPoolVanillaItems.size() == garden_pools::GardenItemRegionSize)
		{
			WriteChaoSaveItemsToMemory(TestPoolVanillaItems);
		}
		TestPoolOverlaySession.reset();
		TestPoolOverlayProtectsSave = false;
		TestPoolRestorePending = false;
		TestPoolRestoreFailureLogged = false;
		TestPoolSidecarCommitted = false;
		TestPoolCheckpointFailureLogged = false;
		TestPoolVanillaRecords.clear();
		TestPoolVanillaSaveBlock.clear();
		TestPoolVanillaItems.clear();
		TestPoolOverlayBeginRecords.clear();
		TestPoolChaoSaveFilename.clear();
		Log("Restored Neutral's original Chao records after teardown.");
	}

	LONG CALLBACK LogAccessViolation(EXCEPTION_POINTERS* info)
	{
		if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
			&& NeutralLandSwapped)
		{
			char text[256];
			const auto* context = info->ContextRecord;
			const auto* record = info->ExceptionRecord;
			std::snprintf(text, sizeof(text),
				"Access violation while the converted environment is active: "
				"eip=%08lX addr=%08lX rw=%lu eax=%08lX ebx=%08lX ecx=%08lX "
				"edx=%08lX esi=%08lX edi=%08lX esp=%08lX",
				context->Eip,
				static_cast<unsigned long>(record->ExceptionInformation[1]),
				static_cast<unsigned long>(record->ExceptionInformation[0]),
				context->Eax, context->Ebx, context->Ecx, context->Edx,
				context->Esi, context->Edi, context->Esp);
			Log(text);
			const auto* stack = reinterpret_cast<const std::uint32_t*>(context->Esp);
			std::string frames = "Stack:";
			for (int i = 0; i < 24 && !IsBadReadPtr(stack + i, 4); ++i)
			{
				char word[16];
				std::snprintf(word, sizeof(word), " %08X", stack[i]);
				frames += word;
			}
			Log(frames);
			MEMORY_BASIC_INFORMATION memory = {};
			const auto dumpAddress = context->Eip >= 0x100
				? context->Eip - 0x100
				: context->Eip;
			std::uint32_t dumpSize = 0;
			if (VirtualQuery(
					reinterpret_cast<const void*>(dumpAddress),
					&memory,
					sizeof(memory)) == sizeof(memory)
				&& memory.State == MEM_COMMIT
				&& (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0)
			{
				const auto regionEnd = reinterpret_cast<std::uintptr_t>(
					memory.BaseAddress) + memory.RegionSize;
				dumpSize = static_cast<std::uint32_t>(std::min<std::uintptr_t>(
					0x400, regionEnd - dumpAddress));
			}
			if (dumpSize != 0)
			{
				std::ofstream code(
					ModDirectory / L"crash_code.bin",
					std::ios::binary);
				if (code)
				{
					code.write(
						reinterpret_cast<const char*>(&dumpAddress),
						sizeof(dumpAddress));
					code.write(
						reinterpret_cast<const char*>(&dumpSize),
						sizeof(dumpSize));
					code.write(
						reinterpret_cast<const char*>(dumpAddress),
						dumpSize);
				}
				if (code)
				{
					Log("Captured crash code at "
						+ std::to_string(dumpAddress) + " ("
						+ std::to_string(dumpSize) + " bytes).");
				}
				else
				{
					Log("Could not write crash_code.bin.");
				}
			}
			else
			{
				Log("Could not read the crashing instruction's memory region.");
			}
			if (ActiveGardenLand != nullptr)
			{
				Log("Converted image spans "
					+ std::to_string(reinterpret_cast<std::uintptr_t>(ActiveGardenLand->Image.data()))
					+ " + " + std::to_string(ActiveGardenLand->Image.size()));
			}
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	LandTable* NeutralLandExport()
	{
		return reinterpret_cast<LandTable*>(GetProcAddress(
			**datadllhandle, "objLandTableNeut"));
	}

	bool WriteLandTable(LandTable* target, const LandTable& source)
	{
		DWORD oldProtect = 0;
		if (!VirtualProtect(target, sizeof(LandTable), PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			return false;
		}
		std::memcpy(target, &source, sizeof(LandTable));
		DWORD ignored = 0;
		VirtualProtect(target, sizeof(LandTable), oldProtect, &ignored);
		return true;
	}

	// The lobby's five stock land tables are replaced by converted copies that
	// have the west wall opened and the gate corridor attached.
	std::vector<std::unique_ptr<sa2lvl::LoadedLandTable>> LobbyCorridorLand;
	static constexpr unsigned int LobbyTexNameCapacity = 24;
	NJS_TEXNAME LobbyTexNames[5][LobbyTexNameCapacity] = {};
	std::vector<NJS_TEXLIST*> LobbyPatchedTexlists;
	const char* const LobbyExtraTexNames[3] = { "gate_brick", "gate_stone", "gate_metal" };
	bool LobbyLandPatched = false;

	void PatchLobbyLandTables()
	{
		if (LobbyLandPatched || ModDirectory.empty() || datadllhandle == nullptr
			|| *datadllhandle == nullptr || **datadllhandle == nullptr)
		{
			return;
		}
		LobbyLandPatched = true;
		static const char* const variants[] = { "000", "00k", "0dk", "h0k", "hdk" };
		int patched = 0;
		for (const char* variant : variants)
		{
			const auto path = ModDirectory / "garden_assets"
				/ (std::string("Lobby") + variant + ".sa2blvl");
			std::error_code error;
			if (!std::filesystem::exists(path, error))
			{
				continue;
			}
			try
			{
				auto* live = reinterpret_cast<LandTable*>(GetProcAddress(
					**datadllhandle, (std::string("objLandTableLobby") + variant).c_str()));
				if (live == nullptr)
				{
					throw std::runtime_error("land table export not found.");
				}
				auto land = std::make_unique<sa2lvl::LoadedLandTable>(sa2lvl::Load(path));
				land->Table->TextureName = live->TextureName;
				land->Table->TextureList = live->TextureList;
				auto* texlist = live->TextureList;
				const auto archive = ModDirectory / "gd_PC"
					/ (std::string("al_stg_lobby_") + variant + "_tex.prs");
				if (texlist != nullptr && std::filesystem::exists(archive, error)
					&& texlist->nbTexture + 3 <= LobbyTexNameCapacity
					&& std::find(LobbyPatchedTexlists.begin(), LobbyPatchedTexlists.end(),
						texlist) == LobbyPatchedTexlists.end())
				{
					// Extra gate-box textures were appended to this lobby archive.
					auto* names = LobbyTexNames[patched];
					const auto stock = texlist->nbTexture;
					std::memcpy(names, texlist->textures, stock * sizeof(NJS_TEXNAME));
					for (unsigned int extra = 0; extra < 3; ++extra)
					{
						names[stock + extra] = {};
						names[stock + extra].filename = const_cast<char*>(LobbyExtraTexNames[extra]);
					}
					DWORD oldProtect = 0;
					if (VirtualProtect(texlist, sizeof(*texlist), PAGE_READWRITE, &oldProtect))
					{
						texlist->textures = names;
						texlist->nbTexture = stock + 3;
						VirtualProtect(texlist, sizeof(*texlist), oldProtect, &oldProtect);
						LobbyPatchedTexlists.push_back(texlist);
					}
				}
				if (!WriteLandTable(live, *land->Table))
				{
					throw std::runtime_error("could not unprotect the land table.");
				}
				LobbyCorridorLand.push_back(std::move(land));
				++patched;
			}
			catch (const std::exception& patchError)
			{
				Log(std::string("Lobby corridor table ") + variant + " not applied: "
					+ patchError.what());
			}
		}
		Log("Applied the lobby corridor to " + std::to_string(patched)
			+ " of 5 lobby land tables.");
	}

	// Loads the converted map's SADX textures (a GVM in this mod's gd_PC
	// folder) before the stage draws; drawing with unloaded entries crashes.
	bool LoadMysticTextures()
	{
		const auto* profile = ActiveGardenProfile;
		std::error_code error;
		if (profile == nullptr || ModDirectory.empty()
			|| !std::filesystem::exists(
				ModDirectory / "gd_PC" / (std::string(profile->TextureArchive) + ".prs"), error))
		{
			Log("The garden texture archive is missing; the converted map keeps the Neutral textures.");
			return false;
		}
		std::memset(MysticTextureNames, 0, sizeof(MysticTextureNames));
		std::memcpy(MysticTextureName, profile->TextureArchive,
			std::strlen(profile->TextureArchive) + 1);
		MysticTextureList.nbTexture = static_cast<unsigned int>(profile->TextureCount);
		const auto result = LoadTextureList(MysticTextureName, &MysticTextureList);
		Log(std::string("LoadTextureList(") + MysticTextureName + ") returned "
			+ std::to_string(result)
			+ "; entry 0 texaddr=" + FormatAddress(static_cast<std::uintptr_t>(
				MysticTextureNames[0].texaddr)) + ".");
		return true;
	}

	// Logs the GC blend/texture parameters of Neutral's own translucent meshes so
	// the converted water can use the same setup.
	void DumpStockTranslucentMeshes(const LandTable& land)
	{
		const auto* cols = reinterpret_cast<const std::uint8_t*>(land.COLList);
		int logged = 0;
		for (int index = 0; index < land.COLCount && logged < 6; ++index)
		{
			const auto* model = *reinterpret_cast<const std::uint8_t* const*>(cols + index * 0x20 + 0x10);
			if (model == nullptr)
			{
				continue;
			}
			const auto* attach = *reinterpret_cast<const std::uint8_t* const*>(model + 4);
			if (attach == nullptr)
			{
				continue;
			}
			const auto translucentCount = *reinterpret_cast<const std::uint16_t*>(attach + 0x12);
			const auto* translucent = *reinterpret_cast<const std::uint8_t* const*>(attach + 0xC);
			if (translucentCount == 0 || translucentCount > 16 || translucent == nullptr)
			{
				continue;
			}
			const auto* params = *reinterpret_cast<const std::uint8_t* const*>(translucent);
			const auto paramCount = *reinterpret_cast<const std::uint32_t*>(translucent + 4);
			std::string line = "Stock translucent mesh COL " + std::to_string(index)
				+ " flags=" + FormatAddress(*reinterpret_cast<const std::uint32_t*>(cols + index * 0x20 + 0x1C))
				+ " params:";
			for (std::uint32_t p = 0; p < paramCount && p < 16 && params != nullptr; ++p)
			{
				line += " " + FormatAddress(*reinterpret_cast<const std::uint32_t*>(params + p * 8))
					+ "/" + FormatAddress(*reinterpret_cast<const std::uint32_t*>(params + p * 8 + 4));
			}
			Log(line);
			++logged;
		}
	}

	// Water is drawn by this plugin as an alpha-blended chunk model because the
	// converted land-table meshes are always drawn opaque by the game.
	struct WaterOverlayState
	{
		struct Corner
		{
			std::size_t Word;
			int U;
			int V;
			int Texture;
		};
		std::vector<std::int32_t> Vertices;
		std::vector<std::int16_t> Polys;
		std::vector<Corner> Corners;
		NJS_CNK_MODEL Model{};
		NJS_OBJECT Object{};
		bool Ready = false;
	};

	WaterOverlayState WaterOverlay;
	ObjectMaster* WaterOverlayObject = nullptr;
	int WaterScrollU = 0;
	int WaterScrollV = 0;
	int WaterfallScrollV = 0;
	constexpr std::uint32_t DefaultWaterOverlayAlpha = 0x99;
	// Egg Carrier's water is 25% more opaque (0x99 * 1.25).
	std::uint32_t WaterOverlayAlphaFor(const GardenProfile* profile)
	{
		return profile != nullptr && std::strcmp(profile->Stage, "AL_GARDEN02") == 0
			? 0xBF : DefaultWaterOverlayAlpha;
	}

	void PushWord(std::vector<std::int16_t>& words, std::uint32_t value)
	{
		words.push_back(static_cast<std::int16_t>(value & 0xFFFF));
		words.push_back(static_cast<std::int16_t>(value >> 16));
	}

	void LoadWaterOverlay(const std::filesystem::path& path)
	{
		WaterOverlay = WaterOverlayState{};
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			Log("No water overlay file; water stays as converted.");
			return;
		}
		std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
			std::istreambuf_iterator<char>());
		if (bytes.size() < 4)
		{
			return;
		}
		std::uint32_t count = 0;
		std::memcpy(&count, bytes.data(), 4);
		struct Triangle
		{
			std::uint32_t Texture;
			float Corners[3][5];
		};
		constexpr std::size_t TriangleBytes = 4 + 3 * 5 * 4;
		if (count == 0 || count > 4000 || bytes.size() < 4 + count * TriangleBytes)
		{
			Log("Water overlay file is invalid.");
			return;
		}
		std::vector<Triangle> triangles(count);
		for (std::uint32_t i = 0; i < count; ++i)
		{
			std::memcpy(&triangles[i], bytes.data() + 4 + i * TriangleBytes, TriangleBytes);
		}

		auto& state = WaterOverlay;
		const std::uint32_t WaterOverlayAlpha = WaterOverlayAlphaFor(ActiveGardenProfile);
		state.Vertices.push_back(0x23 | (static_cast<std::int32_t>(1 + count * 3 * 4) << 16));
		state.Vertices.push_back(static_cast<std::int32_t>(count * 3) << 16);
		for (const auto& triangle : triangles)
		{
			for (const auto& corner : triangle.Corners)
			{
				for (int axis = 0; axis < 3; ++axis)
				{
					std::int32_t bits = 0;
					std::memcpy(&bits, &corner[axis], 4);
					state.Vertices.push_back(bits);
				}
				state.Vertices.push_back(static_cast<std::int32_t>(
					(WaterOverlayAlpha << 24) | 0x00FFFFFF));
			}
		}
		state.Vertices.push_back(0xFF);

		const std::uint32_t overlayTextures[2] = {
			static_cast<std::uint32_t>(ActiveGardenProfile != nullptr ? ActiveGardenProfile->PoolTexture : -1),
			static_cast<std::uint32_t>(ActiveGardenProfile != nullptr ? ActiveGardenProfile->WaterfallTexture : -1) };
		for (const std::uint32_t texture : overlayTextures)
		{
			std::uint32_t stripCount = 0;
			for (const auto& triangle : triangles)
			{
				stripCount += triangle.Texture == texture ? 1 : 0;
			}
			if (stripCount == 0)
			{
				continue;
			}
			// Material (diffuse + ambient, source/inverse-source alpha blend).
			state.Polys.push_back(static_cast<std::int16_t>(0x13 | (0x25 << 8)));
			state.Polys.push_back(4);
			PushWord(state.Polys, (WaterOverlayAlpha << 24) | 0x00FFFFFF);
			PushWord(state.Polys, 0xFF7F7F7F);
			// Texture id with bilinear filtering and wrapping.
			state.Polys.push_back(static_cast<std::int16_t>(0x08 | (0x04 << 8)));
			state.Polys.push_back(static_cast<std::int16_t>(texture | (1 << 14)));
			// UV strips: use alpha, double sided.
			state.Polys.push_back(static_cast<std::int16_t>(0x41 | (0x1A << 8)));
			state.Polys.push_back(static_cast<std::int16_t>(1 + stripCount * 10));
			state.Polys.push_back(static_cast<std::int16_t>(stripCount));
			std::uint32_t triangleIndex = 0;
			for (const auto& triangle : triangles)
			{
				const std::uint32_t firstVertex = triangleIndex * 3;
				++triangleIndex;
				if (triangle.Texture != texture)
				{
					continue;
				}
				state.Polys.push_back(3);
				for (int corner = 0; corner < 3; ++corner)
				{
					state.Polys.push_back(static_cast<std::int16_t>(firstVertex + corner));
					const int u = static_cast<int>(std::lround(triangle.Corners[corner][3] * 255.0f));
					const int v = static_cast<int>(std::lround(triangle.Corners[corner][4] * 255.0f));
					state.Corners.push_back({ state.Polys.size(), u, v, static_cast<int>(texture) });
					state.Polys.push_back(static_cast<std::int16_t>(u));
					state.Polys.push_back(static_cast<std::int16_t>(v));
				}
			}
		}
		state.Polys.push_back(0x00FF);

		state.Model.vlist = state.Vertices.data();
		state.Model.plist = state.Polys.data();
		state.Model.center = { 0.0f, 0.0f, 0.0f };
		state.Model.r = 100000.0f;
		state.Object.evalflags = 0x17;
		state.Object.model = &state.Model;
		state.Object.scl[0] = state.Object.scl[1] = state.Object.scl[2] = 1.0f;
		state.Ready = true;
		Log("Loaded the water overlay: " + std::to_string(count) + " triangles.");
	}

	void __cdecl WaterOverlayMain(ObjectMaster*)
	{
	}

	void __cdecl WaterOverlayDelete(ObjectMaster*)
	{
		WaterOverlayObject = nullptr;
	}

	void __cdecl WaterOverlayDisplay(ObjectMaster*)
	{
		auto& state = WaterOverlay;
		static unsigned int displayCalls = 0;
		if (state.Ready && (++displayCalls == 1 || displayCalls % 600 == 0))
		{
			Log("Water overlay display call " + std::to_string(displayCalls) + " pool="
				+ std::to_string(ActiveGardenProfile != nullptr ? ActiveGardenProfile->PoolTexture : -1)
				+ " texlistCount=" + std::to_string(MysticTextureList.nbTexture)
				+ " polyWords=" + std::to_string(state.Polys.size()));
		}
		if (!state.Ready)
		{
			return;
		}
		for (const auto& corner : state.Corners)
		{
			const int poolTexture = ActiveGardenProfile != nullptr
				? ActiveGardenProfile->PoolTexture : -1;
			const int du = static_cast<int>(corner.Texture) == poolTexture ? WaterScrollU : 0;
			const int dv = static_cast<int>(corner.Texture) == poolTexture
				? WaterScrollV : -WaterfallScrollV;
			state.Polys[corner.Word] = static_cast<std::int16_t>(corner.U + du);
			state.Polys[corner.Word + 1] = static_cast<std::int16_t>(corner.V + dv);
		}
		auto* context = *reinterpret_cast<std::uint8_t**>(0x2670544);
		auto* renderFlags = reinterpret_cast<std::uint32_t*>(0x25F02D8);
		auto* texlistSlot = reinterpret_cast<std::uint32_t*>(context + 0x20);
		const std::uint32_t savedTexlist = *texlistSlot;
		__asm
		{
			movsx eax, byte ptr ds:[0x1DE4664]
			mov ecx, 0x487060
			call ecx
		}
		*renderFlags |= 0x2400;
		*texlistSlot = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&MysticTextureList));
		njPushMatrix(nullptr);
		reinterpret_cast<void(__cdecl*)()>(0x42D340)();
		reinterpret_cast<void(__cdecl*)(NJS_OBJECT*, int(__cdecl*)(NJS_CNK_MODEL*))>(0x42EB30)(
			&state.Object, reinterpret_cast<int(__cdecl*)(NJS_CNK_MODEL*)>(0x42D650));
		njPopMatrix(1);
		*renderFlags &= 0xFFFFDBFF;
		*texlistSlot = savedTexlist;
		__asm
		{
			movsx eax, byte ptr ds:[0x1DE4400]
			mov ecx, 0x487060
			call ecx
		}
	}

	void EnsureWaterOverlayObject()
	{
		if (!WaterOverlay.Ready || WaterOverlayObject != nullptr
			|| MysticTextureNames[0].texaddr == 0)
		{
			return;
		}
		auto* object = LoadObject(3, "GardenWaterOverlay", WaterOverlayMain, 0);
		if (object == nullptr)
		{
			return;
		}
		object->DisplaySub = WaterOverlayDisplay;
		object->DeleteSub = WaterOverlayDelete;
		WaterOverlayObject = object;
		Log("Spawned the water overlay object.");
	}

	// The garden title card loads "al_stagetitle_%s_%c_tex". Egg Carrier swaps the
	// format for a literal name (22 chars fit the 24-byte slot) so the game loads
	// this mod's own title archive; Neutral gets the original back.
	constexpr std::uintptr_t StageTitleFormatAddress = 0x13620F8;
	constexpr char StageTitleFormat[] = "al_stagetitle_%s_%c_tex";

	void SetStageTitleName(const char* text)
	{
		auto* target = reinterpret_cast<char*>(StageTitleFormatAddress);
		DWORD oldProtect = 0;
		if (!VirtualProtect(target, 24, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			Log("Could not unprotect the stage title format string.");
			return;
		}
		if (std::strncmp(target, "al_stagetitle_", 14) == 0)
		{
			std::memcpy(target, text, std::strlen(text) + 1);
		}
		else
		{
			Log("The stage title format string was not found where expected.");
		}
		DWORD ignored = 0;
		VirtualProtect(target, 24, oldProtect, &ignored);
	}

	// Replaces the Neutral land table with a locally generated SADX conversion
	// when one exists for the stage. A missing file leaves the vanilla scene.
	void ApplyGardenEnvironment(const char* stageName)
	{
		if (NeutralLandSwapped || ModDirectory.empty())
		{
			return;
		}
		ActiveGardenProfile = nullptr;
		for (const auto& candidate : GardenProfiles)
		{
			if (std::strcmp(candidate.Stage, stageName) == 0)
			{
				ActiveGardenProfile = &candidate;
			}
		}
		const auto path = ModDirectory / "garden_assets"
			/ (std::string(stageName) + ".sa2blvl");
		std::error_code error;
		if (ActiveGardenProfile == nullptr || !std::filesystem::exists(path, error))
		{
			ActiveGardenProfile = nullptr;
			return;
		}
		try
		{
			auto land = std::make_unique<sa2lvl::LoadedLandTable>(sa2lvl::Load(path));
			auto* live = NeutralLandExport();
			if (live == nullptr)
			{
				throw std::runtime_error("objLandTableNeut export not found.");
			}
			SavedNeutralLand = *live;
			DumpStockTranslucentMeshes(SavedNeutralLand);
			land->Table->TextureName = SavedNeutralLand.TextureName;
			land->Table->TextureList = SavedNeutralLand.TextureList;
			if (LoadMysticTextures())
			{
				land->Table->TextureName = MysticTextureName;
				land->Table->TextureList = &MysticTextureList;
			}
			if (!WriteLandTable(live, *land->Table))
			{
				throw std::runtime_error("Could not unprotect objLandTableNeut.");
			}
			SavedLandTableSA2BModels = LandTableSA2BModels;
			LandTableSA2BModels = 1;
			ActiveGardenLand = std::move(land);
			NeutralLandSwapped = true;
			auto waterPath = path;
			waterPath.replace_extension(".water");
			LoadWaterOverlay(waterPath);
			{
				const std::string titleName = std::string("al_stagetitle_")
					+ (std::strcmp(stageName, "AL_GARDEN00") == 0 ? "ss"
						: std::strcmp(stageName, "AL_GARDEN01") == 0 ? "mr" : "ec") + "_e_tex";
				if (std::filesystem::exists(ModDirectory / "gd_PC" / (titleName + ".prs"), error))
				{
					SetStageTitleName(titleName.c_str());
				}
			}
			Log(std::string("Swapped in the converted environment for ") + stageName
				+ " (" + std::to_string(ActiveGardenLand->Table->COLCount)
				+ " COLs; SA2B model mode enabled).");
		}
		catch (const std::exception& swapError)
		{
			Log(std::string("Environment swap failed; vanilla Neutral kept: ")
				+ swapError.what());
		}
	}

	void RestoreGardenEnvironment()
	{
		if (!NeutralLandSwapped)
		{
			return;
		}
		auto* live = NeutralLandExport();
		if (live != nullptr && WriteLandTable(live, SavedNeutralLand))
		{
			LandTableSA2BModels = SavedLandTableSA2BModels;
			SetStageTitleName(StageTitleFormat);
			NeutralLandSwapped = false;
			ActiveGardenProfile = nullptr;
			if (ActiveGardenLand != nullptr)
			{
				// Keep the old manager's model pointers alive through the transition.
				RetiredGardenLand.clear();
				RetiredGardenLand.push_back(std::move(ActiveGardenLand));
			}
			Log("Restored the vanilla Neutral environment and model-format mode.");
		}
		else
		{
			Log("Could not restore the Neutral land table.");
		}
	}

	// Scrolls the waterfall and pool UVs; the SADX UV animation flag is not
	// honoured by SA2's renderer.
	void AnimateGardenWater()
	{
		if (!NeutralLandSwapped || ActiveGardenLand == nullptr)
		{
			return;
		}
		static unsigned int tick = 0;
		++tick;
		const int waterfallV = static_cast<int>((tick * 5) % 256);
		const int waterU = static_cast<int>((tick / 3) % 256);
		const int waterV = static_cast<int>((tick / 5) % 256);
		WaterScrollU = waterU;
		WaterScrollV = waterV;
		WaterfallScrollV = waterfallV;
		EnsureWaterOverlayObject();
		for (auto& set : ActiveGardenLand->UvSets)
		{
			int du = 0;
			int dv = 0;
			for (const auto id : set.TextureIds)
			{
				if (ActiveGardenProfile != nullptr
					&& id == static_cast<std::uint32_t>(ActiveGardenProfile->WaterfallTexture)
					&& ActiveGardenProfile->WaterfallTexture >= 0)
				{
					dv = -waterfallV;
				}
				else if (ActiveGardenProfile != nullptr
					&& id == static_cast<std::uint32_t>(ActiveGardenProfile->PoolTexture))
				{
					du = waterU;
					dv = waterV;
				}
			}
			if (du == 0 && dv == 0)
			{
				continue;
			}
			for (std::uint32_t i = 0; i < set.PairCount; ++i)
			{
				set.Data[i * 2] = static_cast<std::int16_t>(set.Base[i * 2] + du);
				set.Data[i * 2 + 1] = static_cast<std::int16_t>(set.Base[i * 2 + 1] + dv);
			}
		}
	}

	void TryApplyMysticGroundProbeSpawn()
	{
		if (!NeutralLandSwapped)
		{
			MysticGroundProbeSpawnApplied = false;
			MysticGroundProbeMarkerFailureLogged = false;
			return;
		}
		if (ModDirectory.empty() || MysticGroundProbeSpawnApplied)
		{
			return;
		}

		// The stock entry point (59, -143) has no floor in the converted map,
		// and the game re-places the player there during the first frames, so
		// hold a verified grounded position through a later window.
		constexpr unsigned int FirstProbeFrame = 1;
		constexpr unsigned int LastProbeFrame = 50;
		if (GardenEnvironmentDiagnosticFrame < FirstProbeFrame)
		{
			return;
		}
		if (GardenEnvironmentDiagnosticFrame > LastProbeFrame)
		{
			MysticGroundProbeSpawnApplied = true;
			return;
		}
		if (MainCharObj1[0] == nullptr || MainCharObj2[0] == nullptr)
		{
			return;
		}

		auto& player = *MainCharObj1[0];
		auto& character = *MainCharObj2[0];
		if (ActiveGardenProfile == nullptr)
		{
			return;
		}
		const NJS_VECTOR spawn = ActiveGardenProfile->Spawn;
		player.Position = spawn;
		if (std::strcmp(ActiveGardenProfile->Stage, "AL_GARDEN02") == 0)
		{
			// Face away from the exit, which lies to the west of the spawn.
			player.Rotation.y = 0x4000;
			PlaceGardenCameraBehind(spawn);
		}
		character.Speed.x = 0.0f;
		character.Speed.y = 0.0f;
		character.Speed.z = 0.0f;
		if (GardenEnvironmentDiagnosticFrame == FirstProbeFrame)
		{
			Log(std::string("Holding the ") + ActiveGardenProfile->Stage
				+ " spawn position (" + std::to_string(spawn.x) + ", "
				+ std::to_string(spawn.y) + ", " + std::to_string(spawn.z)
				+ ") with zero velocity for frames 1-50.");
		}
	}

	void LogGardenEnvironmentRuntimeState()
	{
		if (CurrentChaoArea != garden_pools::NeutralGardenId)
		{
			GardenEnvironmentDiagnosticFrame = 0;
			InterlockedExchange(&PlayerGroundQueryObservations, 0);
			for (std::size_t index = 0;
				index < PlayerGroundQueryCallSites.size();
				++index)
			{
				InterlockedExchange(&PlayerGroundQueryLastLoggedFrames[index], -1);
				InterlockedExchange(&PlayerGroundQueryPreviousStates[index], -1);
			}
			MysticGroundProbeSpawnApplied = false;
			GardenExitDoorSpawned = false;
			SetGateTexlist(0x1236160);
			MysticGroundProbeMarkerFailureLogged = false;
			return;
		}

		AnimateGardenWater();
		// Egg Carrier's stock exit door requests the lobby right after spawn, so it is removed; the plugin's own position check handles leaving.
		if (ActiveGardenProfile != nullptr
			&& std::strcmp(ActiveGardenProfile->Stage, "AL_GARDEN02") == 0
			)
		{
			RemoveStockExitDoor();
		}
		TryApplyMysticGroundProbeSpawn();
		if (GardenEnvironmentDiagnosticFrame >= 36000)
		{
			return;
		}
		const auto frame = ++GardenEnvironmentDiagnosticFrame;
		TrackGardenObjectChanges(frame);
		// The stock exit trigger cube acts as a wall instead of firing in these
		// maps, so the exit is a plain position check that asks for the lobby.
		static bool exitRequested = false;
		if (frame < 5)
		{
			exitRequested = false;
		}
		const bool earlyExit = ActiveGardenProfile != nullptr
			&& std::strcmp(ActiveGardenProfile->Stage, "AL_GARDEN02") == 0;
		if (ActiveGardenProfile != nullptr && !ActiveGardenProfile->MysticFixes
			&& frame > (earlyExit ? 20u : 90u) && !exitRequested && MainCharObj1[0] != nullptr)
		{
			const auto& pos = MainCharObj1[0]->Position;
			const auto& c = ActiveGardenProfile->TriggerCenter;
			const auto& h = ActiveGardenProfile->TriggerHalfSize;
			if (std::fabs(pos.x - c.x) <= h.x && std::fabs(pos.z - c.z) <= h.z
				&& pos.y > c.y - (earlyExit ? 8.0f : 20.0f)
				&& pos.y < c.y + (earlyExit ? 12.0f : 40.0f))
			{
				exitRequested = true;
				Log("Player reached the garden exit; requesting the lobby.");
				reinterpret_cast<int(__cdecl*)(int)>(ChaoStageRequestAddress)(7);
			}
		}
		if (frame >= 3 && frame % 10 == 0)
		{
			ApplyGardenObjectFixes(frame);
		}
		if (frame >= 3 && ActiveGardenProfile != nullptr && !ActiveGardenProfile->MysticFixes
			&& std::strcmp(ActiveGardenProfile->Stage, "AL_GARDEN00") == 0)
		{
			KeepStationDecorOnGround(*ActiveGardenProfile);
		}
		if ((frame - 1) % 15 != 0)
		{
			return;
		}

		if (frame == 61)
		{
			LogGardenObjectSurvey();
		}
		if (frame == 1 || frame == 61)
		{
			const FogData* fog = FogDataPtr;
			if (fog != nullptr)
			{
				Log("Garden fog data=" + FormatAddress(reinterpret_cast<std::uintptr_t>(fog))
					+ " flags=" + std::to_string(fog->flags)
					+ " color=" + FormatAddress(static_cast<std::uintptr_t>(fog->color))
					+ " far=" + std::to_string(fog->far_)
					+ " near=" + std::to_string(fog->near_));
			}
			else
			{
				Log("Garden fog data pointer is null.");
			}
			std::string textures = "Mystic texlist frame " + std::to_string(frame)
				+ " liveTexList=" + FormatAddress(reinterpret_cast<std::uintptr_t>(
					NeutralLandExport() != nullptr
						? NeutralLandExport()->TextureList : nullptr))
				+ " expected=" + FormatAddress(
					reinterpret_cast<std::uintptr_t>(&MysticTextureList)) + " texaddr:";
			for (const auto& entry : MysticTextureNames)
			{
				textures += " " + FormatAddress(
					static_cast<std::uintptr_t>(entry.texaddr));
			}
			Log(textures);

			// Compare our entries' backing memory against stock Neutral's.
			const auto dump = [](const char* label, std::uintptr_t address)
			{
				unsigned char bytes[32] = {};
				SIZE_T read = 0;
				ReadProcessMemory(GetCurrentProcess(),
					reinterpret_cast<const void*>(address), bytes, sizeof(bytes), &read);
				std::string text = std::string("texaddr dump ") + label + " "
					+ FormatAddress(address) + " read=" + std::to_string(read) + ":";
				static const char digits[] = "0123456789abcdef";
				for (SIZE_T i = 0; i < read; ++i)
				{
					text += ' ';
					text += digits[bytes[i] >> 4];
					text += digits[bytes[i] & 15];
				}
				Log(text);
			};
			dump("mystic0", static_cast<std::uintptr_t>(MysticTextureNames[0].texaddr));
			dump("mystic12", static_cast<std::uintptr_t>(MysticTextureNames[12].texaddr));
			dump("mystic40", static_cast<std::uintptr_t>(MysticTextureNames[40].texaddr));
			if (SavedNeutralLand.TextureList != nullptr
				&& SavedNeutralLand.TextureList->textures != nullptr)
			{
				dump("neutral0", static_cast<std::uintptr_t>(
					SavedNeutralLand.TextureList->textures[0].texaddr));
			}
		}

		EntityData1 player{};
		CharObj2Base character{};
		const bool hasPlayer = MainCharObj1[0] != nullptr
			&& ReadProcessMemory(
				GetCurrentProcess(),
				MainCharObj1[0],
				&player,
				sizeof(player),
				nullptr) != FALSE;
		const bool hasCharacter = MainCharObj2[0] != nullptr
			&& ReadProcessMemory(
				GetCurrentProcess(),
				MainCharObj2[0],
				&character,
				sizeof(character),
				nullptr) != FALSE;
		ObjectMaster playerMasterSnapshot{};
		auto* const playerMaster = MainCharacter[0];
		const bool hasPlayerMaster = playerMaster != nullptr
			&& ReadProcessMemory(
				GetCurrentProcess(),
				playerMaster,
				&playerMasterSnapshot,
				sizeof(playerMasterSnapshot),
				nullptr) != FALSE;

		auto* currentTable = CurrentLandTable;
		LandTable tableSnapshot{};
		const bool hasTable = currentTable != nullptr
			&& ReadProcessMemory(
				GetCurrentProcess(),
				currentTable,
				&tableSnapshot,
				sizeof(tableSnapshot),
				nullptr) != FALSE;
		ObjectMaster managerSnapshot{};
		auto* const manager = LandManagerPtr;
		const bool hasManager = manager != nullptr
			&& ReadProcessMemory(
				GetCurrentProcess(),
				manager,
				&managerSnapshot,
				sizeof(managerSnapshot),
				nullptr) != FALSE;
		auto* const liveNeutralTable = NeutralLandExport();
		std::string message = "Garden runtime frame="
			+ std::to_string(frame)
			+ " environment="
			+ (NeutralLandSwapped ? "custom" : "vanilla")
			+ " player=" + (hasPlayer
				? "(" + std::to_string(player.Position.x) + ", "
					+ std::to_string(player.Position.y) + ", "
					+ std::to_string(player.Position.z) + ")"
				: std::string("<unreadable>"))
			+ " speed=" + (hasCharacter
				? "(" + std::to_string(character.Speed.x) + ", "
					+ std::to_string(character.Speed.y) + ", "
					+ std::to_string(character.Speed.z) + ")"
				: std::string("<unreadable>"))
			+ " CurrentLandTable=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(currentTable))
			+ " NeutralExport=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(liveNeutralTable))
			+ " LandManager=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(manager))
			+ " PlayerMainSub=" + (hasPlayerMaster
				? FormatAddress(reinterpret_cast<std::uintptr_t>(
					playerMasterSnapshot.MainSub))
				: std::string("<unreadable>"));
		if (hasManager)
		{
			message += " ManagerMainSub=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(managerSnapshot.MainSub))
				+ " ManagerDeleteSub=" + FormatAddress(
					reinterpret_cast<std::uintptr_t>(managerSnapshot.DeleteSub))
				+ " ManagerData1=" + FormatAddress(
					reinterpret_cast<std::uintptr_t>(
						managerSnapshot.Data1.Undefined))
				+ " ManagerSETData=" + FormatAddress(
					reinterpret_cast<std::uintptr_t>(managerSnapshot.SETData));
		}
		else
		{
			message += " ManagerState=<unreadable>";
		}
		if (hasPlayer)
		{
			message += " PlayerStatus=" + std::to_string(player.Status)
				+ " PlayerGrounded="
				+ ((player.Status & Status_Ground) != 0 ? "1" : "0")
				+ " PlayerAction="
				+ std::to_string(static_cast<unsigned int>(
					static_cast<unsigned char>(player.Action)))
				+ " PlayerNextAction="
				+ std::to_string(static_cast<unsigned int>(
					static_cast<unsigned char>(player.NextAction)));
		}
		if (hasCharacter)
		{
			message += " CharacterCollisionFlags="
				+ std::to_string(character.CollisionFlags);
		}
		message += " SA2BModelMode="
			+ std::to_string(LandTableSA2BModels);
		const auto appendSplitTable = [&message](
			const char* name,
			const LandTable* table)
		{
			LandTable snapshot{};
			if (ReadProcessMemory(
				GetCurrentProcess(),
				table,
				&snapshot,
				sizeof(snapshot),
				nullptr) == FALSE)
			{
				message += std::string(" ") + name + "=<unreadable>";
				return;
			}
			message += std::string(" ") + name + "={COLs="
				+ std::to_string(snapshot.COLCount)
				+ ",GC_COLs=" + std::to_string(snapshot.ChunkModelCount)
				+ ",COLList="
				+ FormatAddress(reinterpret_cast<std::uintptr_t>(
					snapshot.COLList))
				+ "}";
		};
		appendSplitTable("BasicLandTable", &BasicLandTable);
		appendSplitTable("ChunkLandTable", &ChunkLandTable);
		message += " LandTableSA2BModels="
			+ std::to_string(LandTableSA2BModels);
		if (hasTable)
		{
			message += " COLs=" + std::to_string(tableSnapshot.COLCount)
				+ " GC_COLs=" + std::to_string(tableSnapshot.ChunkModelCount)
				+ " COLList=" + FormatAddress(
					reinterpret_cast<std::uintptr_t>(tableSnapshot.COLList));
		}
		else
		{
			message += " table=<unreadable>";
		}
		Log(message + ".");
	}

	void TryRestoreTestPoolOverlayAfterReturn(
		const ChaoObjectCounts& chaoObjectsAfter)
	{
		if (!TestPoolOverlayProtectsSave)
		{
			return;
		}
		if (!chaoObjectsAfter.Complete
			|| chaoObjectsAfter.MainSubCount != 0
			|| chaoObjectsAfter.DeleteSubCount != 0)
		{
			TestPoolRestorePending = true;
			if (!TestPoolRestoreFailureLogged)
			{
				TestPoolRestoreFailureLogged = true;
				Log("The Lobby transition completed with Chao tasks still present "
					"or an incomplete scan; restoration will wait for a complete "
					"zero-task scan.");
			}
			return;
		}
		if (IsTestPoolSaveGuardTestRequested())
		{
			TestPoolRestorePending = true;
			if (!TestPoolRestoreFailureLogged)
			{
				TestPoolRestoreFailureLogged = true;
				Log("Deferred Neutral record restoration for the opt-in save-guard "
					"test; remove the save-test marker to allow restoration if the "
					"save is cancelled.");
			}
			return;
		}

		try
		{
			CompleteTestPoolOverlay();
		}
		catch (const std::exception& error)
		{
			TestPoolRestorePending = true;
			if (!TestPoolRestoreFailureLogged)
			{
				TestPoolRestoreFailureLogged = true;
				Log("The test-pool restoration is pending after the Lobby "
					"transition; the save guard remains active: "
					+ std::string(error.what()));
			}
		}
	}

	std::string FormatAddress(std::uintptr_t address)
	{
		static constexpr char HexDigits[] = "0123456789ABCDEF";
		std::array<char, sizeof(address) * 2> digits{};
		for (std::size_t index = 0; index < digits.size(); ++index)
		{
			const auto shift = (digits.size() - index - 1) * 4;
			digits[index] = HexDigits[(address >> shift) & 0x0F];
		}
		return "0x" + std::string(digits.begin(), digits.end());
	}

	ChaoObjectCounts CountChaoObjects()
	{
		ChaoObjectCounts counts{};
		counts.Complete = true;

		for (std::size_t listIndex = 0;
			listIndex < static_cast<std::size_t>(ObjectLists_Length);
			++listIndex)
		{
			std::array<std::uintptr_t, MaxObjectListTraversal> visited{};
			std::size_t visitedCount = 0;
			auto* object = ObjectLists[listIndex];
			while (object != nullptr)
			{
				const auto objectAddress = reinterpret_cast<std::uintptr_t>(object);
				if (objectAddress < 0x10000 || objectAddress >= 0x7FFF0000)
				{
					counts.Complete = false;
					break;
				}

				bool alreadyVisited = false;
				for (std::size_t index = 0; index < visitedCount; ++index)
				{
					if (visited[index] == objectAddress)
					{
						alreadyVisited = true;
						break;
					}
				}
				if (alreadyVisited)
				{
					break;
				}
				if (visitedCount == visited.size())
				{
					counts.Complete = false;
					break;
				}
				visited[visitedCount++] = objectAddress;

				ObjectMaster snapshot{};
				SIZE_T bytesRead = 0;
				if (!ReadProcessMemory(
					GetCurrentProcess(),
					object,
					&snapshot,
					sizeof(snapshot),
					&bytesRead)
					|| bytesRead != sizeof(snapshot))
				{
					counts.Complete = false;
					break;
				}

				if (reinterpret_cast<std::uintptr_t>(snapshot.MainSub)
					== ChaoMainObjectFunctionAddress)
				{
					++counts.MainSubCount;
				}
				if (reinterpret_cast<std::uintptr_t>(snapshot.DeleteSub)
					== ChaoDeleteObjectFunctionAddress)
				{
					++counts.DeleteSubCount;
				}

				object = snapshot.NextObject;
			}
		}

		return counts;
	}

	struct LobbyObjectInfo
	{
		std::uintptr_t Address;
		std::uintptr_t MainSub;
		NJS_VECTOR Position;
		::Rotation Angles;
	};

	// Snapshots lobby objects whose main function is in `mainFunctions`.
	std::vector<LobbyObjectInfo> FindObjectsByMainSub(
		const std::vector<std::uintptr_t>& mainFunctions)
	{
		std::vector<LobbyObjectInfo> found;
		for (std::size_t listIndex = 0;
			listIndex < static_cast<std::size_t>(ObjectLists_Length);
			++listIndex)
		{
			auto* object = ObjectLists[listIndex];
			for (std::size_t steps = 0;
				object != nullptr && steps < MaxObjectListTraversal;
				++steps)
			{
				const auto objectAddress = reinterpret_cast<std::uintptr_t>(object);
				if (objectAddress < 0x10000 || objectAddress >= 0x7FFF0000)
				{
					break;
				}
				ObjectMaster snapshot{};
				SIZE_T bytesRead = 0;
				if (!ReadProcessMemory(GetCurrentProcess(), object, &snapshot,
					sizeof(snapshot), &bytesRead)
					|| bytesRead != sizeof(snapshot))
				{
					break;
				}
				const auto mainSub =
					reinterpret_cast<std::uintptr_t>(snapshot.MainSub);
				bool wanted = mainFunctions.empty();
				for (const auto candidate : mainFunctions)
				{
					wanted = wanted || mainSub == candidate;
				}
				bool duplicate = false;
				for (const auto& existing : found)
				{
					duplicate = duplicate || existing.Address == objectAddress;
				}
				EntityData1 data{};
				if (wanted && !duplicate
					&& snapshot.Data1.Entity != nullptr
					&& ReadProcessMemory(GetCurrentProcess(),
						snapshot.Data1.Entity, &data, sizeof(data),
						&bytesRead)
					&& bytesRead == sizeof(data))
				{
					found.push_back({ objectAddress, mainSub, data.Position,
						data.Rotation });
				}
				object = snapshot.NextObject;
			}
		}
		return found;
	}

	// The Mystic tunnel ends at x=-7 (floor y=12). The vanilla exit objects and
	// the Chao transporter use Neutral coordinates, so they are moved here.
	// The lobby gate display sets the texture list from an immediate at
	// 0x57F7DF. While the gate stands in the garden, point it at the Mystic
	// list; the original value is restored when the garden is left.
	constexpr std::uintptr_t GateTexlistImmediate = 0x57F7DF;

	void SetGateTexlist(std::uint32_t value)
	{
		auto* target = reinterpret_cast<std::uint32_t*>(GateTexlistImmediate);
		DWORD oldProtect = 0;
		if (VirtualProtect(target, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			*target = value;
			VirtualProtect(target, 4, oldProtect, &oldProtect);
		}
	}

	void __cdecl GardenDoorMainStub(ObjectMaster*)
	{
	}

	void __cdecl InertObjectMain(ObjectMaster*)
	{
	}

	// Ground height under (x, z) for Station Square or Mystic Ruins; false when the map has none there.
	bool GardenGroundHeight(const GardenProfile& profile, float x, float z, float& y)
	{
		namespace gg = garden_ground;
		const bool mystic = profile.MysticFixes;
		const auto& cells = mystic ? gg::MysticGrid.Cells : gg::StationGrid.Cells;
		const int x0 = mystic ? gg::MysticGrid.X0 : gg::StationGrid.X0;
		const int z0 = mystic ? gg::MysticGrid.Z0 : gg::StationGrid.Z0;
		const int columns = mystic ? gg::MysticGrid.Columns : gg::StationGrid.Columns;
		const int rows = mystic ? gg::MysticGrid.Rows : gg::StationGrid.Rows;
		return gg::Height(cells, x0, z0, columns, rows, x, z, y) != 0.0f;
	}

	// Coarse ground height of Egg Carrier on a 20-unit grid (x from -340, z from -280).
	float EggCarrierGroundHeight(float x, float z)
	{
		static const signed char grid[29][35] = {
		{ -2, -2, -2, -2, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -2, -2, -2, -2, -2 },
		{ -2, -2, -2, -2, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -2, -2, -2 },
		{ -2, -2, -2, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -2, -2 },
		{ -2, -2, -2, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -2 },
		{ -2, -2, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -2 },
		{ -2, -2, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1 },
		{ -2, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 5, 5, 5, 5, 5, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1 },
		{ -2, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 16, 16, 16, 14, 13, 11, 10, 8, 7, 6, 6, 6, 6, 6, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1 },
		{ -2, -1, -1, -1, 0, 0, 0, 0, 0, 0, 16, 16, 16, 16, 16, 15, 15, 13, 11, 10, 9, 9, 7, 7, 7, 7, 0, 0, 0, 0, 0, 0, -1, -1, -1 },
		{ -2, -1, -1, -1, 0, 0, 0, 0, 0, 16, 16, 16, 16, 16, 16, 16, 16, 16, 14, 13, 12, 13, 13, 7, 7, 7, 7, 0, 0, 0, 0, 0, 0, -1, -1 },
		{ -2, -1, -1, 0, 0, 0, 0, 0, 0, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 15, 15, 15, 15, 16, 7, 7, 7, 7, 0, 0, 0, 0, 0, -1, -1 },
		{ -2, -1, -1, 0, 0, 0, 0, 0, 0, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 7, 7, 6, 0, 0, 0, 0, 0, -1, -1 },
		{ -2, -1, -1, 0, 0, 0, 0, 0, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 7, 6, 6, 0, 0, 0, 0, 0, -1, -1 },
		{ -2, -1, -1, 0, 0, 0, 0, 1, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 7, 6, 5, 0, 0, 0, 0, 0, -1, -1 },
		{ -1, -1, -1, 0, 0, 0, 1, 1, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 7, 6, 5, 0, 0, 0, 0, 0, -1, -1 },
		{ -1, -1, -1, 0, 0, 0, 0, 1, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 5, 5, 4, 0, 0, 0, 0, 0, -1, -1 },
		{ -1, -1, -1, 0, 0, 0, 0, 2, 1, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 4, 4, 3, 3, 0, 0, 0, 0, 0, -1, -1 },
		{ -2, -1, 0, 0, 0, 0, 0, 2, 4, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 4, 3, 3, 2, 1, 0, 0, 0, 0, 0, -1, -1 },
		{ -2, -1, 0, 0, 0, 0, 1, 1, 3, 4, 16, 16, 16, 15, 16, 16, 16, 16, 14, 14, 15, 16, 4, 3, 3, 2, 1, 0, 0, 0, 0, 0, 0, -1, -1 },
		{ -1, -1, 0, 0, 0, 0, 0, 3, 3, 5, 6, 14, 14, 13, 14, 13, 13, 12, 11, 9, 6, 5, 4, 2, 2, 1, 0, 0, 0, 0, 0, 0, -1, -1, -1 },
		{ -1, 0, 0, 0, 0, 0, 0, 6, 5, 5, 6, 7, 9, 14, 11, 10, 9, 9, 7, 6, 4, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1 },
		{ -1, 0, 0, 0, 0, 0, 0, 6, 6, 5, 6, 7, 8, 8, 9, 9, 7, 5, 3, 7, 6, 4, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1 },
		{ -1, 0, 0, 0, 0, 0, 0, 0, 6, 6, 7, 7, 7, 7, 7, 7, 6, 7, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1 },
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 6, 7, 7, 7, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -2 },
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -2, -2 },
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -2, -2, -2 },
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -2, -2, -2 },
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -2, -2, -2, -2 },
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3 }
		};
		const auto cell = [](float value, float origin, int count, int& index, float& fraction)
		{
			float position = (value - origin) / 20.0f;
			position = std::fmin(std::fmax(position, 0.0f), static_cast<float>(count - 1) - 0.001f);
			index = static_cast<int>(position);
			fraction = position - static_cast<float>(index);
		};
		int ix = 0, iz = 0;
		float fx = 0.0f, fz = 0.0f;
		cell(x, -340.0f, 35, ix, fx);
		cell(z, -280.0f, 29, iz, fz);
		const float top = grid[iz][ix] * (1.0f - fx) + grid[iz][ix + 1] * fx;
		const float bottom = grid[iz + 1][ix] * (1.0f - fx) + grid[iz + 1][ix + 1] * fx;
		return top * (1.0f - fz) + bottom * fz;
	}

	// Stops an object's own main function so it no longer registers its
	// collision or trigger, without deleting it (deletion does not stick for
	// some stock objects).
	void NeutralizeObjects(const std::vector<std::uintptr_t>& mains)
	{
		for (const auto& object : FindObjectsByMainSub(mains))
		{
			auto* master = reinterpret_cast<ObjectMaster*>(object.Address);
			master->MainSub = InertObjectMain;
			if (master->Data1.Entity != nullptr)
			{
				master->Data1.Entity->Collision = nullptr;
				master->Data1.Entity->Position.y = -1000.0f;
			}
		}
	}

	void RemoveStockExitDoor()
	{
		for (const auto& object : FindObjectsByMainSub({ 0x55AB90, 0x6E54E0 }))
		{
			DeleteObject_(reinterpret_cast<ObjectMaster*>(object.Address));
		}
	}

	// Station Square decor and fauna spawn at the stock Neutral heights and drift,
	// so every one still above the ground is removed each frame.
	void KeepStationDecorOnGround(const GardenProfile& profile)
	{
		for (const auto& object : FindObjectsByMainSub({ 0x5B4250, 0x46F720 }))
		{
			auto* master = reinterpret_cast<ObjectMaster*>(object.Address);
			auto* data = master->Data1.Entity;
			if (data == nullptr || data->Position.y < -500.0f)
			{
				continue;
			}
			float ground = 0.0f;
			if (!GardenGroundHeight(profile, data->Position.x, data->Position.z, ground)
				|| data->Position.y > ground + 15.0f)
			{
				// Deleting respawns the SET object every frame; an inert, sunken one stays put.
				master->MainSub = InertObjectMain;
				master->DisplaySub = nullptr;
				data->Collision = nullptr;
				data->Position.y = -1000.0f;
			}
		}
	}

	void ApplyGardenObjectFixes(unsigned int frame)
	{
		const auto* profile = ActiveGardenProfile;
		if (profile == nullptr)
		{
			return;
		}
		const float doorX = profile->Door.x;
		const float doorY = profile->Door.y;
		const float doorZ = profile->Door.z;
		constexpr int halfTurn = 0x8000;
		ObjectMaster* exitDoor = nullptr;
		// Keep the exit trigger out of play while the spawn hold runs.
		for (const auto& cube : FindObjectsByMainSub({ 0x6E54E0 }))
		{
			auto* cubeData = reinterpret_cast<ObjectMaster*>(cube.Address)->Data1.Entity;
			if (cubeData == nullptr)
			{
				continue;
			}
			if (frame < 90 || !profile->MysticFixes)
			{
				cubeData->Position.y = -1000.0f;
			}
			else if (cubeData->Position.y < -500.0f)
			{
				cubeData->Position.y = profile->TriggerCenter.y;
			}
		}
		for (const auto& object : FindObjectsByMainSub({ 0x55AB90 }))
		{
			exitDoor = reinterpret_cast<ObjectMaster*>(object.Address);
		}
		if (exitDoor != nullptr && exitDoor->Data1.Entity != nullptr)
		{
			auto* door = exitDoor->Data1.Entity;
			if (std::fabs(door->Position.x - doorX) > 0.01f)
			{
				door->Position = { doorX, doorY, doorZ };
				door->Rotation.y += halfTurn;
				for (const auto& cube : FindObjectsByMainSub({ 0x6E54E0 }))
				{
					auto* cubeData = reinterpret_cast<ObjectMaster*>(cube.Address)
						->Data1.Entity;
					if (cubeData != nullptr)
					{
						// Fit the trigger to the gate opening (z -159 +-10, x 118..130)
						// so it only fires from inside the gate.
						cubeData->Position = profile->TriggerCenter;
						if (frame < 90 || !profile->MysticFixes)
						{
							cubeData->Position.y = -1000.0f;
						}
						cubeData->Rotation.y = 0;
						auto* info = cubeData->Collision;
						if (info != nullptr && info->CollisionArray != nullptr)
						{
							for (int i = 0; i < info->Count; ++i)
							{
								auto& shape = info->CollisionArray[i];
								Log("Exit trigger shape " + std::to_string(i) + " kind="
									+ std::to_string(shape.field_2) + " center=("
									+ std::to_string(shape.some_vector.x) + ","
									+ std::to_string(shape.some_vector.y) + ","
									+ std::to_string(shape.some_vector.z) + ") size=("
									+ std::to_string(shape.anonymous_1) + ","
									+ std::to_string(shape.anonymous_2) + ","
									+ std::to_string(shape.anonymous_3) + ")");
								shape.some_vector = { 0.0f, 0.0f, 0.0f };
								shape.anonymous_1 = profile->TriggerHalfSize.x;
								shape.anonymous_2 = profile->TriggerHalfSize.y;
								shape.anonymous_3 = profile->TriggerHalfSize.z;
							}
						}
					}
				}
				Log(std::string("Moved the Neutral exit door and trigger for ") + profile->Stage + ".");
			}
			else if (door->Position.y > -500.0f)
			{
				// The door object's own solid collision is the invisible wall at
				// the tunnel end; the trigger cube alone sends the player out.
				door->Position.y = -1000.0f;
				Log("Sank the exit door object's collision.");
			}
		}
		const bool eggCarrierStage = std::strcmp(profile->Stage, "AL_GARDEN02") == 0;
		for (const auto& object : FindObjectsByMainSub({ 0x57D540 }))
		{
			auto* data = reinterpret_cast<ObjectMaster*>(object.Address)->Data1.Entity;
			const float transporterY = profile->MysticFixes ? -6.0f
				: eggCarrierStage ? EggCarrierGroundHeight(data != nullptr ? data->Position.x : 0.0f,
					data != nullptr ? data->Position.z : 0.0f) : 0.0f;
			if (data != nullptr && std::fabs(data->Position.y - transporterY) > 0.01f
				&& data->Position.y > -500.0f)
			{
				data->Position.y = transporterY;
				Log("Lowered the Chao transporter to the garden ground.");
			}
		}
		// The Chao race entrance sits in the converted pool area.
		if (!profile->MysticFixes)
		{
			NeutralizeObjects({ 0x55AC60 });
			if (std::strcmp(profile->Stage, "AL_GARDEN02") == 0)
			{
				// Egg Carrier keeps a single exit, so every other stock exit or warp is stopped.
				NeutralizeObjects({ 0x55AB20, 0x46F720, 0x6E5400, 0x6E5470 });
			}
		}
		// Neutral's trees whose trunks fail to render are removed.
		struct TreeSpot { float x; float z; bool keep; };
		static const TreeSpot treeSpots[] = {
			{ -50.0f, 7.0f, false }, { 4.0f, 35.0f, false }, { -0.3f, -50.0f, false },
			{ -33.0f, -27.0f, true }, { 54.0f, -47.0f, true }, { 70.0f, -84.0f, true },
			{ -10.0f, -145.0f, false } };
		const auto nearestTree = [](float x, float z) -> const TreeSpot&
		{
			const TreeSpot* best = &treeSpots[0];
			float bestDistance = 1e30f;
			for (const auto& spot : treeSpots)
			{
				const float distance = (spot.x - x) * (spot.x - x) + (spot.z - z) * (spot.z - z);
				if (distance < bestDistance)
				{
					bestDistance = distance;
					best = &spot;
				}
			}
			return *best;
		};
		for (const auto& object : FindObjectsByMainSub({ 0x546810, 0x545E40 }))
		{
			static volatile bool deleteBrokenTrees = true;
			if (deleteBrokenTrees
				&& (!profile->MysticFixes || !nearestTree(object.Position.x, object.Position.z).keep))
			{
				DeleteObject_(reinterpret_cast<ObjectMaster*>(object.Address));
				Log("Removed a garden tree or fruit with a broken trunk.");
			}
			else if (profile->MysticFixes && object.MainSub == 0x546810
				&& std::fabs(object.Position.x + 33.0f) < 6.0f && std::fabs(object.Position.z + 27.0f) < 6.0f
				&& object.Position.y > -5.5f)
			{
				// The tree standing in the pool floats at the stock height; the pool floor is -5.7.
				auto* data = reinterpret_cast<ObjectMaster*>(object.Address)->Data1.Entity;
				if (data != nullptr)
				{
					data->Position.y = -5.7f;
					Log("Lowered the pool tree to the pool floor.");
				}
			}
		}
		// Egg Carrier decor and fauna sit on the ground instead of floating.
		if (eggCarrierStage)
		{
			for (const auto& object : FindObjectsByMainSub({ 0x5B4250, 0x46F720 }))
			{
				auto* data = reinterpret_cast<ObjectMaster*>(object.Address)->Data1.Entity;
				if (data == nullptr || data->Position.y < -500.0f)
				{
					continue;
				}
				const float ground = EggCarrierGroundHeight(data->Position.x, data->Position.z);
				if (std::fabs(data->Position.y - ground) > 0.5f)
				{
					data->Position.y = ground;
					Log("Placed an Egg Carrier decor object on the ground.");
				}
			}
		}
		else
		{
			// Station Square and Mystic Ruins decor and fauna also sit on the ground;
			// anything over empty space is sunk out of sight.
			for (const auto& object : FindObjectsByMainSub({ 0x5B4250, 0x46F720 }))
			{
				auto* data = reinterpret_cast<ObjectMaster*>(object.Address)->Data1.Entity;
				if (data == nullptr || data->Position.y < -500.0f)
				{
					continue;
				}
				float ground = 0.0f;
				if (!GardenGroundHeight(*profile, data->Position.x, data->Position.z, ground))
				{
					data->Position.y = -1000.0f;
				}
				else if (!profile->MysticFixes && data->Position.y - ground > 15.0f)
				{
					// Handled every frame by KeepStationDecorOnGround.
				}
				else if (std::fabs(data->Position.y - ground) > 0.5f)
				{
					data->Position.y = ground;
				}
			}
		}
		// A decor object floating 17 units above the Mystic floor.
		for (const auto& object : FindObjectsByMainSub({ 0x5B4250 }))
		{
			if (profile->MysticFixes
				&& std::fabs(object.Position.x - 108.45f) < 0.5f
				&& std::fabs(object.Position.z - 43.32f) < 0.5f
				&& object.Position.y > 5.0f)
			{
				// Deleting it has no effect (it is respawned or ignored), so sink it too.
				auto* master = reinterpret_cast<ObjectMaster*>(object.Address);
				if (master->Data1.Entity != nullptr)
				{
					master->Data1.Entity->Position.y = -1000.0f;
				}
				DeleteObject_(master);
				Log("Removed a floating decor object.");
			}
		}
		// Neutral collision spheres and cylinders are left over from its terrain
		// and act as invisible walls here; sink them out of the way. The door
		// trigger cube (0x6E54E0) is kept.
		for (const auto& object : FindObjectsByMainSub({ 0x6E5400, 0x6E5470 }))
		{
			auto* data = reinterpret_cast<ObjectMaster*>(object.Address)->Data1.Entity;
			if (data != nullptr && data->Position.y > -500.0f)
			{
				data->Position.y = -1000.0f;
			}
		}
		static bool spawnVisibleGate = false;
		if (spawnVisibleGate && !GardenExitDoorSpawned && frame >= 20 && MysticTextureNames[0].texaddr != 0)
		{
			GardenExitDoorSpawned = true;
			NJS_VECTOR position = { doorX, doorY, doorZ };
			SetGateTexlist(static_cast<std::uint32_t>(
				reinterpret_cast<std::uintptr_t>(&MysticTextureList)));
			auto* gate = ALO_LobbyGateNeutExecutor_Load(&position);
			if (gate != nullptr && gate->Data1.Entity != nullptr)
			{
				gate->Data1.Entity->Rotation.y = 0x4000;
				// Display-only: the trigger logic would reload the lobby.
				gate->MainSub = GardenDoorMainStub;
				Log("Spawned the lobby gate model as the garden exit door.");
			}
			else
			{
				SetGateTexlist(GateOriginalTexlist);
			}
		}
	}

	void LogCodeBytes(std::uintptr_t address, std::size_t length)
	{
		std::string text;
		char buffer[4];
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
		for (std::size_t i = 0; i < length; ++i)
		{
			std::snprintf(buffer, sizeof(buffer), "%02X", bytes[i]);
			text += buffer;
		}
		Log("CODE " + FormatAddress(address) + " " + text);
	}

	void LogChunkObject(std::uintptr_t objectAddress)
	{
		LogCodeBytes(objectAddress, 0x34);
		const auto model = *reinterpret_cast<const std::uintptr_t*>(objectAddress + 4);
		if (model < 0x400000 || model > 0x2000000)
		{
			return;
		}
		LogCodeBytes(model, 0x18);
		const auto vlist = *reinterpret_cast<const std::uintptr_t*>(model);
		const auto plist = *reinterpret_cast<const std::uintptr_t*>(model + 4);
		if (vlist >= 0x400000 && vlist < 0x2000000)
		{
			LogCodeBytes(vlist, 0x80);
		}
		if (plist >= 0x400000 && plist < 0x2000000)
		{
			LogCodeBytes(plist, 0x100);
		}
	}

	void LogGardenObjectSurvey()
	{
		LogChunkObject(0x123F02C);
		LogChunkObject(0x12562A4);
		for (const auto& object : FindObjectsByMainSub({}))
		{
			Log("Garden object main=" + FormatAddress(object.MainSub)
				+ " position=(" + std::to_string(object.Position.x) + ", "
				+ std::to_string(object.Position.y) + ", "
				+ std::to_string(object.Position.z) + ") rotation=("
				+ std::to_string(object.Angles.x) + ", "
				+ std::to_string(object.Angles.y) + ", "
				+ std::to_string(object.Angles.z) + ").");
		}
	}

	// Logs stock objects that appear or vanish while in a garden, so picking up
	// and putting down an item reveals which object type it is.
	void TrackGardenObjectChanges(unsigned int frame)
	{
		static std::map<std::uintptr_t, LobbyObjectInfo> previous;
		if (frame == 1)
		{
			previous.clear();
		}
		if (frame < 120 || frame % 5 != 0)
		{
			return;
		}
		std::map<std::uintptr_t, LobbyObjectInfo> current;
		for (const auto& object : FindObjectsByMainSub({}))
		{
			current[object.Address] = object;
		}
		if (!previous.empty())
		{
			const auto describe = [](const char* what, const LobbyObjectInfo& o)
			{
				Log(std::string("Object ") + what + " main=" + FormatAddress(o.MainSub)
					+ " position=(" + std::to_string(o.Position.x) + ", "
					+ std::to_string(o.Position.y) + ", "
					+ std::to_string(o.Position.z) + ").");
			};
			for (const auto& entry : current)
			{
				const auto it = previous.find(entry.first);
				if (it == previous.end() || it->second.MainSub != entry.second.MainSub)
				{
					describe("appeared", entry.second);
				}
			}
			for (const auto& entry : previous)
			{
				const auto it = current.find(entry.first);
				if (it == current.end() || it->second.MainSub != entry.second.MainSub)
				{
					describe("vanished", entry.second);
				}
			}
		}
		previous = std::move(current);
	}

	// Puts the camera object behind a player facing +x so it doesn't start at the stock position.
	void PlaceGardenCameraBehind(const NJS_VECTOR& spawn)
	{
		constexpr float cameraDistance = 25.0f;
		for (const auto& camera : FindObjectsByMainSub({ 0x4EB8E0 }))
		{
			auto* cameraMaster = reinterpret_cast<ObjectMaster*>(camera.Address);
			if (cameraMaster->Data1.Entity != nullptr)
			{
				auto& cameraData = *cameraMaster->Data1.Entity;
				cameraData.Position.x = spawn.x - cameraDistance;
				cameraData.Position.y = spawn.y + 5.0f;
				cameraData.Position.z = spawn.z;
				cameraData.Rotation.y = 0x4000 + 32768;
			}
		}
	}

	void ApplyLobbyReturnPosition()
	{
		if (CurrentChaoArea != 7 || NextChaoArea != 7)
		{
			LobbyReturnHandled = false;
			LobbyFramesSinceEntry = 0;
			return;
		}
		if (LobbyReturnHandled || !TestLobbyGateSpawned
			|| MainCharObj1[0] == nullptr)
		{
			return;
		}
		// Wait for the Lobby to create its own player; the previous scene's can linger.
		if (++LobbyFramesSinceEntry < 4)
		{
			return;
		}
		LobbyReturnHandled = true;
		if (LobbyReturnGateIndex < 0)
		{
			return;
		}

		// Stand in front of the gate that was used, facing the lobby centre.
		const auto& gate = LobbyGateSlots[LobbyReturnGateIndex];
		constexpr float exitDistance = 30.0f;
		auto& player = *MainCharObj1[0];
		player.Position.x = gate.x + gate.FacingX * exitDistance;
		player.Position.y = gate.y;
		player.Position.z = gate.z + gate.FacingZ * exitDistance;
		constexpr double radiansToBams = 65536.0 / (2.0 * 3.14159265358979);
		player.Rotation.y = static_cast<int>(
			std::atan2(gate.FacingX, gate.FacingZ) * radiansToBams);
		// The lobby camera object sits behind the player; move it too.
		constexpr float cameraDistance = 25.0f;
		const float forwardX = gate.FacingX;
		const float forwardZ = gate.FacingZ;
		for (const auto& camera : FindObjectsByMainSub({ 0x4EB8E0 }))
		{
			auto* cameraMaster = reinterpret_cast<ObjectMaster*>(camera.Address);
			if (cameraMaster->Data1.Entity != nullptr)
			{
				auto& cameraData = *cameraMaster->Data1.Entity;
				cameraData.Position.x = player.Position.x - forwardX * cameraDistance;
				cameraData.Position.y = player.Position.y + 5.0f;
				cameraData.Position.z = player.Position.z - forwardZ * cameraDistance;
				cameraData.Rotation.y = player.Rotation.y + 32768;
			}
		}		Log("Placed the player in front of added lobby gate "
			+ std::to_string(LobbyReturnGateIndex) + ".");
		LobbyReturnGateIndex = -1;
	}
	void LogLobbyGateObjects()
	{
		static bool logged = false;
		static int lobbyFrames = 0;
		if (CurrentChaoArea != 7 || NextChaoArea != 7)
		{
			logged = false;
			lobbyFrames = 0;
			return;
		}
		if (logged || ++lobbyFrames < 180)
		{
			return;
		}
		logged = true;
		{
			const auto* list = reinterpret_cast<const NJS_TEXLIST*>(GateOriginalTexlist);
			Log("Lobby gate texlist textures=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(list->textures))
				+ " count=" + std::to_string(list->nbTexture));
			for (unsigned int i = 0; i < list->nbTexture && i < 80
				&& list->textures != nullptr; ++i)
			{
				const char* name = reinterpret_cast<const char*>(
					list->textures[i].filename);
				Log("Lobby texlist[" + std::to_string(i) + "] "
					+ (name != nullptr ? std::string(name, strnlen(name, 40)) : "null"));
			}
		}
		const auto gates = FindObjectsByMainSub({});
		for (const auto& gate : gates)
		{
			Log("Lobby object main=" + FormatAddress(gate.MainSub)
				+ " position=(" + std::to_string(gate.Position.x) + ", "
				+ std::to_string(gate.Position.y) + ", "
				+ std::to_string(gate.Position.z) + ") rotation=("
				+ std::to_string(gate.Angles.x) + ", "
				+ std::to_string(gate.Angles.y) + ", "
				+ std::to_string(gate.Angles.z) + ").");
		}
		for (const auto& gate : FindObjectsByMainSub({ 0x57F780 }))
		{
			auto* master = reinterpret_cast<ObjectMaster*>(gate.Address);
			EntityData1 data{};
			SIZE_T read = 0;
			ReadProcessMemory(GetCurrentProcess(), master->Data1.Entity, &data,
				sizeof(data), &read);
			CollisionInfo info{};
			if (data.Collision == nullptr
				|| !ReadProcessMemory(GetCurrentProcess(), data.Collision, &info,
					sizeof(info), &read))
			{
				Log("Gate at x=" + std::to_string(gate.Position.x)
					+ " has no readable collision.");
				continue;
			}
			std::string text = "Gate x=" + std::to_string(gate.Position.x)
				+ " z=" + std::to_string(gate.Position.z)
				+ " status=" + std::to_string(data.Status)
				+ " collisions=" + std::to_string(info.Count);
			for (unsigned i = 0; i < info.Count && i < 8; ++i)
			{
				CollisionData cd{};
				ReadProcessMemory(GetCurrentProcess(), info.CollisionArray + i,
					&cd, sizeof(cd), &read);
				text += " [f0=" + std::to_string(cd.field_0)
					+ " f4=" + std::to_string(cd.field_4)
					+ " v=(" + std::to_string(cd.some_vector.x) + ","
					+ std::to_string(cd.some_vector.y) + ","
					+ std::to_string(cd.some_vector.z) + ") a="
					+ std::to_string(cd.anonymous_1) + ","
					+ std::to_string(cd.anonymous_2) + ","
					+ std::to_string(cd.anonymous_3) + " r="
					+ std::to_string(cd.field_20) + ","
					+ std::to_string(cd.field_24) + ","
					+ std::to_string(cd.field_28) + "]";
			}
			Log(text);
		}		if (MainCharObj1[0] != nullptr)
		{
			const auto& p = MainCharObj1[0]->Position;
			Log("Lobby player position=(" + std::to_string(p.x) + ", "
				+ std::to_string(p.y) + ", " + std::to_string(p.z) + ").");
		}
		Log("Lobby gate scan found " + std::to_string(gates.size())
			+ " object(s).");
	}

	void LogLiveChaoObjectCountChanges()
	{
		const int currentChaoArea = CurrentChaoArea;
		const int nextChaoArea = NextChaoArea;
		const bool isGardenOrLobby =
			(currentChaoArea >= 1 && currentChaoArea <= 3)
			|| currentChaoArea == 7
			|| (nextChaoArea >= 1 && nextChaoArea <= 3)
			|| nextChaoArea == 7;
		if (!isGardenOrLobby)
		{
			return;
		}

		const auto counts = CountChaoObjects();
		if (HasPreviousChaoObjectCounts
			&& counts.MainSubCount == PreviousChaoMainObjectCount
			&& counts.DeleteSubCount == PreviousChaoDeleteObjectCount
			&& counts.Complete == PreviousChaoObjectScanComplete)
		{
			return;
		}

		HasPreviousChaoObjectCounts = true;
		PreviousChaoMainObjectCount = counts.MainSubCount;
		PreviousChaoDeleteObjectCount = counts.DeleteSubCount;
		PreviousChaoObjectScanComplete = counts.Complete;
		Log("Live Chao object scan: Chao_Main="
			+ std::to_string(counts.MainSubCount)
			+ " Chao_Delete=" + std::to_string(counts.DeleteSubCount)
			+ (counts.Complete ? " (complete)." : " (incomplete; not conclusive)."));
	}

	void LogChaoMainConstructorPatchSite()
	{
		if (HasLoggedPatchSite)
		{
			return;
		}
		HasLoggedPatchSite = true;

		const auto* const address =
			reinterpret_cast<const void*>(ChaoMainConstructorPatchSiteAddress);
		MEMORY_BASIC_INFORMATION memoryInfo{};
		if (VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) != sizeof(memoryInfo)
			|| memoryInfo.State != MEM_COMMIT
			|| (memoryInfo.Protect & PAGE_GUARD) != 0)
		{
			Log("Could not inspect the Chao main constructor patch site.");
			return;
		}

		const DWORD pageProtection = memoryInfo.Protect & 0xFF;
		if (pageProtection != PAGE_READONLY
			&& pageProtection != PAGE_READWRITE
			&& pageProtection != PAGE_WRITECOPY
			&& pageProtection != PAGE_EXECUTE_READ
			&& pageProtection != PAGE_EXECUTE_READWRITE
			&& pageProtection != PAGE_EXECUTE_WRITECOPY)
		{
			Log("The Chao main constructor patch site is not readable.");
			return;
		}

		const auto regionStart =
			reinterpret_cast<std::uintptr_t>(memoryInfo.BaseAddress);
		const auto regionSize = static_cast<std::size_t>(memoryInfo.RegionSize);
		if (ChaoMainConstructorPatchSiteAddress < regionStart
			|| ChaoMainConstructorPatchSiteAddress - regionStart > regionSize
			|| ReferencePatchSiteSignature.size()
				> regionSize - static_cast<std::size_t>(
					ChaoMainConstructorPatchSiteAddress - regionStart))
		{
			Log("The Chao main constructor patch site crosses an unreadable memory region.");
			return;
		}

		std::array<std::uint8_t, ReferencePatchSiteSignature.size()> bytes{};
		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(
			GetCurrentProcess(),
			address,
			bytes.data(),
			bytes.size(),
			&bytesRead)
			|| bytesRead != bytes.size())
		{
			Log("Could not read the Chao main constructor patch site.");
			return;
		}

		static constexpr char HexDigits[] = "0123456789ABCDEF";
		std::string message = "Chao main constructor code at 0x52B370:";
		for (const auto byte : bytes)
		{
			message += " ";
			message += HexDigits[byte >> 4];
			message += HexDigits[byte & 0x0F];
		}
		if (bytes == ReferencePatchSiteSignature)
		{
			message += " (matches the Extra Chao Garden 1.0.6 pre-hook signature; read-only probe)";
		}
		Log(message);
	}

	bool IsExecutableAddress(std::uintptr_t address)
	{
		MEMORY_BASIC_INFORMATION memoryInfo{};
		if (VirtualQuery(
			reinterpret_cast<const void*>(address),
			&memoryInfo,
			sizeof(memoryInfo)) != sizeof(memoryInfo)
			|| memoryInfo.State != MEM_COMMIT
			|| (memoryInfo.Protect & PAGE_GUARD) != 0)
		{
			return false;
		}

		const DWORD pageProtection = memoryInfo.Protect & 0xFF;
		return pageProtection == PAGE_EXECUTE
			|| pageProtection == PAGE_EXECUTE_READ
			|| pageProtection == PAGE_EXECUTE_READWRITE
			|| pageProtection == PAGE_EXECUTE_WRITECOPY;
	}

	bool ReadRelativeCallTarget(
		std::uintptr_t callSite,
		std::uintptr_t& target)
	{
		std::array<std::uint8_t, 5> instruction{};
		SIZE_T bytesRead = 0;
		if (!ReadProcessMemory(
			GetCurrentProcess(),
			reinterpret_cast<const void*>(callSite),
			instruction.data(),
			instruction.size(),
			&bytesRead)
			|| bytesRead != instruction.size()
			|| instruction[0] != 0xE8)
		{
			return false;
		}

		std::int32_t displacement = 0;
		std::memcpy(&displacement, instruction.data() + 1, sizeof(displacement));
		target = static_cast<std::uintptr_t>(
			static_cast<std::uint32_t>(callSite + instruction.size())
			+ static_cast<std::uint32_t>(displacement));
		return IsExecutableAddress(target);
	}

	std::string ReadPlayerGroundQueryMemory(
		std::uintptr_t address,
		std::size_t size)
	{
		std::array<std::uint8_t, 0x20> bytes{};
		SIZE_T bytesRead = 0;
		if (address < 0x10000
			|| size > bytes.size()
			|| ReadProcessMemory(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(address),
				bytes.data(),
				size,
				&bytesRead) == FALSE
			|| bytesRead != size)
		{
			return "<unreadable>";
		}

		constexpr char HexDigits[] = "0123456789ABCDEF";
		std::string hex;
		hex.reserve(size * 2);
		for (std::size_t index = 0; index < size; ++index)
		{
			hex.push_back(HexDigits[bytes[index] >> 4]);
			hex.push_back(HexDigits[bytes[index] & 0x0F]);
		}
		return hex;
	}

	int __cdecl BeginPlayerGroundQueryObservation(
		std::uintptr_t eaxValue,
		std::uintptr_t argument1,
		std::uintptr_t argument2,
		std::uintptr_t argument3,
		std::uintptr_t callSite)
	{
		if (!NeutralLandSwapped || CurrentChaoArea != garden_pools::NeutralGardenId)
		{
			return 0;
		}

		std::size_t siteIndex = 0;
		while (siteIndex < PlayerGroundQueryCallSites.size()
			&& PlayerGroundQueryCallSites[siteIndex] != callSite)
		{
			++siteIndex;
		}
		if (siteIndex == PlayerGroundQueryCallSites.size())
		{
			return 0;
		}

		EntityData1 player{};
		if (MainCharObj1[0] == nullptr
			|| ReadProcessMemory(
				GetCurrentProcess(),
				MainCharObj1[0],
				&player,
				sizeof(player),
				nullptr) == FALSE)
		{
			return 0;
		}

		const auto playerAction = static_cast<unsigned int>(
			static_cast<unsigned char>(player.Action));
		if (playerAction != 1 && playerAction != 6 && playerAction != 10)
		{
			return 0;
		}

		const auto playerState = static_cast<LONG>(
			(playerAction << 1)
			| ((player.Status & Status_Ground) != 0 ? 1U : 0U));
		const auto frame = static_cast<LONG>(GardenEnvironmentDiagnosticFrame);
		const auto previousState = InterlockedCompareExchange(
			&PlayerGroundQueryPreviousStates[siteIndex],
			0,
			0);
		const auto previousFrame = InterlockedCompareExchange(
			&PlayerGroundQueryLastLoggedFrames[siteIndex],
			0,
			0);
		if (frame == previousFrame
			|| (frame % 15 != 0 && playerState == previousState))
		{
			return 0;
		}
		if (InterlockedCompareExchange(
				&PlayerGroundQueryLastLoggedFrames[siteIndex],
				frame,
				previousFrame) != previousFrame)
		{
			return 0;
		}
		InterlockedExchange(
			&PlayerGroundQueryPreviousStates[siteIndex],
			playerState);

		const auto sample = InterlockedIncrement(&PlayerGroundQueryObservations);
		if (sample > 64)
		{
			return 0;
		}

		Log("Player ground-query input sample=" + std::to_string(sample)
			+ " callSite=" + FormatAddress(callSite)
			+ " frame=" + std::to_string(frame)
			+ " playerAction=" + std::to_string(playerAction)
			+ " playerGrounded="
			+ ((player.Status & Status_Ground) != 0 ? "1" : "0")
			+ " playerPosition=(" + std::to_string(player.Position.x) + ", "
			+ std::to_string(player.Position.y) + ", "
			+ std::to_string(player.Position.z) + ")"
			+ " eax=" + FormatAddress(eaxValue)
			+ " arg1=" + FormatAddress(argument1)
			+ " arg1Bytes=" + ReadPlayerGroundQueryMemory(argument1, 0x10)
			+ " arg2=" + FormatAddress(argument2)
			+ " arg2Bytes=" + ReadPlayerGroundQueryMemory(argument2, 0x10)
			+ " arg3=" + FormatAddress(argument3)
			+ " arg3Bytes=" + ReadPlayerGroundQueryMemory(argument3, 0x10)
			+ ".");
		return sample;
	}

	void __cdecl EndPlayerGroundQueryObservation(
		int sample,
		std::uintptr_t argument1,
		std::uintptr_t argument2,
		std::uintptr_t argument3,
		int result)
	{
		if (sample == 0)
		{
			return;
		}

		Log("Player ground-query result sample=" + std::to_string(sample)
			+ " result=" + std::to_string(result)
			+ " arg1After=" + ReadPlayerGroundQueryMemory(argument1, 4)
			+ " arg2After=" + ReadPlayerGroundQueryMemory(argument2, 4)
			+ " arg3After=" + ReadPlayerGroundQueryMemory(argument3, 4)
			+ ".");
	}

	__declspec(naked) int __cdecl PlayerGroundQueryObserver()
	{
		__asm
		{
			push ebp
			mov ebp, esp
			sub esp, 18h
			mov dword ptr [ebp - 4], eax
			mov dword ptr [ebp - 8], ecx
			mov dword ptr [ebp - 0Ch], edx

			pushfd
			pushad
			mov eax, dword ptr [ebp + 04h]
			sub eax, 5
			push eax
			push dword ptr [ebp + 10h]
			push dword ptr [ebp + 0Ch]
			push dword ptr [ebp + 08h]
			push dword ptr [ebp - 4]
			call BeginPlayerGroundQueryObservation
			add esp, 14h
			mov dword ptr [ebp - 10h], eax
			popad
			popfd

			push dword ptr [ebp + 10h]
			push dword ptr [ebp + 0Ch]
			push dword ptr [ebp + 08h]
			mov eax, dword ptr [ebp - 4]
			mov ecx, dword ptr [ebp - 8]
			mov edx, dword ptr [ebp - 0Ch]
			call dword ptr [PreviousPlayerGroundQueryTarget]
			mov dword ptr [ebp - 14h], eax
			pushfd
			pop dword ptr [ebp - 18h]
			add esp, 0Ch

			pushad
			push dword ptr [ebp - 14h]
			push dword ptr [ebp + 10h]
			push dword ptr [ebp + 0Ch]
			push dword ptr [ebp + 08h]
			push dword ptr [ebp - 10h]
			push dword ptr [ebp - 18h]
			popfd
			call EndPlayerGroundQueryObservation
			add esp, 14h
			popad
			push dword ptr [ebp - 18h]
			popfd

			mov esp, ebp
			pop ebp
			ret
		}
	}

	void TryInstallPlayerGroundQueryObserver()
	{
		if (PlayerGroundQueryObserverAttempted)
		{
			return;
		}
		PlayerGroundQueryObserverAttempted = true;

		std::array<std::uintptr_t, PlayerGroundQueryCallSites.size()> targets{};
		for (std::size_t index = 0; index < PlayerGroundQueryCallSites.size(); ++index)
		{
			if (!ReadRelativeCallTarget(PlayerGroundQueryCallSites[index], targets[index])
				|| targets[index] != PlayerGroundQueryRoutineAddress)
			{
				Log("Player ground-query observer was not installed: call site "
					+ FormatAddress(PlayerGroundQueryCallSites[index])
					+ " did not resolve to the validated query routine.");
				return;
			}
		}

		PreviousPlayerGroundQueryTarget = reinterpret_cast<void*>(
			PlayerGroundQueryRoutineAddress);
		for (std::size_t index = 0; index < PlayerGroundQueryCallSites.size(); ++index)
		{
			const auto callSite = PlayerGroundQueryCallSites[index];
			const bool writeSucceeded = WriteCall(
				reinterpret_cast<void*>(callSite),
				reinterpret_cast<void*>(&PlayerGroundQueryObserver));
			const bool flushSucceeded = writeSucceeded
				&& FlushInstructionCache(
					GetCurrentProcess(),
					reinterpret_cast<const void*>(callSite),
					5);
			if (!writeSucceeded || !flushSucceeded)
			{
				const auto error = GetLastError();
				bool rollbackSucceeded = true;
				for (std::size_t rollbackIndex = 0;
					rollbackIndex <= index;
					++rollbackIndex)
				{
					const auto rollbackCallSite =
						PlayerGroundQueryCallSites[rollbackIndex];
					const bool rollbackWriteSucceeded = WriteCall(
						reinterpret_cast<void*>(rollbackCallSite),
						PreviousPlayerGroundQueryTarget);
					const bool rollbackFlushSucceeded = rollbackWriteSucceeded
						&& FlushInstructionCache(
							GetCurrentProcess(),
							reinterpret_cast<const void*>(rollbackCallSite),
							5);
					rollbackSucceeded = rollbackSucceeded
						&& rollbackWriteSucceeded
						&& rollbackFlushSucceeded;
				}
				Log("Could not install the player ground-query observer at "
					+ FormatAddress(callSite)
					+ " (Win32 error " + std::to_string(error) + ")."
					+ (rollbackSucceeded
						? " The original call targets were restored."
						: " Hook rollback failed; inspect the game before continuing."));
				return;
			}
		}

		Log("Installed the player ground-query observer at five validated "
			"call sites. It samples each call site independently, records raw "
			"register/stack arguments, and preserves registers while forwarding "
			"the original routine unchanged.");
	}

	void TryInstallChaoSaveWriteHooks()
	{
		if (ChaoSaveWriteHooksAttempted || ChaoSaveWriteHooksInstalled)
		{
			return;
		}

		std::array<std::uintptr_t, ChaoSaveWriteCallSites.size()> targets{};
		for (std::size_t index = 0; index < ChaoSaveWriteCallSites.size(); ++index)
		{
			if (!ReadRelativeCallTarget(ChaoSaveWriteCallSites[index], targets[index]))
			{
				ChaoSaveWriteHooksAttempted = true;
				Log("Chao save-write hooks were not installed: a known save call site "
					"was unreadable, not a relative CALL, or did not target executable code.");
				return;
			}
		}

		for (const auto target : targets)
		{
			std::uint8_t firstOpcode = 0;
			SIZE_T bytesRead = 0;
			if (!ReadProcessMemory(
					GetCurrentProcess(),
					reinterpret_cast<const void*>(target),
					&firstOpcode,
					sizeof(firstOpcode),
					&bytesRead)
					|| bytesRead != sizeof(firstOpcode))
			{
					ChaoSaveWriteHooksAttempted = true;
					Log("Chao save-write hooks were not installed: an original target "
						"was executable but its entry byte could not be read.");
					return;
			}
			if (firstOpcode == 0xC2 || firstOpcode == 0xC3)
			{
					if (!ChaoSaveWriteHookDeferralLogged)
					{
						Log("Deferring Chao save-write hook installation because "
							+ FormatAddress(target)
							+ " currently begins with an immediate RET; retrying after "
							"other save wrappers have initialized.");
						ChaoSaveWriteHookDeferralLogged = true;
					}
					return;
			}
		}

		ChaoSaveWriteHooksAttempted = true;
		for (std::size_t index = 0; index < targets.size(); ++index)
		{
			Log("Captured original Chao save writer target "
				+ std::to_string(index + 1) + ": "
				+ FormatAddress(targets[index]) + ".");
		}

		const std::array<void*, ChaoSaveWriteCallSites.size()> hooks{
			reinterpret_cast<void*>(&SaveChaoFileHook0),
			reinterpret_cast<void*>(&SaveChaoFileHook1),
			reinterpret_cast<void*>(&SaveChaoFileHook2)
		};
		for (std::size_t index = 0; index < targets.size(); ++index)
		{
			PreviousChaoSaveWriteTargets[index] =
				reinterpret_cast<void*>(targets[index]);
		}

		std::size_t patchedCount = 0;
		for (; patchedCount < ChaoSaveWriteCallSites.size(); ++patchedCount)
		{
			const auto callSite = ChaoSaveWriteCallSites[patchedCount];
			const bool writeSucceeded = WriteCall(
				reinterpret_cast<void*>(callSite),
				hooks[patchedCount]);
			const bool flushSucceeded = writeSucceeded
				&& FlushInstructionCache(
					GetCurrentProcess(),
					reinterpret_cast<const void*>(callSite),
					5);
			if (!writeSucceeded || !flushSucceeded)
			{
				const auto error = GetLastError();
				bool rollbackSucceeded = true;
				std::size_t rollbackCount = patchedCount + 1;
				while (rollbackCount > 0)
				{
					--rollbackCount;
					const auto previousSite = ChaoSaveWriteCallSites[rollbackCount];
					if (!WriteCall(
						reinterpret_cast<void*>(previousSite),
						PreviousChaoSaveWriteTargets[rollbackCount])
						|| !FlushInstructionCache(
							GetCurrentProcess(),
							reinterpret_cast<const void*>(previousSite),
							5))
					{
						rollbackSucceeded = false;
					}
				}

				Log("Could not install the validated Chao save-write hooks (Win32 error "
					+ std::to_string(error) + ")."
					+ (rollbackSucceeded
						? " Attempted call sites were restored."
						: " Hook rollback failed; inspect the game before continuing."));
				return;
			}
		}

		Log("Installed three Chao save-write observers and chained each site's "
			"existing target; the opt-in test overlay can protect its stock block.");
		ChaoSaveWriteHooksInstalled = true;
	}

	void TryInstallNeutralGardenSpawnObserver()
	{
		if (NeutralGardenSpawnObserverAttempted)
		{
			return;
		}
		NeutralGardenSpawnObserverAttempted = true;

		std::uintptr_t target = 0;
		if (!ReadRelativeCallTarget(NeutralGardenSpawnCallSiteAddress, target)
			|| target != SpawnAllChaoInGardenAddress)
		{
			Log("Neutral garden spawn observer was not installed: the call site did "
				"not resolve to the validated SpawnAllChaoInGarden target.");
			return;
		}

		PreviousNeutralGardenSpawnTarget = reinterpret_cast<void*>(target);
		const bool writeSucceeded = WriteCall(
			reinterpret_cast<void*>(NeutralGardenSpawnCallSiteAddress),
			reinterpret_cast<void*>(&NeutralGardenSpawnObserver));
		const bool flushSucceeded = writeSucceeded
			&& FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(NeutralGardenSpawnCallSiteAddress),
				5);
		if (!writeSucceeded || !flushSucceeded)
		{
			const auto error = GetLastError();
			const bool rollbackSucceeded = WriteCall(
				reinterpret_cast<void*>(NeutralGardenSpawnCallSiteAddress),
				PreviousNeutralGardenSpawnTarget)
				&& FlushInstructionCache(
					GetCurrentProcess(),
					reinterpret_cast<const void*>(NeutralGardenSpawnCallSiteAddress),
					5);
			Log("Could not install the Neutral garden spawn observer (Win32 error "
				+ std::to_string(error) + ")."
				+ (rollbackSucceeded
					? " The original call target was restored."
					: " Hook rollback failed; inspect the game before continuing."));
			return;
		}

		Log("Installed an observer at Neutral garden's pre-spawn call; the "
			"opt-in overlay test is disabled unless its marker file exists.");
	}

	void TryInstallPreviousGardenStageRequestObservers()
	{
		if (PreviousGardenStageRequestObserversAttempted)
		{
			return;
		}
		PreviousGardenStageRequestObserversAttempted = true;

		std::array<std::uintptr_t, PreviousGardenStageRequestCallSites.size()> targets{};
		for (std::size_t index = 0;
			index < PreviousGardenStageRequestCallSites.size();
			++index)
		{
			if (!ReadRelativeCallTarget(
			PreviousGardenStageRequestCallSites[index],
			targets[index])
			|| targets[index] != ChaoStageRequestAddress)
			{
			Log("Previous-garden stage-request observers were not installed: "
				"a candidate call site did not resolve to the validated "
				"Chao stage-request function.");
			return;
			}
		}

		const std::array<void*, PreviousGardenStageRequestCallSites.size()> hooks{
			reinterpret_cast<void*>(&PreviousGardenStageRequestObserver0),
			reinterpret_cast<void*>(&PreviousGardenStageRequestObserver1),
			reinterpret_cast<void*>(&PreviousGardenStageRequestObserver2),
			reinterpret_cast<void*>(&PreviousGardenStageRequestObserver3)
		};
		for (std::size_t index = 0; index < targets.size(); ++index)
		{
			PreviousGardenStageRequestTargets[index] =
				reinterpret_cast<void*>(targets[index]);
		}

		std::size_t patchedCount = 0;
		for (; patchedCount < PreviousGardenStageRequestCallSites.size(); ++patchedCount)
		{
			const auto callSite = PreviousGardenStageRequestCallSites[patchedCount];
			const bool writeSucceeded = WriteCall(
				reinterpret_cast<void*>(callSite),
				hooks[patchedCount]);
			const bool flushSucceeded = writeSucceeded
				&& FlushInstructionCache(
					GetCurrentProcess(),
					reinterpret_cast<const void*>(callSite),
					5);
			if (!writeSucceeded || !flushSucceeded)
			{
				const auto error = GetLastError();
				bool rollbackSucceeded = true;
				std::size_t rollbackCount = patchedCount + 1;
				while (rollbackCount > 0)
				{
					--rollbackCount;
					const auto previousSite =
						PreviousGardenStageRequestCallSites[rollbackCount];
					if (!WriteCall(
						reinterpret_cast<void*>(previousSite),
						PreviousGardenStageRequestTargets[rollbackCount])
						|| !FlushInstructionCache(
							GetCurrentProcess(),
							reinterpret_cast<const void*>(previousSite),
							5))
					{
						rollbackSucceeded = false;
					}
				}

				Log("Could not install previous-garden stage-request observers "
					"(Win32 error " + std::to_string(error) + ")."
					+ (rollbackSucceeded
						? " Attempted call sites were restored."
						: " Hook rollback failed; inspect the game before continuing."));
				return;
			}
		}

		Log("Installed four pass-through observers for stage requests sourced "
			"from LastChaoArea; pool data remains untouched.");
	}

	void TryInstallOtherGardenStageRequestObservers()
	{
		if (OtherGardenStageRequestObserversAttempted)
		{
			return;
		}
		OtherGardenStageRequestObserversAttempted = true;

		std::array<std::uintptr_t, OtherGardenStageRequestCallSites.size()> targets{};
		for (std::size_t index = 0;
			index < OtherGardenStageRequestCallSites.size();
			++index)
		{
			if (!ReadRelativeCallTarget(
				OtherGardenStageRequestCallSites[index],
				targets[index])
				|| targets[index] != ChaoStageRequestAddress)
			{
				Log("Additional stage-request observers were not installed: "
					"a candidate call site did not resolve to the validated "
					"Chao stage-request function.");
				return;
			}
		}

		for (std::size_t index = 0; index < targets.size(); ++index)
		{
			OtherGardenStageRequestTargets[index] =
				reinterpret_cast<void*>(targets[index]);
		}

		std::size_t patchedCount = 0;
		for (; patchedCount < OtherGardenStageRequestCallSites.size(); ++patchedCount)
		{
			const auto callSite = OtherGardenStageRequestCallSites[patchedCount];
			const bool writeSucceeded = WriteCall(
				reinterpret_cast<void*>(callSite),
				reinterpret_cast<void*>(&OtherGardenStageRequestObserver));
			const bool flushSucceeded = writeSucceeded
				&& FlushInstructionCache(
					GetCurrentProcess(),
					reinterpret_cast<const void*>(callSite),
					5);
			if (!writeSucceeded || !flushSucceeded)
			{
				const auto error = GetLastError();
				bool rollbackSucceeded = true;
				std::size_t rollbackCount = patchedCount + 1;
				while (rollbackCount > 0)
				{
					--rollbackCount;
					const auto previousSite =
						OtherGardenStageRequestCallSites[rollbackCount];
					if (!WriteCall(
						reinterpret_cast<void*>(previousSite),
						OtherGardenStageRequestTargets[rollbackCount])
						|| !FlushInstructionCache(
							GetCurrentProcess(),
							reinterpret_cast<const void*>(previousSite),
							5))
					{
						rollbackSucceeded = false;
					}
				}

				Log("Could not install additional stage-request observers "
					"(Win32 error " + std::to_string(error) + ")."
					+ (rollbackSucceeded
						? " Attempted call sites were restored."
						: " Hook rollback failed; inspect the game before continuing."));
				return;
			}
		}

		Log("Installed fourteen additional pass-through observers for calls to "
			"the Chao stage-request function; stage and pool data remain unchanged.");
	}

	void TryInstallChaoStageActivationObserver()
	{
		if (ChaoStageActivationObserverAttempted)
		{
			return;
		}
		ChaoStageActivationObserverAttempted = true;

		std::uintptr_t target = 0;
		if (!ReadRelativeCallTarget(
			ChaoStageActivationCallSiteAddress,
			target)
			|| target != ChaoStageActivationRoutineAddress)
		{
			Log("Chao stage-activation observer was not installed: the call site "
				"did not resolve to the validated stage-activation routine.");
			return;
		}

		PreviousChaoStageActivationTarget = reinterpret_cast<void*>(target);
		const bool writeSucceeded = WriteCall(
			reinterpret_cast<void*>(ChaoStageActivationCallSiteAddress),
			reinterpret_cast<void*>(&ChaoStageActivationObserver));
		const bool flushSucceeded = writeSucceeded
			&& FlushInstructionCache(
				GetCurrentProcess(),
				reinterpret_cast<const void*>(ChaoStageActivationCallSiteAddress),
				5);
		if (!writeSucceeded || !flushSucceeded)
		{
			const auto error = GetLastError();
			const bool rollbackSucceeded = WriteCall(
				reinterpret_cast<void*>(ChaoStageActivationCallSiteAddress),
				PreviousChaoStageActivationTarget)
				&& FlushInstructionCache(
					GetCurrentProcess(),
					reinterpret_cast<const void*>(ChaoStageActivationCallSiteAddress),
					5);
			Log("Could not install the Chao stage-activation observer "
				"(Win32 error " + std::to_string(error) + ")."
				+ (rollbackSucceeded
					? " The original call target was restored."
					: " Hook rollback failed; inspect the game before continuing."));
			return;
		}

		Log("Installed a pass-through observer after the Chao stage-activation "
			"routine; it will report stage values before and after the routine.");
	}

	int __cdecl NeutralGardenSpawnObserver()
	{
		if (InterlockedCompareExchange(
			&NeutralGardenSpawnObserverLogged,
			1,
			0) == 0)
		{
			Log("Neutral garden pre-spawn call reached.");
		}

		if (PreviousNeutralGardenSpawnTarget == nullptr)
		{
			Log("Neutral garden spawn observer has no original target.");
			return -1;
		}

		const auto overlayResult = TryBeginTestPoolOverlay();
		if (overlayResult == TestPoolOverlayResult::Unsafe)
		{
			Log("Neutral Chao spawning was stopped because the opt-in test overlay "
				"could not be safely applied or restored.");
			return -1;
		}

		using SpawnAllChaoInGardenFunction = int(__cdecl*)();
		const auto original = reinterpret_cast<SpawnAllChaoInGardenFunction>(
			PreviousNeutralGardenSpawnTarget);
		return original();
	}

	int __cdecl PreviousGardenStageRequestObserverWorker(
		std::size_t hookIndex,
		int destination)
	{
		const int currentChaoArea = CurrentChaoArea;
		if (destination == 7
			&& currentChaoArea >= 1
			&& currentChaoArea <= 3
			&& InterlockedCompareExchange(
				&PreviousGardenStageRequestLogged[hookIndex],
				1,
				0) == 0)
		{
			Log("Previous-garden return candidate at call site "
				+ FormatAddress(PreviousGardenStageRequestCallSites[hookIndex])
				+ ": current=" + std::to_string(currentChaoArea)
				+ " destination=" + std::to_string(destination) + ".");
		}

		if (PreviousGardenStageRequestTargets[hookIndex] == nullptr)
		{
			Log("Previous-garden stage-request observer has no original target.");
			return -1;
		}

		using ChaoStageRequestFunction = int(__cdecl*)(int);
		const auto original = reinterpret_cast<ChaoStageRequestFunction>(
			PreviousGardenStageRequestTargets[hookIndex]);
		return original(destination);
	}

	int __cdecl PreviousGardenStageRequestObserver0(int destination)
	{
		return PreviousGardenStageRequestObserverWorker(0, destination);
	}

	int __cdecl PreviousGardenStageRequestObserver1(int destination)
	{
		return PreviousGardenStageRequestObserverWorker(1, destination);
	}

	int __cdecl PreviousGardenStageRequestObserver2(int destination)
	{
		return PreviousGardenStageRequestObserverWorker(2, destination);
	}

	int __cdecl PreviousGardenStageRequestObserver3(int destination)
	{
		return PreviousGardenStageRequestObserverWorker(3, destination);
	}

	int __declspec(noinline) __cdecl OtherGardenStageRequestObserver(int destination)
	{
		const int currentChaoArea = CurrentChaoArea;
		if (destination == garden_pools::NeutralGardenId
			&& currentChaoArea == 7
			&& TestLobbyGateSpawned
			&& LobbyGateNearby
			&& LobbyReturnGateIndex >= 0
			&& static_cast<std::size_t>(LobbyReturnGateIndex)
				< garden_pools::AddedGardens.size()
			&& TestPoolOverlayRequested())
		{
			SelectedAddedGarden = garden_pools::AddedGardens[
				static_cast<std::size_t>(LobbyReturnGateIndex)];
			const auto* stageName =
				garden_pools::SADXStageName(SelectedAddedGarden);
			Log("Preparing the " + std::string(stageName)
				+ " environment before the Neutral stage load.");
			ApplyGardenEnvironment(stageName);
		}
		if (destination == 7
			&& currentChaoArea >= 1
			&& currentChaoArea <= 3
			&& InterlockedCompareExchange(
				&OtherGardenStageRequestLogged,
				1,
				0) == 0)
		{
			const auto returnAddress =
				reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			const auto logCallSite = returnAddress >= 5 ? returnAddress - 5 : 0;
			Log("Garden-to-Lobby stage request observed at call site "
				+ FormatAddress(logCallSite)
				+ ": current=" + std::to_string(currentChaoArea)
				+ " destination=" + std::to_string(destination) + ".");
		}

		using ChaoStageRequestFunction = int(__cdecl*)(int);
		const auto original = reinterpret_cast<ChaoStageRequestFunction>(
			ChaoStageRequestAddress);
		return original(destination);
	}

	void __cdecl ChaoStageActivationObserver()
	{
		const int currentBefore = CurrentChaoArea;
		const int nextBefore = NextChaoArea;
		const bool isGardenLobbyTransition =
			(currentBefore == 7
				&& nextBefore == garden_pools::NeutralGardenId)
			|| (currentBefore == garden_pools::NeutralGardenId
				&& nextBefore == 7);
		const auto chaoObjectsBefore = CountChaoObjects();

		if (isGardenLobbyTransition)
		{
			LogGardenLandManagerState("before stage activation");
		}

		using ChaoStageActivationFunction = void(__cdecl*)();
		const auto original = reinterpret_cast<ChaoStageActivationFunction>(
			PreviousChaoStageActivationTarget);
		if (original == nullptr)
		{
			Log("Chao stage-activation observer has no original target.");
			return;
		}
		original();
		const int currentAfter = CurrentChaoArea;
		const int nextAfter = NextChaoArea;
		const auto chaoObjectsAfter = CountChaoObjects();
		if (isGardenLobbyTransition)
		{
			LogGardenLandManagerState("after stage activation");
		}
		if (currentBefore == garden_pools::NeutralGardenId
			&& currentAfter == 7
			&& nextBefore == 7)
		{
			RestoreGardenEnvironment();
			LogGardenLandManagerState("after environment restore attempt");
			TryRestoreTestPoolOverlayAfterReturn(chaoObjectsAfter);
		}
		if (currentBefore != currentAfter || nextBefore != nextAfter)
		{
			Log("Chao stage-activation routine returned: current="
				+ std::to_string(currentBefore)
				+ " next=" + std::to_string(nextBefore)
				+ " -> current=" + std::to_string(currentAfter)
				+ " next=" + std::to_string(nextAfter)
				+ "; live Chao objects (Chao_Main/Chao_Delete)="
				+ std::to_string(chaoObjectsBefore.MainSubCount)
				+ "/" + std::to_string(chaoObjectsBefore.DeleteSubCount)
				+ " -> " + std::to_string(chaoObjectsAfter.MainSubCount)
				+ "/" + std::to_string(chaoObjectsAfter.DeleteSubCount)
				+ (chaoObjectsBefore.Complete && chaoObjectsAfter.Complete
					? "."
					: " (one or more object-list scans were incomplete)."));
		}
	}

	void LogGardenLandManagerState(const char* phase)
	{
		const auto* const neutralTable = NeutralLandExport();
		LandTable neutralSnapshot{};
		const bool hasNeutralTable = neutralTable != nullptr
			&& ReadProcessMemory(
				GetCurrentProcess(),
				neutralTable,
				&neutralSnapshot,
				sizeof(neutralSnapshot),
				nullptr) != FALSE;
		ObjectMaster managerSnapshot{};
		const auto* const manager = LandManagerPtr;
		const bool hasManager = manager != nullptr
			&& ReadProcessMemory(
				GetCurrentProcess(),
				manager,
				&managerSnapshot,
				sizeof(managerSnapshot),
				nullptr) != FALSE;

		std::string message = "Garden land-manager "
			+ std::string(phase)
			+ ": current=" + std::to_string(CurrentChaoArea)
			+ " next=" + std::to_string(NextChaoArea)
			+ " CurrentLandTable=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(CurrentLandTable))
			+ " NeutralExport=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(neutralTable))
			+ " LandManager=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(manager))
			+ " SA2BModelMode=" + std::to_string(LandTableSA2BModels);
		if (hasNeutralTable)
		{
			message += " NeutralCOLs="
				+ std::to_string(neutralSnapshot.COLCount)
				+ " NeutralGC_COLs="
				+ std::to_string(neutralSnapshot.ChunkModelCount);
		}
		else
		{
			message += " NeutralExport=<unreadable>";
		}
		if (hasManager)
		{
			message += " ManagerMainSub=" + FormatAddress(
				reinterpret_cast<std::uintptr_t>(managerSnapshot.MainSub))
				+ " ManagerDeleteSub=" + FormatAddress(
					reinterpret_cast<std::uintptr_t>(managerSnapshot.DeleteSub))
				+ " ManagerData1=" + FormatAddress(
					reinterpret_cast<std::uintptr_t>(
						managerSnapshot.Data1.Undefined))
				+ " ManagerSETData=" + FormatAddress(
					reinterpret_cast<std::uintptr_t>(managerSnapshot.SETData));
		}
		else
		{
			message += " ManagerState=<unreadable>";
		}
		Log(message + ".");
	}

	int CallPreviousChaoSaveWrite(
		void* target,
		char* path,
		void* buffer,
		std::size_t count)
	{
		int result = 0;
		__asm
		{
			push[count]
			mov edx, [buffer]
			mov ecx, [path]
			call dword ptr[target]
			add esp, 4
			mov[result], eax
		}
		return result;
	}

	int __cdecl SaveChaoFileHookWorker(
		std::size_t hookIndex,
		char* path,
		void* buffer,
		std::size_t count)
	{
		int result = 0;
		const auto pathLength = path == nullptr
			? std::size_t{ 0 }
			: strnlen_s(path, MAX_PATH);
		const bool pathIsBounded = path != nullptr && pathLength < MAX_PATH;
		const std::string_view writerPath = pathIsBounded
			? std::string_view(path, pathLength)
			: std::string_view{};
		if (TestPoolOverlayProtectsSave)
		{
			if (!garden_pools::ChaoSavePathMatches(
				TestPoolChaoSaveFilename,
				writerPath))
			{
				result = -1;
				Log("Rejected a Chao save write during the opt-in test overlay "
					"because its path did not match the active save identity.");
			}
			else
			{
				try
				{
					if (TestPoolOverlaySession != nullptr
						&& TestPoolOverlaySession->IsActive())
					{
						TestPoolOverlaySession->CheckpointItems(
							ReadChaoSaveItemsFromMemory(),
							HeldGardenItemSlotOffset());
						const auto checkpoint =
							TestPoolOverlaySession->Checkpoint(
								ExcludeHeldChaoFromCapture(
									ReadChaoSaveRecordsFromMemory(),
									HeldChaoRecordSlot()));
						TestPoolCheckpointFailureLogged = false;
						Log("Checkpointed " + std::to_string(checkpoint.size())
							+ " " + garden_pools::SADXStageName(ActiveTestGarden)
							+ " Chao to the save-scoped sidecar before the game save.");
					}
				}
				catch (const std::exception& error)
				{
					if (!TestPoolCheckpointFailureLogged)
					{
						TestPoolCheckpointFailureLogged = true;
						Log("Could not checkpoint the added-garden pool before "
							"the game save; continuing with the protected vanilla "
							"save block: " + std::string(error.what()));
					}
				}

				try
				{
					if (TestPoolVanillaSaveBlock.size()
						!= garden_pools::ChaoSaveBlockSize)
					{
						throw std::runtime_error(
							"The original Chao save block is unavailable.");
					}
					garden_pools::ChaoSaveWriteBufferGuard saveGuard(
						buffer,
						count,
						TestPoolVanillaSaveBlock);
					result = CallPreviousChaoSaveWrite(
						PreviousChaoSaveWriteTargets[hookIndex],
						path,
						buffer,
						count);
					Log("Protected the stock Chao save block from the opt-in "
						"in-memory test overlay.");
				}
				catch (const std::exception& error)
				{
					Log("Rejected a matching Chao save write during the opt-in "
						"test overlay because the stock save block could not be "
						"protected: " + std::string(error.what()));
					return -1;
				}
			}
		}
		else
		{
			result = CallPreviousChaoSaveWrite(
				PreviousChaoSaveWriteTargets[hookIndex],
				path,
				buffer,
				count);
		}

		try
		{
			const std::string pathText = path == nullptr
				? "(null)"
				: pathIsBounded
					? std::string(path, pathLength)
					: "(unterminated)";
			Log("Observed Chao save write call " + std::to_string(hookIndex + 1)
				+ ": path=" + pathText
				+ " size=" + std::to_string(count)
				+ " result=" + std::to_string(result) + ".");
		}
		catch (const std::exception& error)
		{
			OutputDebugStringA(
				("[SA2 Six Garden Expansion] Could not format Chao save-write "
					"observation: " + std::string(error.what()) + "\n").c_str());
		}
		catch (...)
		{
			OutputDebugStringA(
				"[SA2 Six Garden Expansion] Could not format Chao save-write observation.\n");
		}

		return result;
	}

	int __cdecl SaveChaoFileHookWorker0(char* path, void* buffer, std::size_t count)
	{
		return SaveChaoFileHookWorker(0, path, buffer, count);
	}

	int __cdecl SaveChaoFileHookWorker1(char* path, void* buffer, std::size_t count)
	{
		return SaveChaoFileHookWorker(1, path, buffer, count);
	}

	int __cdecl SaveChaoFileHookWorker2(char* path, void* buffer, std::size_t count)
	{
		return SaveChaoFileHookWorker(2, path, buffer, count);
	}

	void __declspec(naked) __cdecl SaveChaoFileHook0()
	{
		__asm
		{
			push dword ptr[esp + 4]
			push edx
			push ecx
			call SaveChaoFileHookWorker0
			pop ecx
			pop edx
			add esp, 4
			ret
		}
	}

	void __declspec(naked) __cdecl SaveChaoFileHook1()
	{
		__asm
		{
			push dword ptr[esp + 4]
			push edx
			push ecx
			call SaveChaoFileHookWorker1
			pop ecx
			pop edx
			add esp, 4
			ret
		}
	}

	void __declspec(naked) __cdecl SaveChaoFileHook2()
	{
		__asm
		{
			push dword ptr[esp + 4]
			push edx
			push ecx
			call SaveChaoFileHookWorker2
			pop ecx
			pop edx
			add esp, 4
			ret
		}
	}

	void Log(const std::string& message) noexcept
	{
		try
		{
			const auto line = "[SA2 Six Garden Expansion] " + message + "\n";
			OutputDebugStringA(line.c_str());

			if (!ModDirectory.empty())
			{
				std::ofstream log(ModDirectory / L"GardenExpansion.log", std::ios::app);
				if (log)
				{
					log << line;
				}
			}
		}
		catch (...)
		{
			OutputDebugStringA("[SA2 Six Garden Expansion] Could not write the mod log.\n");
		}
	}

	void __cdecl Initialize(const char* path, const HelperFunctions& helpers)
	{
		try
		{
			if (path == nullptr || path[0] == '\0')
			{
				Log("The loader did not provide a mod directory.");
				return;
			}

			ModDirectory = std::filesystem::u8path(path);
			AddVectoredExceptionHandler(1, LogAccessViolation);
			Log("Native plugin initialized; the marker-gated virtual-garden pool "
				"test is disabled by default.");

			if (helpers.Version < 4 || helpers.GetChaoSavePath == nullptr)
			{
				Log("Loader helper version 4 or newer is required to locate Chao saves.");
				return;
			}

			const char* saveDirectory = helpers.GetChaoSavePath();
			if (saveDirectory == nullptr || saveDirectory[0] == '\0')
			{
				Log("The loader returned an empty Chao save directory.");
				return;
			}

			ChaoSaveDirectory = std::filesystem::u8path(saveDirectory);
			Log("Chao save directory located; resolving the active Chao save filename from the game.");
		}
		catch (const std::exception& error)
		{
			Log(std::string("Initialization failed: ") + error.what());
		}
		catch (...)
		{
			Log("Initialization failed with an unknown error.");
		}
	}

	void LogChaoSaveSidecar()
	{
		if (ChaoSaveDirectory.empty())
		{
			return;
		}

		try
		{
			const auto chaoSaveFilename = ReadCurrentChaoSaveFilename();
			if (chaoSaveFilename == LastCheckedChaoSaveFilename)
			{
				return;
			}

			const auto sidecarPath = garden_pools::SidecarPathForSave(
				ChaoSaveDirectory,
				chaoSaveFilename);
			LastCheckedChaoSaveFilename = chaoSaveFilename;
			LastChaoSaveIdentityError.clear();
			std::error_code error;
			const bool sidecarExists = std::filesystem::exists(sidecarPath, error);
			if (error)
			{
				Log("Could not check the Chao save sidecar for "
					+ chaoSaveFilename + ": " + error.message());
				return;
			}
			if (!sidecarExists)
			{
				Log("No added-garden sidecar exists for " + chaoSaveFilename
					+ "; leaving all game saves untouched.");
				return;
			}

			const auto pools = garden_pools::Load(sidecarPath);
			std::string summary = "Validated added-garden sidecar for ";
			summary += chaoSaveFilename;
			summary += ":";
			for (const auto garden : garden_pools::AddedGardens)
			{
				const auto& pool = garden_pools::PoolFor(pools, garden);
				summary += " ";
				summary += garden_pools::SADXStageName(garden);
				summary += "=" + std::to_string(pool.size());
			}
			Log(summary);
		}
		catch (const std::exception& error)
		{
			const auto identityError = std::string(error.what());
			if (identityError != LastChaoSaveIdentityError)
			{
				Log("Could not resolve or validate the current Chao save sidecar: "
					+ identityError);
				LastChaoSaveIdentityError = identityError;
			}
		}
		catch (...)
		{
			if (LastChaoSaveIdentityError != "unknown")
			{
				Log("Could not resolve or validate the current Chao save sidecar due to an unknown error.");
				LastChaoSaveIdentityError = "unknown";
			}
		}
	}

	void __cdecl LogChaoStateChanges()
	{
		const int saveIndex = static_cast<int>(ChaoSaveIndexThing);
		const int lastChaoArea = LastChaoArea;
		const int currentChaoArea = CurrentChaoArea;
		const int nextChaoArea = NextChaoArea;

		if (HasPreviousChaoState
			&& saveIndex == PreviousSaveIndex
			&& lastChaoArea == PreviousLastChaoArea
			&& currentChaoArea == PreviousCurrentChaoArea
			&& nextChaoArea == PreviousNextChaoArea)
		{
			return;
		}

		HasPreviousChaoState = true;
		PreviousSaveIndex = saveIndex;
		PreviousLastChaoArea = lastChaoArea;
		PreviousCurrentChaoArea = currentChaoArea;
		PreviousNextChaoArea = nextChaoArea;

		Log("Chao state changed: save-index=" + std::to_string(saveIndex)
			+ " last=" + std::to_string(lastChaoArea)
			+ " current=" + std::to_string(currentChaoArea)
			+ " next=" + std::to_string(nextChaoArea));
	}
}

extern "C" __declspec(dllexport) ModInfo SA2ModInfo =
{
	ModLoaderVer,
	Initialize,
	nullptr,
	0,
	nullptr,
	0,
	nullptr,
	0,
	nullptr,
	0
};

// Adds one entry to the main stage-select map by moving the game's 33-entry
// stage table and its unlock-flag array into larger buffers and re-pointing every
// reference in the menu code. The sites come from StageSelectSites.h.
constexpr std::uintptr_t StageTableAddress = 0xC75078;
constexpr std::uintptr_t StageFlagAddress = 0x1A4A974;
constexpr int StageOriginalCount = 33;
constexpr int StageNewCount = 34;
constexpr int StageBufferCount = 36; // the unrolled navigation loops step in threes
constexpr int ChaoArenaStageIndex = 33;
alignas(16) int StageTableBuffer[StageBufferCount * 4] = {};
alignas(16) int StageFlagBuffer[StageBufferCount] = {};
bool StageSelectPatched = false;

bool PatchCodeDword(std::uintptr_t address, std::uint32_t value)
{
	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(address), 4, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		return false;
	}
	*reinterpret_cast<std::uint32_t*>(address) = value;
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 4);
	DWORD ignored = 0;
	VirtualProtect(reinterpret_cast<void*>(address), 4, oldProtect, &ignored);
	return true;
}

void PatchStageSelectOnce()
{
	if (StageSelectPatched)
	{
		return;
	}
	const auto* live = reinterpret_cast<const int*>(StageTableAddress);
	// Only patch once the menu code is decrypted and the table looks like the stock one.
	if (live[0] != 23 || live[1] != 6 || live[2] != 19 || live[3] != 1
		|| live[22 * 4] != 90 || *reinterpret_cast<const std::uint8_t*>(0x677ccf) != 0x83)
	{
		return;
	}
	StageSelectPatched = true;

	std::memcpy(StageTableBuffer, live, sizeof(int) * 4 * StageOriginalCount);
	// Chao Garden's own entry, one map row below it: level 90 reuses its icon,
	// unlock rule and character-select flow.
	StageTableBuffer[ChaoArenaStageIndex * 4 + 0] = 90;
	StageTableBuffer[ChaoArenaStageIndex * 4 + 1] = 0;
	StageTableBuffer[ChaoArenaStageIndex * 4 + 2] = 4;
	StageTableBuffer[ChaoArenaStageIndex * 4 + 3] = 2;

	const auto newTable = reinterpret_cast<std::uintptr_t>(StageTableBuffer);
	const auto newFlags = reinterpret_cast<std::uintptr_t>(StageFlagBuffer);
	int failures = 0;
	auto patch = [&](std::uintptr_t site, std::uintptr_t expected, std::uintptr_t replacement)
	{
		if (*reinterpret_cast<const std::uint32_t*>(site) != expected
			|| !PatchCodeDword(site, static_cast<std::uint32_t>(replacement)))
		{
			++failures;
		}
	};
	for (const auto site : StageTableSites)
	{
		const auto old = *reinterpret_cast<const std::uint32_t*>(site);
		patch(site, old, newTable + (old - StageTableAddress));
	}
	for (const auto site : StageFlagSites)
	{
		const auto old = *reinterpret_cast<const std::uint32_t*>(site);
		patch(site, old, newFlags + (old - StageFlagAddress));
	}
	for (const auto site : StageTableEnd0Sites)
	{
		patch(site, StageTableAddress + StageOriginalCount * 16,
			newTable + StageNewCount * 16);
	}
	for (const auto site : StageTableEnd4Sites)
	{
		patch(site, StageTableAddress + StageOriginalCount * 16 + 4,
			newTable + StageNewCount * 16 + 4);
	}
	for (const auto site : StageCountSites)
	{
		// cmp reg, 33 -> cmp reg, 36 (immediate byte follows the two opcode bytes).
		DWORD oldProtect = 0;
		VirtualProtect(reinterpret_cast<void*>(site), 3, PAGE_EXECUTE_READWRITE, &oldProtect);
		auto* byte = reinterpret_cast<std::uint8_t*>(site + 2);
		if (*byte == StageOriginalCount)
		{
			*byte = StageBufferCount;
		}
		else
		{
			++failures;
		}
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 3);
	}
	Log("Stage select expanded to " + std::to_string(StageNewCount)
		+ " entries (" + std::to_string(failures) + " patch failures).");
}

// ---- Chao Farm stage-select presentation -------------------------------
bool WriteCode(std::uintptr_t address, const void* bytes, std::size_t size);

// The menu draws every sprite from the NJS_TEXANIM array at 0xC68C50 over the
// stageMap texlist. Normal stages pick their highlighted name from
// NameIdTable (0x8A0560, +0x21 English); level 90 instead draws texanim 40
// (one language) / 73 (the other) from its record-panel tail (0x6768A0), and
// its map node icon is hard-coded to texanim 7 (the Chao photo). The mod's
// stageMap.prs/pak carries "CHAO FARM" and a Chao-key tile in the free bottom
// strip of the stg_stagename atlas (texture 14); see tools\build_stagemap_farm.py.
struct MenuTexanim
{
	std::int16_t sx, sy, cx, cy;
	std::uint16_t u1, v1, u2, v2;
	std::int32_t texid;
};
static_assert(sizeof(MenuTexanim) == 20, "NJS_TEXANIM is 20 bytes");
auto* const MenuTexanims = reinterpret_cast<MenuTexanim*>(0xC68C50);
// Chao World's own header name entries, swapped while entry 33 is highlighted.
constexpr int ChaoWorldNameAnimA = 40;
constexpr int ChaoWorldNameAnimB = 73;
// Icon donors: 10 is a Japanese-only name slot, 43 its English counterpart;
// whichever the current language never draws holds the key tile.
constexpr int IconDonorJpSlot = 10;
constexpr int IconDonorEnSlot = 43;
constexpr std::uintptr_t MenuLanguageByte = 0x174AFD1;
constexpr std::uintptr_t NodeIconPatchSite = 0x676423;
constexpr unsigned char NodeIconOriginal[5] = { 0x8D, 0x70, 0xAD, 0xEB, 0x0A };
// Character-select and map-hover milestone boxes: 0x676CF9 (character select)
// and 0x676B43 (map hover panel) call the level-90 record grid (0x6768A0).
// The arena has no missions, so for entry 33 the grid is skipped and only the
// function's tail (the header name draw at 0x6769E2) runs. The panel's plate
// backdrop (shared texanims 91/92/93) is left untouched so it still covers the
// map behind the panel and other stages are unaffected.
constexpr std::uintptr_t CharSelectBoxesCallSite = 0x676CF9;
constexpr std::uintptr_t MapPanelBoxesCallSite = 0x676B43;
constexpr std::uintptr_t Level90RecordPanel = 0x6768A0;
constexpr std::uintptr_t Level90RecordPanelTail = 0x6769E2;

MenuTexanim SavedNameAnims[2] = {}; // stock copies of 40 and 73
MenuTexanim SavedIconAnims[2] = {}; // stock copies of 10 and 43
bool FarmMenuReady = false;
bool FarmMenuFailed = false;
bool NodeIconHooked = false;
int ArenaNodeIconIndex = 7;

// Values printed by tools\build_stagemap_farm.py.
void SetFarmNameAnim(MenuTexanim& anim)
{
	anim.sx = 134; anim.sy = 18; anim.cx = 0; anim.cy = 0;
	anim.u1 = 4; anim.v1 = 232; anim.u2 = 71; anim.v2 = 241;
	anim.texid = 14;
}

void SetFarmIconAnim(MenuTexanim& anim)
{
	anim = MenuTexanims[7];
	anim.u1 = 228; anim.v1 = 231; anim.u2 = 252; anim.v2 = 255;
	anim.texid = 14;
}

// The level-90 character select locks each character behind their
// chao-garden-visited save byte (0x1DEF829 + offset from the map at 0xC75368).
// While the Farm node is selected the three checks (portrait dim at 0x676D12,
// confirms at 0x6782AA and 0x6784A4) read a mod-owned all-ones buffer instead,
// so every character is playable on any save; the real Chao World node keeps
// the stock behaviour. Save data itself is never written.
unsigned char FarmCharsUnlocked[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
constexpr std::uint32_t CharLockStockDisp = 0x1DEF829;
struct CharLockSite
{
	std::uintptr_t Site;
	unsigned char Opcode[2];
};
constexpr CharLockSite CharLockSites[3] = {
	{ 0x676D12, { 0x80, 0xB8 } }, // cmp byte [eax+disp], 0  (portrait dim)
	{ 0x6782AA, { 0x38, 0x91 } }, // cmp byte [ecx+disp], dl (confirm)
	{ 0x6784A4, { 0x38, 0x82 } }, // cmp byte [edx+disp], al (confirm)
};
bool WriteCodeDword(std::uintptr_t address, std::uintptr_t value);

void UpdateFarmCharUnlocks()
{
	const bool farm = StageSelectLevelSelection == ChaoArenaStageIndex;
	const auto mine = static_cast<std::uint32_t>(
		reinterpret_cast<std::uintptr_t>(FarmCharsUnlocked));
	for (const auto& entry : CharLockSites)
	{
		const auto* bytes = reinterpret_cast<const unsigned char*>(entry.Site);
		if (bytes[0] != entry.Opcode[0] || bytes[1] != entry.Opcode[1])
		{
			continue;
		}
		const auto disp = *reinterpret_cast<const std::uint32_t*>(entry.Site + 2);
		if (farm && disp == CharLockStockDisp)
		{
			WriteCodeDword(entry.Site + 2, mine);
		}
		else if (!farm && disp == mine)
		{
			WriteCodeDword(entry.Site + 2, CharLockStockDisp);
		}
	}
}

// Replaces "lea esi,[eax-0x53]; jmp +0xA" in the map-node loop: node 33 shows
// the Chao-key tile, every other level-90 node keeps the Chao photo (7).
__declspec(naked) void StageNodeIconHook()
{
	__asm
	{
		cmp ebp, 33
		jne stock
		mov esi, dword ptr [ArenaNodeIconIndex]
		jmp done
	stock:
		mov esi, 7
	done:
		add dword ptr [esp], 10
		ret
	}
}

// For entry 33 the record grid is skipped: the stub replays the panel
// function's prologue (frame and language offset in EBP) and jumps straight
// to its header-name tail, which reads nothing set up by the grid loop.
__declspec(naked) void CharSelectBoxesHook()
{
	__asm
	{
		movsx eax, byte ptr ds:[0x1D1BF08] // StageSelectLevelSelection
		cmp eax, 33
		je skip
		mov eax, 0x6768A0 // Level90RecordPanel
		jmp eax
	skip:
		sub esp, 0x44
		push ebx
		push ebp
		push esi
		push edi
		movsx ebp, byte ptr ds:[0x174AFD1] // MenuLanguageByte
		neg ebp
		sbb ebp, ebp
		and ebp, 0x21
		mov eax, 0x6769E2 // Level90RecordPanelTail
		jmp eax
	}
}

// Installs a 5-byte call hook at `site`, but only when the site still holds the
// game's original bytes. Re-checked every frame because the SA2 PC port
// re-decrypts the stage-select code region on each node load, which restores
// the original bytes and wipes the hook. Returns true when the hook is present
// (freshly installed or already ours).
bool EnsureCallHook(std::uintptr_t site, std::uintptr_t stockTarget,
	const void* hook, const unsigned char* originalBytes)
{
	const auto* bytes = reinterpret_cast<const unsigned char*>(site);
	if (originalBytes != nullptr)
	{
		if (std::memcmp(bytes, originalBytes, 5) == 0)
		{
			unsigned char call[5] = { 0xE8 };
			const auto rel = static_cast<std::int32_t>(
				reinterpret_cast<std::uintptr_t>(hook) - (site + 5));
			std::memcpy(call + 1, &rel, sizeof(rel));
			return WriteCode(site, call, sizeof(call));
		}
		// Already our 5-byte jump/call to the naked stub.
		return bytes[0] == 0xE8 && site + 5
			+ *reinterpret_cast<const std::int32_t*>(bytes + 1)
			== reinterpret_cast<std::uintptr_t>(hook);
	}
	// Call site: distinguish stock target from our hook target.
	if (bytes[0] != 0xE8)
	{
		return false;
	}
	const auto target = site + 5 + *reinterpret_cast<const std::int32_t*>(bytes + 1);
	if (target == reinterpret_cast<std::uintptr_t>(hook))
	{
		return true;
	}
	if (target != stockTarget)
	{
		return false;
	}
	unsigned char call[5] = { 0xE8 };
	const auto rel = static_cast<std::int32_t>(
		reinterpret_cast<std::uintptr_t>(hook) - (site + 5));
	std::memcpy(call + 1, &rel, sizeof(rel));
	return WriteCode(site, call, sizeof(call));
}

// The stage-select task recomputes every unlock flag each frame (loop ending
// at 0x677437); entry 33 inherits Chao World's rule (any chao garden visited),
// which is 0 on a fresh save, and the snap right after moves a cursor that sits
// on a locked node to City Escape or Iron Gate. Hooking the first instruction
// after the loop (mov bl, [0x1D1BF08], 6 bytes) forces the Chao Farm flag back
// to 1 after each recompute, so the node exists on every save.
constexpr std::uintptr_t FlagSnapSite = 0x67743D;
constexpr unsigned char FlagSnapOriginal[6] = { 0x8A, 0x1D, 0x08, 0xBF, 0xD1, 0x01 };

__declspec(naked) void FlagSnapHook()
{
	__asm
	{
		mov dword ptr [StageFlagBuffer + 132], 1 // entry 33
		mov bl, byte ptr ds:[0x1D1BF08]
		ret
	}
}

void EnsureFlagSnapHook()
{
	const auto* bytes = reinterpret_cast<const unsigned char*>(FlagSnapSite);
	if (std::memcmp(bytes, FlagSnapOriginal, sizeof(FlagSnapOriginal)) == 0)
	{
		unsigned char patch[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
		const auto rel = static_cast<std::int32_t>(
			reinterpret_cast<std::uintptr_t>(&FlagSnapHook) - (FlagSnapSite + 5));
		std::memcpy(patch + 1, &rel, sizeof(rel));
		WriteCode(FlagSnapSite, patch, sizeof(patch));
	}
}

// (Re)applies all the code hooks. Safe to call every frame; a no-op once the
// hooks are in place and the decrypted code has not been restored.
bool EnsureFarmMenuHooks()
{
	bool ok = EnsureCallHook(NodeIconPatchSite, 0,
		reinterpret_cast<const void*>(&StageNodeIconHook), NodeIconOriginal);
	ok = EnsureCallHook(CharSelectBoxesCallSite, Level90RecordPanel,
		reinterpret_cast<const void*>(&CharSelectBoxesHook), nullptr) && ok;
	ok = EnsureCallHook(MapPanelBoxesCallSite, Level90RecordPanel,
		reinterpret_cast<const void*>(&CharSelectBoxesHook), nullptr) && ok;
	EnsureFlagSnapHook();
	NodeIconHooked = ok;
	return ok;
}

void UpdateFarmMenuArt()
{
	if (!StageSelectPatched || FarmMenuFailed)
	{
		return;
	}
	StageFlagBuffer[ChaoArenaStageIndex] = 1; // always unlocked, also on fresh saves
	if (!FarmMenuReady)
	{
		SavedNameAnims[0] = MenuTexanims[ChaoWorldNameAnimA];
		SavedNameAnims[1] = MenuTexanims[ChaoWorldNameAnimB];
		SavedIconAnims[0] = MenuTexanims[IconDonorJpSlot];
		SavedIconAnims[1] = MenuTexanims[IconDonorEnSlot];
		FarmMenuReady = true;
		Log("Chao Farm menu art enabled.");
	}
	// Code hooks are restored by the port's per-node code decryption, so they
	// are re-ensured every frame.
	EnsureFarmMenuHooks();
	UpdateFarmCharUnlocks();
	// The language's unused donor slot holds the key tile; the other stays stock.
	const bool english = *reinterpret_cast<const char*>(MenuLanguageByte) != 0;
	const int icon = english ? IconDonorJpSlot : IconDonorEnSlot;
	SetFarmIconAnim(MenuTexanims[icon]);
	if (english)
	{
		MenuTexanims[IconDonorEnSlot] = SavedIconAnims[1];
	}
	else
	{
		MenuTexanims[IconDonorJpSlot] = SavedIconAnims[0];
	}
	ArenaNodeIconIndex = icon;
	if (StageSelectLevelSelection == ChaoArenaStageIndex)
	{
		SetFarmNameAnim(MenuTexanims[ChaoWorldNameAnimA]);
		SetFarmNameAnim(MenuTexanims[ChaoWorldNameAnimB]);
	}
	else
	{
		MenuTexanims[ChaoWorldNameAnimA] = SavedNameAnims[0];
		MenuTexanims[ChaoWorldNameAnimB] = SavedNameAnims[1];
	}
}

// ---- Main-menu "CHAO FARM" button --------------------------------------
// The PC port's main 1P-menu list has a seventh entry, DOWNLOAD CONTENT (item
// index 5), that is skipped and greyed out once the full game is owned
// (0x1A556F8 != 0). The mod renames its label to CHAO FARM (in the
// mainmenu_button_all_ex atlas) and makes the entry a normal, selectable item
// that opens the 1P Play mode menu. All sites sit in the main-menu code, which
// the port can re-decrypt, so the byte patches are re-ensured every frame.
constexpr std::uintptr_t MenuNavDownJe = 0x664FC9;   // je that skips item 5 going down
constexpr std::uintptr_t MenuNavUpJe = 0x665005;     // je that skips item 5 going up
constexpr std::uintptr_t MenuDimJe = 0x66964D;       // je into the dim-item-5 draw
constexpr std::uintptr_t MenuConfirmTableItem5 = 0x666168; // confirm jump target for item 5
constexpr std::uintptr_t MenuDescTableItem5 = 0x66614C;    // subtitle jump target for item 5
constexpr std::uintptr_t MenuConfirm1PPlay = 0x66504C;     // 1P PLAY confirm handler
constexpr std::uintptr_t MenuConfirmDownload = 0x6651DE;   // stock download-content handler
constexpr std::uintptr_t MenuDescItem5 = 0x664F8C;         // stock item-5 subtitle
constexpr std::uintptr_t MenuDescItem0 = 0x664F6F;         // 1P PLAY subtitle

// Patches one byte only while it still holds `original`; a no-op once ours.
void EnsureByte(std::uintptr_t site, unsigned char original, unsigned char patched)
{
	auto* p = reinterpret_cast<unsigned char*>(site);
	if (*p == original)
	{
		WriteCode(site, &patched, 1);
	}
}

void EnsureDword(std::uintptr_t site, std::uint32_t original, std::uint32_t patched)
{
	auto* p = reinterpret_cast<std::uint32_t*>(site);
	if (*p == original)
	{
		WriteCode(site, &patched, 4);
	}
}

// ---- Straight-to-Chao-Farm warp -----------------------------------------
// Confirming CHAO FARM runs the stock 1P PLAY handler (so the circle menu
// opens normally) with a pending flag set; a hook before the per-frame menu
// dispatch (call site 0x666275 -> 0x67E620) then drives the flow with cursor
// writes and injected button edges only, so the game runs its complete stock
// path for every transition and lands on the Farm's character select.
constexpr std::uintptr_t FarmWarpMenuId = 0x1D7BB10;
constexpr std::uintptr_t FarmWarpMenuSub = 0x1D7BB14;
constexpr std::uintptr_t FarmWarpMenuData = 0xC77F14;
constexpr std::uintptr_t FarmWarpInputEdge = 0x1DEFAB0;
constexpr std::uintptr_t StageSelectCursorAddress = 0x1D1BF08;
constexpr std::uintptr_t StageSelectTexturesLoaded = 0x1D1BEAC;
constexpr std::uintptr_t RunMenusDispatchCallSite = 0x666275;
constexpr std::uintptr_t RunMenusDispatchTarget = 0x67E620;
constexpr int FarmWarpTimeoutFrames = 900;

bool FarmWarpPending = false;
int FarmWarpFrames = 0;
int FarmWarpMapSettle = -1;

void __cdecl FarmWarpConfirmed()
{
	FarmWarpPending = true;
	FarmWarpFrames = 0;
	FarmWarpMapSettle = -1;
	Log("Chao Farm button confirmed; warping to the Farm character select.");
}

// Jump-table target for item 5: note the warp, then run the stock 1P PLAY
// confirm handler with all registers as the dispatch left them.
__declspec(naked) void FarmConfirmStub()
{
	__asm
	{
		pushad
		pushfd
		call FarmWarpConfirmed
		popfd
		popad
		mov eax, 0x66504C // 1P PLAY confirm handler
		jmp eax
	}
}

void __cdecl FarmWarpPreFrame()
{
	// The Chao Farm node is always unlocked, also on fresh saves. The menu
	// computes the flag from save progress when it opens; this runs right
	// before the menu dispatch, so the forced value is what it reads.
	if (StageSelectPatched)
	{
		StageFlagBuffer[ChaoArenaStageIndex] = 1;
	}
	if (!FarmWarpPending)
	{
		return;
	}
	if (++FarmWarpFrames > FarmWarpTimeoutFrames)
	{
		FarmWarpPending = false;
		Log("Chao Farm warp timed out; leaving the menus alone.");
		return;
	}
	// Everything below is cursor writes plus injected button edges: exactly
	// what a human pressing the buttons produces, so every transition runs the
	// game's complete stock path (an earlier build replayed menu internals and
	// left stale panel state that crashed the deferred sprite flush).
	const int menu = *reinterpret_cast<const int*>(FarmWarpMenuId);
	const int sub = *reinterpret_cast<const int*>(FarmWarpMenuSub);
	const auto menuData = *reinterpret_cast<const std::uintptr_t*>(FarmWarpMenuData);
	if (menu == 1 && sub == 4 && menuData != 0)
	{
		// 1P Play circles are interactive: cursor onto STAGE SELECT (1), then
		// confirm. Separate frames, so the game sees the cursor move first.
		auto& selection = *reinterpret_cast<std::int8_t*>(menuData + 0x40);
		if (selection != 1)
		{
			selection = 1;
		}
		else
		{
			*reinterpret_cast<std::uint8_t*>(FarmWarpInputEdge) |= 0x08;
		}
	}
	else if (menu == 1 && sub == 5 && menuData != 0
		&& *reinterpret_cast<std::int8_t*>(menuData + 0x40) == 0)
	{
		// New-save path: with fewer than two circles unlocked the 1P PLAY
		// handler skips the circle menu and auto-confirms STORY (sub 5,
		// selection 0). Steer the pending transition to STAGE SELECT instead.
		*reinterpret_cast<std::int8_t*>(menuData + 0x40) = 1;
	}
	else if (menu == 3 && sub == 2
		&& *reinterpret_cast<const int*>(StageSelectTexturesLoaded) != 0)
	{
		// The node map is interactive and its textures are loaded. Hold the
		// cursor on Chao Farm for a few frames first: the level-90 panel hooks
		// are reinstalled by UpdateFarmMenuArt in the separate OnFrame callback
		// (the port wipes them when it re-decrypts the stage-select code on
		// entry), and confirming before they land runs the stock record panel.
		*reinterpret_cast<std::int8_t*>(StageSelectCursorAddress) =
			static_cast<std::int8_t>(ChaoArenaStageIndex);
		if (FarmWarpMapSettle < 0)
		{
			FarmWarpMapSettle = 0;
		}
		else if (++FarmWarpMapSettle >= 20 && FarmMenuReady && NodeIconHooked)
		{
			*reinterpret_cast<std::uint8_t*>(FarmWarpInputEdge) |= 0x08;
			FarmWarpPending = false;
			Log("Chao Farm warp: node 33 confirmed.");
		}
	}
	else if (menu != 1 && menu != 3)
	{
		FarmWarpPending = false; // left the expected menu flow
	}
}

// Runs before the game's menu dispatch, so the injected input and cursor are
// read in the same frame.
__declspec(naked) void FarmWarpPreFrameStub()
{
	__asm
	{
		pushad
		pushfd
		call FarmWarpPreFrame
		popfd
		popad
		mov eax, 0x67E620 // RunMenusDispatchTarget
		jmp eax
	}
}

void EnsureMainMenuButton()
{
	// Turn the two "je skip-item-5" branches into unconditional jumps over the
	// skip, so the entry is navigable, and the "je" before the dim draw into an
	// unconditional jump to the normal (bright) draw.
	EnsureByte(MenuNavDownJe, 0x74, 0xEB);
	EnsureByte(MenuNavUpJe, 0x74, 0xEB);
	EnsureByte(MenuDimJe, 0x74, 0xEB);
	// Confirm warps to the Farm character select; subtitle matches 1P Play.
	EnsureDword(MenuConfirmTableItem5, MenuConfirmDownload, static_cast<std::uint32_t>(
		reinterpret_cast<std::uintptr_t>(&FarmConfirmStub)));
	EnsureDword(MenuDescTableItem5, MenuDescItem5, MenuDescItem0);
	EnsureCallHook(RunMenusDispatchCallSite, RunMenusDispatchTarget,
		reinterpret_cast<const void*>(&FarmWarpPreFrameStub), nullptr);
}

struct ArenaState
{
	bool Active = false;
	std::unique_ptr<sa2lvl::LoadedLandTable> Land;
	LandTable* Live = nullptr;
	LandTable SavedLand = {};
	std::vector<std::pair<StartPosition*, StartPosition>> SavedStarts;
	bool TimerHidden = false;
	unsigned char SavedTimerCall[5] = {};
	bool Populated = false;
	bool WarpRequested = false;
	std::vector<SETEntry> Entries;
	std::vector<int> Species;
	std::vector<char> SeenAlive;
	std::size_t CrateIndex = 0;
	std::size_t GoalIndex = 0;
	std::array<ObjectMaster*, 4> Drives = {};
	bool DrivesFailed = false;
	int BaseLevel = LevelIDs_CityEscape;
	int DiagFrames = 0;
	int MusicFrames = 0;
	bool MusicGiveUp = false;
	char MusicLastSeen[32] = {};
	std::size_t EmeraldIndex = SIZE_MAX;
	bool EmeraldsArmed = false;
} Arena;

constexpr std::size_t ArenaEmeraldCount = 3;
constexpr std::uintptr_t EmeraldManagerMain = 0x739570;

constexpr std::uintptr_t StageAnimalMain = 0x4898B0;
constexpr std::uintptr_t StageDriveMain = 0x48EEF0;
constexpr std::uintptr_t StageDriveInit = 0x48F810;
constexpr std::uintptr_t StageDriveSpawn = 0x48F8E0;
constexpr std::int16_t SetObjectActive = static_cast<std::int16_t>(0x8000);
constexpr int ArenaAnimalSpecies = 21;

// Each character plays the arena on top of a stage it natively supports:
// Knuckles and Rouge on Pumpkin Hill, Tails on Prison Lane, Eggman on Weapons Bed,
// everyone else on City Escape.
int ArenaBaseLevelFor(int character)
{
	switch (character)
	{
	case Characters_Knuckles:
	case Characters_Rouge:
		return LevelIDs_PumpkinHill;
	case Characters_Tails:
	case Characters_MechTails:
		return LevelIDs_PrisonLane;
	case Characters_Eggman:
	case Characters_MechEggman:
		return LevelIDs_WeaponsBed;
	default:
		return LevelIDs_CityEscape;
	}
}

const char* ArenaLandFileFor(int level)
{
	switch (level)
	{
	case LevelIDs_PumpkinHill:
		return "ArenaPumpkin.sa2blvl";
	case LevelIDs_PrisonLane:
		return "ArenaPrison.sa2blvl";
	case LevelIDs_WeaponsBed:
		return "ArenaWeaponsBed.sa2blvl";
	default:
		return "Arena.sa2blvl";
	}
}

// Each character hears a song from one of its own missions. The names are the
// game's own ADX tracks (the MusicList table at 0x171A2B0 holds them all).
const char* ArenaMusicFor(int character)
{
	switch (character)
	{
	case Characters_Shadow:
		return "r_hwy.adx"; // Radical Highway
	case Characters_Knuckles:
		return "p_hill.adx"; // Pumpkin Hill
	case Characters_Rouge:
		return "s_hall.adx"; // Security Hall
	case Characters_Tails:
	case Characters_MechTails:
		return "m_street.adx"; // Mission Street
	case Characters_Eggman:
	case Characters_MechEggman:
		return "i_gate.adx"; // Iron Gate
	default:
		return "c_escap1.adx"; // City Escape
	}
}

// Starts the character's song once in game and re-asserts it only if the
// stage's own music task observably switches to a different track. On this
// port CurrentSongName can stay empty, so an unreadable name never retriggers.
void ArenaMaintainMusic()
{
	const char* song = ArenaMusicFor(CurrentCharacter);
	if (Arena.MusicGiveUp || Arena.MusicFrames++ % 60 != 0)
	{
		return;
	}
	const bool nameReadable = CurrentSongName[0] != '\0';
	if (Arena.MusicLastSeen[0] != '\0') // already started once
	{
		if (!nameReadable || _strnicmp(&CurrentSongName[0], song, 31) == 0)
		{
			return;
		}
		if (_strnicmp(&CurrentSongName[0], Arena.MusicLastSeen, 31) == 0)
		{
			Arena.MusicGiveUp = true;
			Log("Arena music did not take (current '" + std::string(&CurrentSongName[0])
				+ "'); leaving the stage music alone.");
			return;
		}
		std::memcpy(Arena.MusicLastSeen, &CurrentSongName[0], sizeof(Arena.MusicLastSeen));
		Arena.MusicLastSeen[sizeof(Arena.MusicLastSeen) - 1] = '\0';
	}
	else
	{
		if (nameReadable && _strnicmp(&CurrentSongName[0], song, 31) == 0)
		{
			return;
		}
		// remember that the first play happened even when the name is unreadable
		const char* seen = nameReadable ? &CurrentSongName[0] : "?";
		std::memcpy(Arena.MusicLastSeen, seen,
			(std::min)(std::strlen(seen) + 1, sizeof(Arena.MusicLastSeen) - 1));
	}
	PlayMusic(song);
	Log("Arena music set to " + std::string(song) + " for character "
		+ std::to_string(CurrentCharacter) + ".");
}

// Object ids differ per stage, so they are looked up by name in the live list.
std::int16_t FindStageObjectId(const char* name)
{
	const ObjectListHead* list = CurrentObjectList;
	if (list == nullptr || list->List == nullptr)
	{
		return -1;
	}
	for (int i = 0; i < list->Count; ++i)
	{
		const char* entry = list->List[i].Name;
		if (entry == nullptr)
		{
			continue;
		}
		std::string trimmed(entry);
		while (!trimmed.empty() && trimmed.back() == ' ')
		{
			trimmed.pop_back();
		}
		if (trimmed == name)
		{
			return static_cast<std::int16_t>(i);
		}
	}
	return -1;
}

// The stock animal loader keeps five model slots per stage; the arena needs
// all 21 species, so the slot array moves to a larger buffer and the loader is
// pointed at a 21-entry species table while the arena is active.
alignas(16) unsigned char AnimalSlotBuffer[ArenaAnimalSpecies * 16] = {};
std::int32_t AnimalSpeciesTable[ArenaAnimalSpecies] = {};
bool AnimalSlotsRelocated = false;
bool AnimalLoaderPatched = false;

bool WriteCode(std::uintptr_t address, const void* bytes, std::size_t size)
{
	DWORD oldProtect = 0;
	if (!VirtualProtect(reinterpret_cast<void*>(address), size,
		PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		return false;
	}
	std::memcpy(reinterpret_cast<void*>(address), bytes, size);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
	VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &oldProtect);
	return true;
}

bool WriteCodeDword(std::uintptr_t address, std::uintptr_t value)
{
	const auto word = static_cast<std::uint32_t>(value);
	return WriteCode(address, &word, sizeof(word));
}

void RelocateAnimalSlots()
{
	if (AnimalSlotsRelocated)
	{
		return;
	}
	const auto base = reinterpret_cast<std::uintptr_t>(AnimalSlotBuffer);
	bool ok = WriteCodeDword(0x48A2F7, base)
		&& WriteCodeDword(0x489DE8, base + 8)
		&& WriteCodeDword(0x489DF5, base + 0xC)
		&& WriteCodeDword(0x489E09, base + 8)
		&& WriteCodeDword(0x48A732, base + 8)
		&& WriteCodeDword(0x48AA29, base + 8);
	const unsigned char freeCount = ArenaAnimalSpecies - 1;
	ok = ok && WriteCode(0x48AA24, &freeCount, 1);
	AnimalSlotsRelocated = ok;
	Log(ok ? "Animal slot array relocated." : "Animal slot relocation failed.");
}

void SetAnimalLoaderArena(bool arena)
{
	if (!AnimalSlotsRelocated || arena == AnimalLoaderPatched)
	{
		return;
	}
	for (int i = 0; i < ArenaAnimalSpecies; ++i)
	{
		AnimalSpeciesTable[i] = i;
	}
	// The loader indexes the table by level * 5 + slot.
	const std::uintptr_t table = arena
		? reinterpret_cast<std::uintptr_t>(AnimalSpeciesTable) - Arena.BaseLevel * 5 * 4
		: 0x171B378;
	const unsigned char count = arena ? ArenaAnimalSpecies : 5;
	if (WriteCodeDword(0x48A75F, table) && WriteCode(0x48A9F5, &count, 1))
	{
		AnimalLoaderPatched = arena;
	}
}

template <typename Visitor>
void ForEachObjectTask(Visitor visit)
{
	for (std::size_t listIndex = 0; listIndex < 8; ++listIndex)
	{
		std::size_t steps = 0;
		for (auto* object = ObjectLists[listIndex];
			object != nullptr && steps < MaxObjectListTraversal;
			object = object->NextObject, ++steps)
		{
			visit(object);
		}
	}
}

bool IsStageDriveTask(const ObjectMaster* object)
{
	const auto main = reinterpret_cast<std::uintptr_t>(object->MainSub);
	return main == StageDriveMain || main == StageDriveInit;
}

// The stage's drive spawner takes the position in EBX and returns nothing.
void SpawnStageChaosDrive(NJS_VECTOR* position)
{
	const std::uintptr_t function = StageDriveSpawn;
	__asm
	{
		push ebx
		mov ebx, position
		call function
		pop ebx
	}
}

NJS_VECTOR ArenaDrivePosition(std::size_t index)
{
	return { -45.0f + 30.0f * static_cast<float>(index), 4.0f, 40.0f };
}

// The loading screen title card is loaded by stagename.c as "stg_title%02d"
// through LoadCharTextures (0x44C510) from exactly one call site. While the
// arena is active the filename is swapped for the mod's own "Chao Farm" card
// (garden_assets ships stg_titlefarm.prs and PRS\stg_titlefarm.pak).
constexpr std::uintptr_t StageTitleLoadCallSite = 0x4729A2;
constexpr std::uintptr_t LoadCharTexturesAddress = 0x44C510;
const char ArenaTitleName[] = "stg_titlefarm";
volatile bool ArenaTitleActive = false;
bool StageTitleHooked = false;

__declspec(naked) void StageTitleLoadHook()
{
	__asm
	{
		cmp byte ptr [ArenaTitleActive], 0
		je stock
		mov ecx, offset ArenaTitleName
	stock:
		mov eax, LoadCharTexturesAddress
		jmp eax
	}
}

void InstallStageTitleHook()
{
	if (StageTitleHooked)
	{
		return;
	}
	const auto* site = reinterpret_cast<const unsigned char*>(StageTitleLoadCallSite);
	const auto target = StageTitleLoadCallSite + 5 + *reinterpret_cast<const std::int32_t*>(site + 1);
	if (site[0] != 0xE8 || target != LoadCharTexturesAddress)
	{
		Log("Stage title call site did not match; the arena keeps the stock title card.");
		StageTitleHooked = true;
		return;
	}
	unsigned char call[5] = { 0xE8 };
	const auto relative = static_cast<std::int32_t>(
		reinterpret_cast<std::uintptr_t>(&StageTitleLoadHook) - (StageTitleLoadCallSite + 5));
	std::memcpy(call + 1, &relative, sizeof(relative));
	StageTitleHooked = WriteCode(StageTitleLoadCallSite, call, sizeof(call));
	Log(StageTitleHooked ? "Stage title hook installed." : "Stage title hook failed.");
}

// The in-game mission objective is produced by the displayer's loader
// (0x496370, sole call site 0x496CE0): it reads the base stage's Message file,
// parses the string with the game's message parser (0x6B7F40, handle stored at
// 0x1A5A3F4) and measures it (0x6B68F0, usercall with the text in EAX). While
// the arena is active the loader is skipped and the Farm greeting is parsed
// and measured through the same functions.
const char ArenaMissionText[] = "Welcome To The Farm";
constexpr std::uintptr_t MissionTextLoaderCallSites[2] = { 0x496CE0, 0x472B7B };
constexpr std::uintptr_t MissionTextLoader = 0x496370;
constexpr std::uintptr_t MsgParse = 0x6B7F40;
constexpr std::uintptr_t MsgMeasure = 0x6B68F0;
constexpr std::uintptr_t MissionMsgHandle = 0x1A5A3F4;
constexpr std::uintptr_t MissionMsgWidth = 0x19CF354;
constexpr std::uintptr_t MissionMsgHeight = 0x19EDBF8;

void __cdecl ArenaMissionTextApply()
{
	const int lang = *reinterpret_cast<const signed char*>(MenuLanguageByte);
	auto& handle = *reinterpret_cast<void**>(MissionMsgHandle);
	if (handle == nullptr)
	{
		handle = reinterpret_cast<void* (__cdecl*)(const char*, int, int, int)>(MsgParse)(
			ArenaMissionText, lang, -1, -1);
	}
	*reinterpret_cast<std::int16_t*>(MissionMsgWidth) = 0x10;
	*reinterpret_cast<std::int16_t*>(MissionMsgHeight) = 0x10;
	void* parsed = handle;
	if (parsed == nullptr)
	{
		return;
	}
	const char* text = ArenaMissionText;
	__asm
	{
		push MissionMsgHeight
		push MissionMsgWidth
		mov ecx, dword ptr [lang]
		push ecx
		mov eax, dword ptr [parsed]
		push eax
		mov eax, dword ptr [text]
		mov edx, MsgMeasure
		call edx
		add esp, 16
	}
}

__declspec(naked) void MissionTextLoaderHook()
{
	__asm
	{
		cmp byte ptr [ArenaTitleActive], 0
		jne farm
		mov eax, MissionTextLoader
		jmp eax
	farm:
		pushad
		pushfd
		call ArenaMissionTextApply
		popfd
		popad
		ret
	}
}

// The loading/pause objective window (task created by 0x497240, main 0x44EEA0,
// header display 0x496D20 drawing "1st Mission:") was targeted for removal, but
// clearing the display pointer had no visible effect in game, so the attempt
// was abandoned and the window is left stock.


void EnsureMissionTextHook()
{
	bool ok = true;
	for (const auto site : MissionTextLoaderCallSites)
	{
		ok = EnsureCallHook(site, MissionTextLoader,
			reinterpret_cast<const void*>(&MissionTextLoaderHook), nullptr) && ok;
	}
	static bool reported = false;
	if (!reported)
	{
		reported = true;
		Log(ok ? "Mission text hooks installed." : "Mission text hook site mismatch.");
	}
}

// Replaces the stage's placed objects with the arena's own: animals, rings and
// the crate are ordinary SET objects, so the stock streamer loads them.
void ArenaPopulate()
{
	Arena.Entries.clear();
	Arena.Species.clear();
	Arena.DiagFrames = 0;
	const auto ring = FindStageObjectId("RING");
	const auto animal = FindStageObjectId("MINIMAL");
	const auto crate = FindStageObjectId("CONTCHAO");
	const auto goal = FindStageObjectId("GOALRING");
	Log("Arena object ids on level " + std::to_string(Arena.BaseLevel) + ": RING="
		+ std::to_string(ring) + " MINIMAL=" + std::to_string(animal) + " CONTCHAO="
		+ std::to_string(crate) + " GOALRING=" + std::to_string(goal) + ".");
	auto add = [](std::int16_t id, float x, float y, float z, int species)
	{
		if (id < 0)
		{
			return;
		}
		SETEntry entry = {};
		entry.ID = id;
		entry.Position = { x, y, z };
		entry.Scale = { 1.0f, 1.0f, 1.0f };
		Arena.Entries.push_back(entry);
		Arena.Species.push_back(species);
	};
	for (int i = 0; i < ArenaAnimalSpecies; ++i)
	{
		add(animal, -120.0f + 40.0f * static_cast<float>(i % 7), 3.0f,
			120.0f + 40.0f * static_cast<float>(i / 7), i);
	}
	for (int row = 0; row < 3; ++row)
	{
		for (int column = 0; column < 8; ++column)
		{
			add(ring, -105.0f + 30.0f * static_cast<float>(column), 6.0f,
				-90.0f - 30.0f * static_cast<float>(row), -1);
		}
	}
	Arena.CrateIndex = crate < 0 ? SIZE_MAX : Arena.Entries.size();
	add(crate, 0.0f, 0.0f, -210.0f, -1);
	Arena.GoalIndex = goal < 0 ? SIZE_MAX : Arena.Entries.size();
	add(goal, -240.0f, 28.0f, -240.0f, -1);
	// Knuckles and Rouge also hunt three emerald pieces; they stay dormant until
	// the stage's emerald manager has picked its targets (see ArenaArmEmeralds).
	Arena.EmeraldIndex = SIZE_MAX;
	Arena.EmeraldsArmed = false;
	const auto emerald = Arena.BaseLevel == LevelIDs_PumpkinHill
		? FindStageObjectId("EMERALD") : static_cast<std::int16_t>(-1);
	if (emerald >= 0)
	{
		Arena.EmeraldIndex = Arena.Entries.size();
		for (std::size_t i = 0; i < ArenaEmeraldCount; ++i)
		{
			add(emerald, 210.0f + 30.0f * static_cast<float>(i), 5.0f, 240.0f, -1);
			Arena.Entries.back().Scale = { 0.0f, 3.0f, 0.0f };
		}
	}
	for (std::size_t i = 0; i < Arena.Entries.size(); ++i)
	{
		const bool emeraldSlot = Arena.EmeraldIndex != SIZE_MAX && i >= Arena.EmeraldIndex
			&& i < Arena.EmeraldIndex + ArenaEmeraldCount;
		SETObjects[i] = {};
		SETObjects[i].Flags = emeraldSlot ? 0 : SetObjectActive;
		SETObjects[i].SETEntry = &Arena.Entries[i];
	}
	Arena.SeenAlive.assign(Arena.Entries.size(), 0);
	Arena.Drives = {};
	SETObjectCount = static_cast<std::int16_t>(Arena.Entries.size());
	Arena.Populated = true;
	Log("Arena objects placed: " + std::to_string(Arena.Entries.size()) + " SET entries.");
}

// LoadSetObject parses the stage's SET file and the stock streamer can spawn
// nearby placements during the same loading frame, so the arena's entries go
// in straight after the parse instead of on the next frame.
constexpr std::uintptr_t SetParseCallSite = 0x487EE5;
constexpr std::uintptr_t ByteswapSETFileAddress = 0x487FC0;
bool SetParseHooked = false;

void __cdecl ArenaAfterSetParse()
{
	if (Arena.Active && SETFile != nullptr)
	{
		ArenaPopulate();
	}
}

__declspec(naked) void ByteswapSETFileHook()
{
	__asm
	{
		mov ecx, ByteswapSETFileAddress
		call ecx
		pushad
		call ArenaAfterSetParse
		popad
		ret
	}
}

void InstallSetParseHook()
{
	if (SetParseHooked)
	{
		return;
	}
	const auto* site = reinterpret_cast<const unsigned char*>(SetParseCallSite);
	const auto target = SetParseCallSite + 5 + *reinterpret_cast<const std::int32_t*>(site + 1);
	if (site[0] != 0xE8 || target != ByteswapSETFileAddress)
	{
		Log("SET parse call site did not match; arena populates on the next frame.");
		SetParseHooked = true;
		return;
	}
	unsigned char call[5] = { 0xE8 };
	const auto relative = static_cast<std::int32_t>(
		reinterpret_cast<std::uintptr_t>(&ByteswapSETFileHook) - (SetParseCallSite + 5));
	std::memcpy(call + 1, &relative, sizeof(relative));
	SetParseHooked = WriteCode(SetParseCallSite, call, sizeof(call));
	Log(SetParseHooked ? "SET parse hook installed." : "SET parse hook failed.");
}

void ArenaMaintainObjects()
{
	// A placement whose task has vanished was collected or broken: re-arm it.
	std::vector<const SETObjectData*> live;
	ForEachObjectTask([&](ObjectMaster* object)
	{
		if (object->SETData != nullptr)
		{
			live.push_back(object->SETData);
		}
	});
	for (std::size_t i = 0; i < Arena.Entries.size(); ++i)
	{
		auto& slot = SETObjects[i];
		const bool present = std::find(live.begin(), live.end(), &slot) != live.end();
		if (present)
		{
			Arena.SeenAlive[i] = 1;
			continue;
		}
		if (Arena.SeenAlive[i] == 0 || i == Arena.CrateIndex || i == Arena.GoalIndex
			|| (Arena.EmeraldIndex != SIZE_MAX && i >= Arena.EmeraldIndex
				&& i < Arena.EmeraldIndex + ArenaEmeraldCount))
		{
			continue;
		}
		Arena.SeenAlive[i] = 0;
		slot.LoadCount = 0;
		slot.Flags = SetObjectActive;
		slot.Object = nullptr;
	}

	std::vector<ObjectMaster*> drives;
	ForEachObjectTask([&](ObjectMaster* object)
	{
		auto* data = reinterpret_cast<char*>(object->Data1.Undefined);
		if (reinterpret_cast<std::uintptr_t>(object->MainSub) == StageAnimalMain
			&& object->SETData != nullptr && object->SETData->SETEntry != nullptr)
		{
			const auto index = static_cast<std::size_t>(
				object->SETData->SETEntry - Arena.Entries.data());
			auto** extra = reinterpret_cast<int**>(reinterpret_cast<char*>(object) + 0x40);
			if (index < Arena.Species.size() && Arena.Species[index] >= 0
				&& *extra != nullptr)
			{
				(*extra)[1] = Arena.Species[index];
				(*extra)[2] = Arena.Species[index];
			}
		}
		else if (IsStageDriveTask(object) && data != nullptr)
		{
			// The stage's ground query misses the arena floor, so hold drives above it.
			auto* height = reinterpret_cast<float*>(data + 0x18);
			if (*height < 4.0f)
			{
				*height = 4.0f;
			}
			drives.push_back(object);
		}
	});

	if (Arena.DrivesFailed)
	{
		return;
	}
	for (std::size_t i = 0; i < Arena.Drives.size(); ++i)
	{
		if (Arena.Drives[i] != nullptr
			&& std::find(drives.begin(), drives.end(), Arena.Drives[i]) == drives.end())
		{
			Arena.Drives[i] = nullptr;
		}
		if (Arena.Drives[i] != nullptr)
		{
			continue;
		}
		auto position = ArenaDrivePosition(i);
		SpawnStageChaosDrive(&position);
		ObjectMaster* created = nullptr;
		ForEachObjectTask([&](ObjectMaster* object)
		{
			if (IsStageDriveTask(object)
				&& std::find(drives.begin(), drives.end(), object) == drives.end()
				&& std::find(Arena.Drives.begin(), Arena.Drives.end(), object)
					== Arena.Drives.end())
			{
				created = object;
			}
		});
		if (created != nullptr && created->Data1.Undefined != nullptr)
		{
			reinterpret_cast<char*>(created->Data1.Undefined)[3] = static_cast<char>(i);
			Arena.Drives[i] = created;
			drives.push_back(created);
		}
		else
		{
			Arena.DrivesFailed = true;
			Log("Arena Chaos Drive spawn produced no task; drives disabled.");
			return;
		}
	}
}

// The hunting manager tracks its three targets as (container type, location id)
// pairs and clears the stage natively once all are collected. After it picks its
// targets, they become plain ground pieces at the arena's emerald corner and the
// matching SET pieces are switched on, so the radar and the clear both work.
void ArenaArmEmeralds()
{
	if (Arena.EmeraldsArmed || Arena.EmeraldIndex == SIZE_MAX)
	{
		return;
	}
	EmeManObj2* manager = nullptr;
	ForEachObjectTask([&](ObjectMaster* object)
	{
		if (reinterpret_cast<std::uintptr_t>(object->MainSub) == EmeraldManagerMain)
		{
			manager = *reinterpret_cast<EmeManObj2**>(reinterpret_cast<char*>(object) + 0x40);
		}
	});
	if (manager == nullptr || manager->Action != 4)
	{
		return;
	}
	std::string picked;
	for (std::size_t i = 0; i < ArenaEmeraldCount; ++i)
	{
		auto& target = manager->byte2C[i];
		auto& entry = Arena.Entries[Arena.EmeraldIndex + i];
		picked += " " + std::to_string(static_cast<unsigned char>(target.byte0)) + ":"
			+ std::to_string(static_cast<unsigned char>(target.byte1));
		target.byte0 = 0;
		target.v = entry.Position;
		entry.XRot = static_cast<std::int16_t>(static_cast<unsigned char>(target.byte1));
		auto& slot = SETObjects[Arena.EmeraldIndex + i];
		slot.LoadCount = 0;
		slot.Object = nullptr;
		slot.Flags = SetObjectActive;
	}
	Arena.EmeraldsArmed = true;
	Log("Arena emeralds armed; manager picked" + picked + ", remaining "
		+ std::to_string(manager->byte5) + ".");
}

// One-off report of which arena animals have a live task, to track spawns that never appear.
void ArenaLogAnimals()
{
	std::string report;
	int alive = 0;
	for (std::size_t i = 0; i < Arena.Species.size() && i < Arena.SeenAlive.size(); ++i)
	{
		if (Arena.Species[i] < 0)
		{
			continue;
		}
		const auto& slot = SETObjects[i];
		report += " " + std::to_string(Arena.Species[i]) + (Arena.SeenAlive[i] ? "+" : "-")
			+ "f" + std::to_string(static_cast<unsigned short>(slot.Flags))
			+ "l" + std::to_string(slot.LoadCount);
		alive += Arena.SeenAlive[i] ? 1 : 0;
	}
	std::string models;
	for (int i = 0; i < ArenaAnimalSpecies; ++i)
	{
		const auto* model = reinterpret_cast<const std::uint32_t*>(AnimalSlotBuffer + i * 16);
		models += model[1] != 0 ? "1" : "0";
	}
	Log("Arena animals alive " + std::to_string(alive) + ":" + report + " models " + models + ".");
}

// The stage camera loader picks stgNNcam.prs / camsetNNNN from CurrentLevel
// (movsx ebp, word [CurrentLevel] at 0x453AA0). The arena forces level 90,
// whose camera file is empty, so no stage camera zone changes the view.
constexpr std::uintptr_t CameraLevelRead = 0x453AA0;
constexpr unsigned char CameraLevelOriginal[7] = { 0x0F, 0xBF, 0x2D, 0x70, 0x4B, 0x93, 0x01 };
constexpr unsigned char CameraLevelArena[7] = { 0xBD, 0x5A, 0x00, 0x00, 0x00, 0x90, 0x90 };

void SetArenaCameraZonesDisabled(bool disabled)
{
	const auto* site = reinterpret_cast<const unsigned char*>(CameraLevelRead);
	const auto* expected = disabled ? CameraLevelOriginal : CameraLevelArena;
	if (std::memcmp(site, expected, sizeof(CameraLevelOriginal)) != 0)
	{
		return;
	}
	if (WriteCode(CameraLevelRead, disabled ? CameraLevelArena : CameraLevelOriginal,
		sizeof(CameraLevelOriginal)))
	{
		Log(disabled ? "Arena camera zones disabled." : "Stage camera zones restored.");
	}
}

// The HUD draws the time through the call at 0x44EA76; NOPing it hides the
// timer (the push/add esp pair around the call stays balanced).
void SetArenaTimerHidden(bool hidden)
{
	auto* site = reinterpret_cast<unsigned char*>(0x44EA76);
	DWORD oldProtect = 0;
	if (hidden == Arena.TimerHidden
		|| !VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		return;
	}
	if (hidden)
	{
		if (site[0] != 0xE8)
		{
			VirtualProtect(site, 5, oldProtect, &oldProtect);
			Log("Arena timer site did not match; timer left visible.");
			return;
		}
		std::memcpy(Arena.SavedTimerCall, site, 5);
		std::memset(site, 0x90, 5);
	}
	else
	{
		std::memcpy(site, Arena.SavedTimerCall, 5);
	}
	FlushInstructionCache(GetCurrentProcess(), site, 5);
	VirtualProtect(site, 5, oldProtect, &oldProtect);
	Arena.TimerHidden = hidden;
	Log(hidden ? "Arena timer hidden." : "Arena timer restored.");
}

void ArenaEnter()
{
	if (Arena.Active || ModDirectory.empty() || datadllhandle == nullptr
		|| *datadllhandle == nullptr || **datadllhandle == nullptr)
	{
		return;
	}
	try
	{
		Arena.BaseLevel = ArenaBaseLevelFor(CurrentCharacter);
		char exportName[32];
		std::snprintf(exportName, sizeof(exportName), "objLandTable%04d", Arena.BaseLevel);
		Arena.Live = reinterpret_cast<LandTable*>(GetProcAddress(**datadllhandle, exportName));
		if (Arena.Live == nullptr)
		{
			throw std::runtime_error(std::string(exportName) + " export not found.");
		}
		auto land = std::make_unique<sa2lvl::LoadedLandTable>(sa2lvl::Load(
			ModDirectory / "garden_assets" / ArenaLandFileFor(Arena.BaseLevel)));
		// The base stage's own land textures stay loaded: its objects draw from them too.
		land->Table->TextureName = Arena.Live->TextureName;
		land->Table->TextureList = Arena.Live->TextureList;
		Arena.SavedLand = *Arena.Live;
		if (!WriteLandTable(Arena.Live, *land->Table))
		{
			throw std::runtime_error("could not unprotect the land table.");
		}
		Arena.Land = std::move(land);
		Arena.SavedStarts.clear();
		const std::array<std::pair<StartPosition*, int>, 7> arrays{ {
			{ SonicStartArray, 21 }, { ShadowStartArray, 18 },
			{ KnucklesStartArray, 18 }, { RougeStartArray, 17 },
			{ TailsStartArray, 2 }, { MechEggmanStartArray, 22 },
			{ MechTailsStartArray, 20 } } };
		for (const auto& [array, count] : arrays)
		{
			for (int i = 0; i < count; ++i)
			{
				if (array[i].Level == Arena.BaseLevel)
				{
					Arena.SavedStarts.emplace_back(&array[i], array[i]);
					array[i].Position1P = { 0.0f, 5.0f, 0.0f };
					array[i].PositionP1 = { -10.0f, 5.0f, 0.0f };
					array[i].PositionP2 = { 10.0f, 5.0f, 0.0f };
				}
			}
		}
		Arena.Active = true;
		ArenaTitleActive = true;
		InstallSetParseHook();
		InstallStageTitleHook();
		EnsureMissionTextHook();
		RelocateAnimalSlots();
		SetAnimalLoaderArena(true);
		SetArenaTimerHidden(true);
		SetArenaCameraZonesDisabled(true);
		Log("Arena land table applied on " + std::string(exportName) + " for character "
			+ std::to_string(CurrentCharacter) + "; "
			+ std::to_string(Arena.SavedStarts.size()) + " start positions moved.");
	}
	catch (const std::exception& error)
	{
		Log(std::string("Arena not applied: ") + error.what());
	}
}

void ArenaExit()
{
	if (!Arena.Active)
	{
		return;
	}
	SetArenaTimerHidden(false);
	SetArenaCameraZonesDisabled(false);
	SetAnimalLoaderArena(false);
	WriteLandTable(Arena.Live, Arena.SavedLand);
	for (auto& [target, original] : Arena.SavedStarts)
	{
		*target = original;
	}
	Arena.SavedStarts.clear();
	Arena.Land.reset();
	Arena.Active = false;
	ArenaTitleActive = false;
	Arena.Populated = false;
	Arena.WarpRequested = false;
	Arena.DrivesFailed = false;
	Arena.Drives = {};
	Arena.MusicFrames = 0;
	Arena.MusicGiveUp = false;
	Arena.MusicLastSeen[0] = '\0';
	Log("Arena land table and start positions restored.");
}

void ArenaFrame()
{
	if (!Arena.Active)
	{
		return;
	}
	// Stock placements must not spawn; once the stage's SET file is parsed the
	// arena's own entries replace them.
	if (SETFile == nullptr)
	{
		Arena.Populated = false;
		SETObjectCount = 0;
		return;
	}
	if (!Arena.Populated)
	{
		ArenaPopulate();
	}
	if (GameState == GameStates_Ingame && !Arena.WarpRequested)
	{
		ArenaMaintainObjects();
		ArenaArmEmeralds();
		ArenaMaintainMusic();
		if (Arena.DiagFrames == 300)
		{
			ArenaLogAnimals();
		}
		if (++Arena.DiagFrames % 60 == 0 && Arena.DiagFrames <= 600 && MainCharObj1[0] != nullptr)
		{
			const auto& at = MainCharObj1[0]->Position;
			Log("Arena player at (" + std::to_string(at.x) + ", " + std::to_string(at.y) + ", "
				+ std::to_string(at.z) + ") action=" + std::to_string(MainCharObj1[0]->Action)
				+ " level=" + std::to_string(CurrentLevel) + ".");
		}
	}
}

void LogStageSelectionChanges()
{
	static int last = -1;
	const int now = StageSelectLevelSelection;
	static bool arenaPending = false;
	if (GameMode == 2 || (GameMode >= 12 && GameMode <= 14) || GameMode == 7 || GameMode == 8)
	{
		if (arenaPending)
		{
			ArenaExit();
		}
		arenaPending = false;
	}
	static bool suppressEntry = false;
	if (GameMode >= 12 && GameMode <= 14)
	{
		suppressEntry = false;
	}
	if (StageSelectPatched && now == 33 && CurrentLevel == LevelIDs_ChaoWorld
		&& (GameMode == 4 || GameMode == 5) && !suppressEntry)
	{
		if (!arenaPending)
		{
			Log("Arena entry confirmed: redirecting level 90.");
			ArenaEnter();
		}
		arenaPending = true;
	}
	ArenaFrame();
	// The Chao key sets the next level to the Chao World; let that transition through.
	if (arenaPending && NextLevel == LevelIDs_ChaoWorld
		&& GameState == GameStates_GoToNextLevel)
	{
		ArenaExit();
		arenaPending = false;
		suppressEntry = true;
	}
	if (Arena.WarpRequested && arenaPending)
	{
		ArenaExit();
		arenaPending = false;
		NextLevel = LevelIDs_ChaoWorld;
		GameState = GameStates_GoToNextLevel;
	}
	if (arenaPending && CurrentLevel == LevelIDs_ChaoWorld)
	{
		CurrentLevel = static_cast<short>(Arena.BaseLevel);
	}
	if (StageSelectPatched && now != last)
	{
		last = now;
		Log("Stage select cursor on entry " + std::to_string(now) + ".");
	}
	static int lastLevel = -2, lastState = -2, lastChar = -2, lastMode = -2, lastNext = -2;
	const int level = CurrentLevel, state = GameState, chr = CurrentCharacter,
		mode = GameMode, next = NextLevel;
	if (StageSelectPatched && (level != lastLevel || state != lastState
		|| chr != lastChar || mode != lastMode || next != lastNext))
	{
		lastLevel = level; lastState = state; lastChar = chr; lastMode = mode; lastNext = next;
		Log("Flow: sel=" + std::to_string(now) + " level=" + std::to_string(level)
			+ " next=" + std::to_string(next) + " state=" + std::to_string(state)
			+ " mode=" + std::to_string(mode) + " char=" + std::to_string(chr) + ".");
	}
}
extern "C" __declspec(dllexport) void __cdecl OnFrame()
{
	PatchStageSelectOnce();
	UpdateFarmMenuArt();
	EnsureMainMenuButton();
	LogStageSelectionChanges();
	LogChaoMainConstructorPatchSite();
	PatchLobbyLandTables();
	TrySpawnTestLobbyGate();
	ApplyLobbyReturnPosition();
	UpdateLobbyGateSelection();
	LogLobbyGateObjects();
	TryInstallChaoSaveWriteHooks();
	TryInstallNeutralGardenSpawnObserver();
	TryInstallPreviousGardenStageRequestObservers();
	TryInstallOtherGardenStageRequestObservers();
	TryInstallChaoStageActivationObserver();
	TryInstallPlayerGroundQueryObserver();
	TryRestorePendingTestPoolOverlay();
	LogLiveChaoObjectCountChanges();
	LogChaoSaveSidecar();
	LogChaoStateChanges();
	LogGardenEnvironmentRuntimeState();
}


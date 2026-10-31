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
			const auto overlay = TestPoolOverlaySession->Begin(
				SelectedAddedGarden,
				vanillaRecords);
			ActiveTestGarden = SelectedAddedGarden;
			TestPoolChaoSaveFilename = chaoSaveFilename;
			TestPoolSidecarCommitted = false;
			TestPoolCheckpointFailureLogged = false;
			TestPoolOverlayProtectsSave = true;
			try
			{
				WriteChaoSaveRecordsToMemory(overlay);
			}
			catch (const std::exception& writeError)
			{
				try
				{
					WriteChaoSaveRecordsToMemory(TestPoolVanillaRecords);
					TestPoolOverlaySession->Cancel(TestPoolVanillaRecords);
					TestPoolOverlaySession.reset();
					TestPoolOverlayProtectsSave = false;
					TestPoolRestorePending = false;
					TestPoolRestoreFailureLogged = false;
					TestPoolSidecarCommitted = false;
					TestPoolCheckpointFailureLogged = false;
					TestPoolVanillaRecords.clear();
					TestPoolVanillaSaveBlock.clear();
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

	void CompleteTestPoolOverlay()
	{
		if (!TestPoolSidecarCommitted
			&& TestPoolOverlaySession != nullptr
			&& TestPoolOverlaySession->IsActive())
		{
			const auto liveRecords = ReadChaoSaveRecordsFromMemory();
			garden_pools::ChaoSaveRecordTable restoredRecords;
			const auto capturedTestPool = TestPoolOverlaySession->End(
				liveRecords,
				restoredRecords);
			TestPoolSidecarCommitted = true;
			if (restoredRecords != TestPoolVanillaRecords)
			{
				throw std::runtime_error(
					"The saved Neutral Chao records changed during the virtual-garden visit.");
			}
			Log("Committed " + std::to_string(capturedTestPool.size())
				+ " " + garden_pools::SADXStageName(ActiveTestGarden)
				+ " Chao to the save-scoped sidecar.");
		}

		WriteChaoSaveRecordsToMemory(TestPoolVanillaRecords);
		TestPoolOverlaySession.reset();
		TestPoolOverlayProtectsSave = false;
		TestPoolRestorePending = false;
		TestPoolRestoreFailureLogged = false;
		TestPoolSidecarCommitted = false;
		TestPoolCheckpointFailureLogged = false;
		TestPoolVanillaRecords.clear();
		TestPoolVanillaSaveBlock.clear();
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
						const auto checkpoint =
							TestPoolOverlaySession->Checkpoint(
								ReadChaoSaveRecordsFromMemory());
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

struct ArenaState
{
	bool Active = false;
	std::unique_ptr<sa2lvl::LoadedLandTable> Land;
	LandTable* Live = nullptr;
	LandTable SavedLand = {};
	std::vector<std::pair<StartPosition*, StartPosition>> SavedStarts;
} Arena;

void ArenaEnter()
{
	if (Arena.Active || ModDirectory.empty() || datadllhandle == nullptr
		|| *datadllhandle == nullptr || **datadllhandle == nullptr)
	{
		return;
	}
	try
	{
		Arena.Live = reinterpret_cast<LandTable*>(GetProcAddress(
			**datadllhandle, "objLandTable0003"));
		if (Arena.Live == nullptr)
		{
			throw std::runtime_error("objLandTable0003 export not found.");
		}
		auto land = std::make_unique<sa2lvl::LoadedLandTable>(
			sa2lvl::Load(ModDirectory / "garden_assets" / "Arena.sa2blvl"));
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
				if (array[i].Level == LevelIDs_GreenForest)
				{
					Arena.SavedStarts.emplace_back(&array[i], array[i]);
					array[i].Position1P = { 0.0f, 5.0f, 0.0f };
					array[i].PositionP1 = { -10.0f, 5.0f, 0.0f };
					array[i].PositionP2 = { 10.0f, 5.0f, 0.0f };
				}
			}
		}
		Arena.Active = true;
		Log("Arena land table applied; " + std::to_string(Arena.SavedStarts.size())
			+ " start positions moved.");
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
	WriteLandTable(Arena.Live, Arena.SavedLand);
	for (auto& [target, original] : Arena.SavedStarts)
	{
		*target = original;
	}
	Arena.SavedStarts.clear();
	Arena.Land.reset();
	Arena.Active = false;
	Log("Arena land table and start positions restored.");
}

void ArenaFrame()
{
	if (!Arena.Active)
	{
		return;
	}
	static int lastCount = -1;
	if (SETObjectCount != lastCount)
	{
		lastCount = SETObjectCount;
		Log("Arena SET object count " + std::to_string(SETObjectCount) + ".");
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
	if (StageSelectPatched && now == 33 && CurrentLevel == LevelIDs_ChaoWorld
		&& (GameMode == 4 || GameMode == 5))
	{
		if (!arenaPending)
		{
			Log("Arena entry confirmed: redirecting level 90 -> 3.");
			ArenaEnter();
		}
		arenaPending = true;
	}
	ArenaFrame();
	if (arenaPending && CurrentLevel == LevelIDs_ChaoWorld)
	{
		CurrentLevel = LevelIDs_GreenForest;
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


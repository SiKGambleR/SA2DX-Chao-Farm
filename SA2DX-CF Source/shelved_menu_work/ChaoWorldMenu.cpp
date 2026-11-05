#include "ChaoWorldMenu.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>

namespace
{
	constexpr std::uintptr_t MenuIdAddress = 0x1D7BB10;
	constexpr std::uintptr_t MenuSubStateAddress = 0x1D7BB14;
	constexpr std::uintptr_t MainMenuDataPointerAddress = 0xC77F14;
	constexpr std::uintptr_t MenuTransitionPointerAddress = 0xC77F44;
	constexpr std::uintptr_t InputEdgeAddress = 0x1DEFAB0;
	constexpr std::uintptr_t LanguageAddress = 0x1D1BBD0;
	constexpr std::uintptr_t CircleBrightnessAddress = 0x1D1BBF4;
	constexpr std::uintptr_t LabelFrameTableAddress = 0xC77820;
	constexpr std::uintptr_t TexAnimTableAddress = 0xC6A248;
	constexpr std::uintptr_t StageSelectCursorAddress = 0x1D1BF08;
	constexpr std::uintptr_t RunMenusDispatchCallSite = 0x666275;
	constexpr std::uintptr_t RunMenusDispatchTarget = 0x67E620;
	constexpr std::uintptr_t CircleDrawCallSite = 0x669728;
	constexpr std::uintptr_t CircleDrawTarget = 0x6687A0;

	constexpr int MenuMain = 1;
	constexpr int MenuStageSelect = 3;
	constexpr int SubCircles = 4;
	constexpr int SubCirclesConfirm = 5;
	constexpr int SubStageSelectMap = 2;
	constexpr int ChaoWorldStageSelectIndex = 22;
	constexpr int BossCircleIndex = 3;
	constexpr int StageSelectCircleIndex = 1;
	constexpr int NoCircleSelected = -1;
	constexpr int LabelBandTop = 144;
	constexpr int LabelBandBottom = 168;
	constexpr float FifthOffsetX = -160.0f;
	constexpr float FifthOffsetY = -90.0f;
	constexpr int PendingFrameLimit = 900;

	constexpr std::uint8_t InputRight = 0x80;
	constexpr std::uint8_t InputLeft = 0x40;
	constexpr std::uint8_t InputBack = 0x02;
	constexpr std::uint8_t InputConfirm = 0x0C;

	bool FifthSelected = false;
	bool StageSelectPending = false;
	int PendingFrames = 0;

	void* DrawReturnAddress = nullptr;
	int DrawItemIndex = 0;
	int* DrawArguments = nullptr;
	float* DrawItem = nullptr;
	float FifthPosition[3]{};
	std::int16_t SavedFrames[2][4]{};
	int PatchedFrames[2]{ -1, -1 };

	template <typename T>
	T& At(std::uintptr_t address)
	{
		return *reinterpret_cast<T*>(address);
	}

	std::int8_t& CircleSelection()
	{
		return *reinterpret_cast<std::int8_t*>(At<std::uintptr_t>(MainMenuDataPointerAddress) + 0x40);
	}

	void PlaySound(int id)
	{
		__asm
		{
			push esi
			push 0
			push 0
			push 0
			mov esi, id
			mov eax, 0x437260
			call eax
			add esp, 12
			pop esi
		}
	}

	void EnterStageSelectFromCircles()
	{
		PlaySound(0x8001);
		reinterpret_cast<void(__cdecl*)()>(0x664DA0)();
		reinterpret_cast<void(__cdecl*)()>(0x671B80)();
		At<int>(MenuSubStateAddress) = SubCirclesConfirm;
		At<int>(At<std::uintptr_t>(MenuTransitionPointerAddress) + 0x14) = 0;
		CircleSelection() = static_cast<std::int8_t>(StageSelectCircleIndex);
	}

	void __cdecl PreFrame()
	{
		const int menu = At<int>(MenuIdAddress);
		const int sub = At<int>(MenuSubStateAddress);
		auto& input = At<std::uint8_t>(InputEdgeAddress);
		const bool menuReady = At<std::uintptr_t>(MainMenuDataPointerAddress) != 0;

		if (menu == MenuMain && sub == SubCircles && menuReady)
		{
			auto& selection = CircleSelection();
			if (!FifthSelected)
			{
				if (selection == BossCircleIndex && (input & InputRight) != 0)
				{
					input = static_cast<std::uint8_t>(input & ~InputRight);
					FifthSelected = true;
					selection = static_cast<std::int8_t>(NoCircleSelected);
					PlaySound(0x8000);
				}
			}
			else if ((input & InputLeft) != 0)
			{
				input = static_cast<std::uint8_t>(input & ~InputLeft);
				FifthSelected = false;
				selection = static_cast<std::int8_t>(BossCircleIndex);
				PlaySound(0x8000);
			}
			else if ((input & InputRight) != 0)
			{
				input = static_cast<std::uint8_t>(input & ~InputRight);
			}
			else if ((input & InputConfirm) != 0)
			{
				input = static_cast<std::uint8_t>(input & ~InputConfirm);
				FifthSelected = false;
				StageSelectPending = true;
				PendingFrames = 0;
				EnterStageSelectFromCircles();
			}
			else if ((input & InputBack) != 0)
			{
				FifthSelected = false;
				selection = 0;
			}
			else
			{
				selection = static_cast<std::int8_t>(NoCircleSelected);
			}
		}
		else if (FifthSelected)
		{
			FifthSelected = false;
		}

		if (StageSelectPending)
		{
			if (++PendingFrames > PendingFrameLimit || (menu != MenuStageSelect && menu != MenuMain))
			{
				StageSelectPending = false;
			}
			else if (menu == MenuStageSelect && sub == SubStageSelectMap)
			{
				At<std::int8_t>(StageSelectCursorAddress) = static_cast<std::int8_t>(ChaoWorldStageSelectIndex);
				input = static_cast<std::uint8_t>(input | 0x08);
				StageSelectPending = false;
			}
		}
	}

	__declspec(naked) void PreFrameStub()
	{
		__asm
		{
			pushad
			pushfd
			call PreFrame
			popfd
			popad
			push RunMenusDispatchTarget
			ret
		}
	}

	std::int16_t* TexAnimFrame(int frame)
	{
		return reinterpret_cast<std::int16_t*>(TexAnimTableAddress + static_cast<std::uintptr_t>(frame) * 20) + 4;
	}

	void __cdecl BeginFifthCircle()
	{
		FifthPosition[0] = DrawItem[0] + FifthOffsetX;
		FifthPosition[1] = DrawItem[1] + FifthOffsetY;
		FifthPosition[2] = DrawItem[2];

		DrawArguments[2] = FifthSelected ? At<int>(CircleBrightnessAddress) : 0;

		const int row = 16 + At<std::int8_t>(LanguageAddress);
		for (int slot = 0; slot < 2; ++slot)
		{
			const int frame = At<int>(LabelFrameTableAddress + static_cast<std::uintptr_t>(row) * 12 + slot * 4);
			PatchedFrames[slot] = -1;
			if (frame < 0 || frame > 120)
			{
				continue;
			}
			std::int16_t* uv = TexAnimFrame(frame);
			std::memcpy(SavedFrames[slot], uv, sizeof(SavedFrames[slot]));
			uv[1] = LabelBandTop;
			uv[3] = LabelBandBottom;
			PatchedFrames[slot] = frame;
		}
	}

	void __cdecl EndFifthCircle()
	{
		for (int slot = 1; slot >= 0; --slot)
		{
			if (PatchedFrames[slot] >= 0)
			{
				std::memcpy(TexAnimFrame(PatchedFrames[slot]), SavedFrames[slot], sizeof(SavedFrames[slot]));
				PatchedFrames[slot] = -1;
			}
		}
	}

	__declspec(naked) void CircleDrawStub()
	{
		__asm
		{
			pop DrawReturnAddress
			mov DrawItemIndex, ecx
			mov DrawItem, eax
			mov DrawArguments, esp
			mov edx, CircleDrawTarget
			call edx
			cmp DrawItemIndex, 11
			jne finished
			pushad
			call BeginFifthCircle
			popad
			mov eax, offset FifthPosition
			mov ecx, 8
			mov edx, CircleDrawTarget
			call edx
			pushad
			call EndFifthCircle
			popad
		finished:
			push DrawReturnAddress
			ret
		}
	}

	bool PatchCall(std::uintptr_t site, std::uintptr_t expectedTarget, void* replacement)
	{
		auto* bytes = reinterpret_cast<std::uint8_t*>(site);
		if (bytes[0] != 0xE8)
		{
			return false;
		}
		std::int32_t relative;
		std::memcpy(&relative, bytes + 1, sizeof(relative));
		if (static_cast<std::uintptr_t>(site + 5 + relative) != expectedTarget)
		{
			return false;
		}
		DWORD oldProtect = 0;
		if (!VirtualProtect(bytes, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			return false;
		}
		const std::int32_t patched = static_cast<std::int32_t>(
			reinterpret_cast<std::uintptr_t>(replacement) - (site + 5));
		std::memcpy(bytes + 1, &patched, sizeof(patched));
		VirtualProtect(bytes, 5, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), bytes, 5);
		return true;
	}
}

bool InstallChaoWorldMenu()
{
	static bool installed = false;
	if (installed)
	{
		return true;
	}
	if (!PatchCall(RunMenusDispatchCallSite, RunMenusDispatchTarget, reinterpret_cast<void*>(PreFrameStub)))
	{
		return false;
	}
	if (!PatchCall(CircleDrawCallSite, CircleDrawTarget, reinterpret_cast<void*>(CircleDrawStub)))
	{
		return false;
	}
	installed = true;
	return true;
}

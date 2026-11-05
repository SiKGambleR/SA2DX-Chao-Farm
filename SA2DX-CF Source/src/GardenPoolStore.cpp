#include "GardenPoolStore.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace
{
	constexpr std::array<std::uint8_t, 8> FileMagic{ 'S', 'A', '2', 'C', 'G', 'P', '0', '1' };
	// Version 2 appends per-garden ground-item regions; version 1 files are
	// still accepted and load with empty item regions.
	constexpr std::uint32_t FileVersion = 2;
	constexpr std::uint32_t LegacyFileVersion = 1;
	constexpr std::size_t HeaderSize = 24;
	constexpr std::size_t MaxFileBytes = HeaderSize
		+ garden_pools::AddedGardenCount
			* (sizeof(std::uint32_t)
				+ garden_pools::MaxChaoPerGarden
					* (sizeof(std::uint32_t) + garden_pools::MaxRecordBytes))
		+ garden_pools::AddedGardenCount
			* (sizeof(std::uint32_t) + garden_pools::GardenItemRegionSize);
	std::atomic<std::uint32_t> TempFileCounter{ 0 };

	void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
	{
		for (unsigned int shift = 0; shift < 32; shift += 8)
		{
			bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
		}
	}

	std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
	{
		if (bytes.size() - offset < sizeof(std::uint32_t))
		{
			throw std::runtime_error("Garden sidecar is truncated.");
		}

		std::uint32_t value = 0;
		for (unsigned int shift = 0; shift < 32; shift += 8)
		{
			value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
		}
		return value;
	}

	std::uint32_t Crc32(const std::uint8_t* data, std::size_t size)
	{
		std::uint32_t crc = 0xFFFFFFFF;
		for (std::size_t index = 0; index < size; ++index)
		{
			crc ^= data[index];
			for (unsigned int bit = 0; bit < 8; ++bit)
			{
				const std::uint32_t mask = 0U - (crc & 1U);
				crc = (crc >> 1) ^ (0xEDB88320U & mask);
			}
		}
		return ~crc;
	}

	std::system_error LastWindowsError(const char* operation)
	{
		return std::system_error(
			static_cast<int>(GetLastError()),
			std::system_category(),
			operation);
	}

	bool IsValidChaoSavePath(std::string_view path) noexcept
	{
		if (path.empty() || path.size() > 259)
		{
			return false;
		}

		std::size_t segmentStart = 0;
		while (segmentStart < path.size())
		{
			auto segmentEnd = segmentStart;
			while (segmentEnd < path.size()
				&& path[segmentEnd] != '/'
				&& path[segmentEnd] != '\\')
			{
				++segmentEnd;
			}
			if (segmentEnd == segmentStart)
			{
				return false;
			}

			const auto segment = path.substr(
				segmentStart,
				segmentEnd - segmentStart);
			if (segment != ".")
			{
				if (segment == "..")
				{
					return false;
				}
				for (const char character : segment)
				{
					const bool isUppercase = character >= 'A' && character <= 'Z';
					const bool isLowercase = character >= 'a' && character <= 'z';
					const bool isDigit = character >= '0' && character <= '9';
					if (!isUppercase && !isLowercase && !isDigit
						&& character != '_' && character != '-')
					{
						return false;
					}
				}
			}
			else if (segmentStart != 0 || segmentEnd == path.size())
			{
				return false;
			}

			if (segmentEnd == path.size())
			{
				return true;
			}
			segmentStart = segmentEnd + 1;
		}

		return false;
	}

	char FoldAsciiCase(char character) noexcept
	{
		return character >= 'A' && character <= 'Z'
			? static_cast<char>(character - 'A' + 'a')
			: character;
	}

	struct TempFile
	{
		std::filesystem::path path;
		HANDLE handle = INVALID_HANDLE_VALUE;
		bool committed = false;

		~TempFile()
		{
			if (handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(handle);
			}
			if (!committed && !path.empty())
			{
				DeleteFileW(path.c_str());
			}
		}
	};

	TempFile CreateTempFile(const std::filesystem::path& destination)
	{
		static_assert(sizeof(wchar_t) == 2, "The sidecar writer requires Windows UTF-16 paths.");

		const auto parent = destination.has_parent_path()
			? destination.parent_path()
			: std::filesystem::current_path();
		std::filesystem::create_directories(parent);

		for (unsigned int attempt = 0; attempt < 32; ++attempt)
		{
			const auto suffix = L".tmp."
				+ std::to_wstring(GetCurrentProcessId())
				+ L"."
				+ std::to_wstring(TempFileCounter.fetch_add(1));
			auto tempPath = destination;
			tempPath += suffix;

			HANDLE handle = CreateFileW(
				tempPath.c_str(),
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (handle != INVALID_HANDLE_VALUE)
			{
				return TempFile{ tempPath, handle, false };
			}

			const auto error = GetLastError();
			if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
			{
				throw LastWindowsError("Could not create garden sidecar temporary file.");
			}
		}

		throw std::runtime_error("Could not reserve a unique garden sidecar temporary file.");
	}

	void WriteAll(HANDLE handle, const std::vector<std::uint8_t>& bytes)
	{
		std::size_t offset = 0;
		while (offset < bytes.size())
		{
			const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
				bytes.size() - offset,
				(std::numeric_limits<DWORD>::max)()));
			DWORD written = 0;
			if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr))
			{
				throw LastWindowsError("Could not write garden sidecar.");
			}
			if (written == 0)
			{
				throw std::runtime_error("Garden sidecar write made no progress.");
			}
			offset += written;
		}
	}
}

namespace garden_pools
{
	std::string ChaoSaveFilenameFromBuffer(
		const char* filenameBuffer,
		std::size_t bufferSize)
	{
		if (filenameBuffer == nullptr || bufferSize == 0)
		{
			throw std::invalid_argument("The Chao save filename buffer is unavailable.");
		}

		std::size_t length = 0;
		while (length < bufferSize && filenameBuffer[length] != '\0')
		{
			++length;
		}
		if (length == 0 || length == bufferSize)
		{
			throw std::invalid_argument(
				"The Chao save filename must be non-empty and null-terminated.");
		}

		return std::string(filenameBuffer, length);
	}

	bool ChaoSavePathMatches(
		std::string_view activeSavePath,
		std::string_view writerPath) noexcept
	{
		if (!IsValidChaoSavePath(activeSavePath)
			|| !IsValidChaoSavePath(writerPath)
			|| activeSavePath.size() != writerPath.size())
		{
			return false;
		}

		for (std::size_t index = 0; index < activeSavePath.size(); ++index)
		{
			const char activeCharacter =
				activeSavePath[index] == '\\' ? '/' : activeSavePath[index];
			const char writerCharacter =
				writerPath[index] == '\\' ? '/' : writerPath[index];
			if (FoldAsciiCase(activeCharacter)
				!= FoldAsciiCase(writerCharacter))
			{
				return false;
			}
		}
		return true;
	}

	std::filesystem::path SidecarPathForSave(
		const std::filesystem::path& saveDirectory,
		std::string_view chaoSaveFilename)
	{
		if (saveDirectory.empty())
		{
			throw std::invalid_argument("The Chao save directory cannot be empty.");
		}

		const auto separator = chaoSaveFilename.find_last_of("/\\");
		const auto fileName = chaoSaveFilename.substr(
			separator == std::string_view::npos ? 0 : separator + 1);
		if (fileName.empty() || fileName.size() > 64)
		{
			throw std::invalid_argument("The Chao save filename is invalid.");
		}

		for (const char character : fileName)
		{
			const bool isUppercase = character >= 'A' && character <= 'Z';
			const bool isLowercase = character >= 'a' && character <= 'z';
			const bool isDigit = character >= '0' && character <= '9';
			if (!isUppercase && !isLowercase && !isDigit
				&& character != '_' && character != '-')
			{
				throw std::invalid_argument("The Chao save filename contains unsupported characters.");
			}
		}

		std::string sidecarName = "SA2SixGardens-";
		sidecarName.append(fileName.data(), fileName.size());
		sidecarName += ".dat";
		return saveDirectory / std::filesystem::u8path(sidecarName);
	}

	const char* SADXStageName(AddedGarden garden)
	{
		switch (garden)
		{
		case AddedGarden::StationSquare:
			return "AL_GARDEN00";
		case AddedGarden::MysticRuins:
			return "AL_GARDEN01";
		case AddedGarden::EggCarrier:
			return "AL_GARDEN02";
		default:
			throw std::invalid_argument("Unknown added garden.");
		}
	}

	SADXChaoStageNumber SADXStageNumber(AddedGarden garden)
	{
		switch (garden)
		{
		case AddedGarden::StationSquare:
			return SADXChaoStageNumber::StationSquare;
		case AddedGarden::MysticRuins:
			return SADXChaoStageNumber::MysticRuins;
		case AddedGarden::EggCarrier:
			return SADXChaoStageNumber::EggCarrier;
		default:
			throw std::invalid_argument("Unknown added garden.");
		}
	}

	std::optional<AddedGarden> AddedGardenForSADXStageNumber(int stageNumber)
	{
		switch (static_cast<SADXChaoStageNumber>(stageNumber))
		{
		case SADXChaoStageNumber::StationSquare:
			return AddedGarden::StationSquare;
		case SADXChaoStageNumber::MysticRuins:
			return AddedGarden::MysticRuins;
		case SADXChaoStageNumber::EggCarrier:
			return AddedGarden::EggCarrier;
		default:
			return std::nullopt;
		}
	}

	GardenPool& PoolFor(AddedGardenPools& gardens, AddedGarden garden)
	{
		const auto index = static_cast<std::size_t>(garden);
		if (index >= gardens.size())
		{
			throw std::invalid_argument("Unknown added garden.");
		}
		return gardens[index];
	}

	const GardenPool& PoolFor(const AddedGardenPools& gardens, AddedGarden garden)
	{
		const auto index = static_cast<std::size_t>(garden);
		if (index >= gardens.size())
		{
			throw std::invalid_argument("Unknown added garden.");
		}
		return gardens[index];
	}

	ItemRegion& ItemsFor(AddedGardenItemRegions& items, AddedGarden garden)
	{
		const auto index = static_cast<std::size_t>(garden);
		if (index >= items.size())
		{
			throw std::invalid_argument("Unknown added garden.");
		}
		return items[index];
	}

	const ItemRegion& ItemsFor(const AddedGardenItemRegions& items, AddedGarden garden)
	{
		const auto index = static_cast<std::size_t>(garden);
		if (index >= items.size())
		{
			throw std::invalid_argument("Unknown added garden.");
		}
		return items[index];
	}

	std::vector<std::uint8_t> Encode(const AddedGardenPools& gardens)
	{
		return Encode(gardens, AddedGardenItemRegions{});
	}

	std::vector<std::uint8_t> Encode(
		const AddedGardenPools& gardens,
		const AddedGardenItemRegions& items)
	{
		std::vector<std::uint8_t> payload;
		payload.reserve(MaxFileBytes - HeaderSize);

		for (const auto& garden : gardens)
		{
			if (garden.size() > MaxChaoPerGarden)
			{
				throw std::invalid_argument("An added garden cannot contain more than eight Chao.");
			}

			AppendU32(payload, static_cast<std::uint32_t>(garden.size()));
			for (const auto& record : garden)
			{
				if (record.empty())
				{
					throw std::invalid_argument("A Chao sidecar record cannot be empty.");
				}
				if (record.size() > MaxRecordBytes)
				{
					throw std::invalid_argument("A Chao sidecar record exceeds the size limit.");
				}

				AppendU32(payload, static_cast<std::uint32_t>(record.size()));
				payload.insert(payload.end(), record.begin(), record.end());
			}
		}

		for (const auto& region : items)
		{
			if (!region.empty() && region.size() != GardenItemRegionSize)
			{
				throw std::invalid_argument(
					"A garden item region must be empty or exactly the live array size.");
			}
			AppendU32(payload, static_cast<std::uint32_t>(region.size()));
			payload.insert(payload.end(), region.begin(), region.end());
		}

		std::vector<std::uint8_t> bytes;
		bytes.reserve(HeaderSize + payload.size());
		bytes.insert(bytes.end(), FileMagic.begin(), FileMagic.end());
		AppendU32(bytes, FileVersion);
		AppendU32(bytes, static_cast<std::uint32_t>(AddedGardenCount));
		AppendU32(bytes, static_cast<std::uint32_t>(payload.size()));
		AppendU32(bytes, Crc32(payload.data(), payload.size()));
		bytes.insert(bytes.end(), payload.begin(), payload.end());
		return bytes;
	}

	AddedGardenPools Decode(const std::vector<std::uint8_t>& bytes)
	{
		AddedGardenPools gardens;
		AddedGardenItemRegions items;
		DecodeAll(bytes, gardens, items);
		return gardens;
	}

	void DecodeAll(
		const std::vector<std::uint8_t>& bytes,
		AddedGardenPools& gardens,
		AddedGardenItemRegions& items)
	{
		if (bytes.size() < HeaderSize || bytes.size() > MaxFileBytes)
		{
			throw std::runtime_error("Garden sidecar has an invalid size.");
		}
		if (!std::equal(FileMagic.begin(), FileMagic.end(), bytes.begin()))
		{
			throw std::runtime_error("Garden sidecar has an invalid signature.");
		}

		std::size_t offset = FileMagic.size();
		const auto version = ReadU32(bytes, offset);
		if (version != FileVersion && version != LegacyFileVersion)
		{
			throw std::runtime_error("Garden sidecar version is unsupported.");
		}
		if (ReadU32(bytes, offset) != AddedGardenCount)
		{
			throw std::runtime_error("Garden sidecar does not contain three added gardens.");
		}

		const auto payloadSize = ReadU32(bytes, offset);
		const auto expectedCrc = ReadU32(bytes, offset);
		if (payloadSize != bytes.size() - HeaderSize)
		{
			throw std::runtime_error("Garden sidecar payload size is invalid.");
		}
		if (Crc32(bytes.data() + HeaderSize, payloadSize) != expectedCrc)
		{
			throw std::runtime_error("Garden sidecar checksum failed.");
		}

		AddedGardenPools decodedGardens;
		for (auto& garden : decodedGardens)
		{
			const auto count = ReadU32(bytes, offset);
			if (count > MaxChaoPerGarden)
			{
				throw std::runtime_error("Garden sidecar contains more than eight Chao in one garden.");
			}
			garden.reserve(count);

			for (std::uint32_t index = 0; index < count; ++index)
			{
				const auto recordSize = ReadU32(bytes, offset);
				if (recordSize == 0
					|| recordSize > MaxRecordBytes
					|| recordSize > bytes.size() - offset)
				{
					throw std::runtime_error("Garden sidecar contains an invalid Chao record size.");
				}

				garden.emplace_back(
					bytes.begin() + offset,
					bytes.begin() + offset + recordSize);
				offset += recordSize;
			}
		}
		AddedGardenItemRegions decodedItems;
		if (version >= 2)
		{
			for (auto& region : decodedItems)
			{
				const auto regionSize = ReadU32(bytes, offset);
				if ((regionSize != 0 && regionSize != GardenItemRegionSize)
					|| regionSize > bytes.size() - offset)
				{
					throw std::runtime_error(
						"Garden sidecar contains an invalid item region size.");
				}
				region.assign(
					bytes.begin() + offset,
					bytes.begin() + offset + regionSize);
				offset += regionSize;
			}
		}
		if (offset != bytes.size())
		{
			throw std::runtime_error("Garden sidecar contains trailing data.");
		}
		gardens = std::move(decodedGardens);
		items = std::move(decodedItems);
	}

	void Save(const std::filesystem::path& path, const AddedGardenPools& gardens)
	{
		Save(path, gardens, AddedGardenItemRegions{});
	}

	void Save(
		const std::filesystem::path& path,
		const AddedGardenPools& gardens,
		const AddedGardenItemRegions& items)
	{
		const auto bytes = Encode(gardens, items);
		auto temp = CreateTempFile(path);
		WriteAll(temp.handle, bytes);
		if (!FlushFileBuffers(temp.handle))
		{
			throw LastWindowsError("Could not flush garden sidecar.");
		}
		if (!CloseHandle(temp.handle))
		{
			throw LastWindowsError("Could not close garden sidecar.");
		}
		temp.handle = INVALID_HANDLE_VALUE;

		if (!MoveFileExW(
			temp.path.c_str(),
			path.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			throw LastWindowsError("Could not atomically replace garden sidecar.");
		}
		temp.committed = true;
	}

	AddedGardenPools Load(const std::filesystem::path& path)
	{
		AddedGardenPools gardens;
		AddedGardenItemRegions items;
		LoadAll(path, gardens, items);
		return gardens;
	}

	void LoadAll(
		const std::filesystem::path& path,
		AddedGardenPools& gardens,
		AddedGardenItemRegions& items)
	{
		const auto size = std::filesystem::file_size(path);
		if (size < HeaderSize || size > MaxFileBytes)
		{
			throw std::runtime_error("Garden sidecar has an invalid size.");
		}

		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			throw std::runtime_error("Could not open garden sidecar.");
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		input.read(
			reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		if (!input || input.peek() != std::char_traits<char>::eof())
		{
			throw std::runtime_error("Could not read the complete garden sidecar.");
		}
		DecodeAll(bytes, gardens, items);
	}

	AddedGardenPoolTransaction::AddedGardenPoolTransaction(
		std::filesystem::path path)
		: path_(std::move(path))
	{
		if (path_.empty())
		{
			throw std::invalid_argument("The garden sidecar path cannot be empty.");
		}
	}

	void AddedGardenPoolTransaction::Load()
	{
		if (loaded_)
		{
			throw std::logic_error("The garden pool transaction is already loaded.");
		}

		std::error_code error;
		const bool sidecarExists = std::filesystem::exists(path_, error);
		if (error)
		{
			throw std::filesystem::filesystem_error(
				"Could not check whether the garden sidecar exists.",
				path_,
				error);
		}

		auto pools = AddedGardenPools{};
		auto items = AddedGardenItemRegions{};
		if (sidecarExists)
		{
			garden_pools::LoadAll(path_, pools, items);
		}
		committedPools_ = pools;
		workingPools_ = std::move(pools);
		committedItems_ = items;
		workingItems_ = std::move(items);
		loaded_ = true;
	}

	AddedGardenPools& AddedGardenPoolTransaction::WorkingPools()
	{
		EnsureLoaded();
		return workingPools_;
	}

	const AddedGardenPools& AddedGardenPoolTransaction::WorkingPools() const
	{
		EnsureLoaded();
		return workingPools_;
	}

	AddedGardenItemRegions& AddedGardenPoolTransaction::WorkingItems()
	{
		EnsureLoaded();
		return workingItems_;
	}

	const AddedGardenItemRegions& AddedGardenPoolTransaction::WorkingItems() const
	{
		EnsureLoaded();
		return workingItems_;
	}

	bool AddedGardenPoolTransaction::IsDirty() const
	{
		EnsureLoaded();
		return workingPools_ != committedPools_ || workingItems_ != committedItems_;
	}

	void AddedGardenPoolTransaction::Commit()
	{
		EnsureLoaded();
		if (IsDirty())
		{
			auto committedPools = workingPools_;
			auto committedItems = workingItems_;
			garden_pools::Save(path_, workingPools_, workingItems_);
			committedPools_ = std::move(committedPools);
			committedItems_ = std::move(committedItems);
		}
	}

	void AddedGardenPoolTransaction::Rollback()
	{
		EnsureLoaded();
		workingPools_ = committedPools_;
		workingItems_ = committedItems_;
	}

	void AddedGardenPoolTransaction::EnsureLoaded() const
	{
		if (!loaded_)
		{
			throw std::logic_error("The garden pool transaction is not loaded.");
		}
	}
}

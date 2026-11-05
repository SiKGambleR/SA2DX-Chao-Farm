#include "Sa2LvlLoader.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>
#include <string>

namespace sa2lvl
{
	namespace
	{
		constexpr std::uint32_t LandTableSize = 0x20;
		constexpr std::uint32_t ColSize = 0x20;
		constexpr std::uint32_t ObjectSize = 0x34;
		constexpr std::uint32_t BasicAttachSize = 0x28;
		constexpr std::uint32_t BasicMeshSetSize = 0x18;

		class Relocator
		{
		public:
			Relocator(std::vector<std::uint8_t>& image)
				: Image(image)
			{
			}

			std::uint32_t Read(std::uint32_t offset) const
			{
				Require(offset, 4);
				std::uint32_t value;
				std::memcpy(&value, Image.data() + offset, 4);
				return value;
			}

			const std::uint8_t* Bytes() const
			{
				return Image.data();
			}

			void Require(std::uint32_t offset, std::uint32_t size) const
			{
				if (static_cast<std::uint64_t>(offset) + size > Image.size())
				{
					throw std::runtime_error("The .sa2blvl file has an out-of-range pointer.");
				}
			}

			// Converts the file offset stored at fieldOffset into an address.
			// Returns the original offset, or 0 when the field is null.
			std::uint32_t Fix(std::uint32_t fieldOffset)
			{
				const auto value = Read(fieldOffset);
				if (value == 0 || !Fixed.insert(fieldOffset).second)
				{
					return 0;
				}
				Require(value, 4);
				const auto address = static_cast<std::uint32_t>(
					reinterpret_cast<std::uintptr_t>(Image.data()) + value);
				std::memcpy(Image.data() + fieldOffset, &address, 4);
				return value;
			}

		private:
			std::vector<std::uint8_t>& Image;
			std::set<std::uint32_t> Fixed;
		};
	}

	namespace
	{
		constexpr std::uint32_t GcAttachSize = 0x24;
		constexpr std::uint32_t MeshEntrySize = 0x10;
		constexpr std::uint32_t VertexSetSize = 0x10;
		constexpr std::uint8_t Tex0Attribute = 5;
		constexpr std::uint32_t TextureParameterType = 8;

		void RelocateBasicAttach(Relocator& relocator, std::uint32_t attach)
		{
			relocator.Require(attach, BasicAttachSize);
			relocator.Fix(attach);
			relocator.Fix(attach + 4);
			const auto meshSets = relocator.Fix(attach + 0xC);
			relocator.Fix(attach + 0x10);
			std::uint16_t count = 0;
			std::memcpy(&count, relocator.Bytes() + attach + 0x14, sizeof(count));
			if (meshSets == 0)
			{
				return;
			}
			relocator.Require(meshSets, count * BasicMeshSetSize);
			for (std::uint32_t i = 0; i < count; ++i)
			{
				for (const auto field : { 4u, 8u, 0xCu, 0x10u, 0x14u })
				{
					relocator.Fix(meshSets + i * BasicMeshSetSize + field);
				}
			}
		}

		void RelocateGcAttach(Relocator& relocator, std::uint32_t attach,
			std::vector<GcUvSet>& uvSets)
		{
			relocator.Require(attach, GcAttachSize);
			const auto vertexSets = relocator.Fix(attach);
			const auto opaque = relocator.Fix(attach + 8);
			const auto translucent = relocator.Fix(attach + 0xC);
			std::uint16_t counts[2] = {};
			std::memcpy(counts, relocator.Bytes() + attach + 0x10, 4);

			GcUvSet uv;
			for (std::uint32_t i = 0; vertexSets != 0; ++i)
			{
				const auto entry = vertexSets + i * VertexSetSize;
				relocator.Require(entry, VertexSetSize);
				if (relocator.Bytes()[entry] == 0xFF)
				{
					break;
				}
				const auto data = relocator.Fix(entry + 8);
				if (relocator.Bytes()[entry] == Tex0Attribute && data != 0)
				{
					std::uint32_t size = 0;
					std::memcpy(&size, relocator.Bytes() + entry + 12, 4);
					relocator.Require(data, size);
					uv.PairCount = size / 4;
					uv.Data = reinterpret_cast<std::int16_t*>(
						const_cast<std::uint8_t*>(relocator.Bytes()) + data);
				}
			}
			const std::pair<std::uint32_t, std::uint16_t> lists[] = {
				{ opaque, counts[0] }, { translucent, counts[1] } };
			for (const auto& [list, count] : lists)
			{
				if (list == 0)
				{
					continue;
				}
				relocator.Require(list, count * MeshEntrySize);
				for (std::uint32_t i = 0; i < count; ++i)
				{
					const auto parameters = relocator.Fix(list + i * MeshEntrySize);
					relocator.Fix(list + i * MeshEntrySize + 8);
					std::uint32_t parameterCount = 0;
					std::memcpy(&parameterCount,
						relocator.Bytes() + list + i * MeshEntrySize + 4, 4);
					if (parameters == 0)
					{
						continue;
					}
					relocator.Require(parameters, parameterCount * 8);
					for (std::uint32_t p = 0; p < parameterCount; ++p)
					{
						const auto* bytes = relocator.Bytes() + parameters + p * 8;
						std::uint32_t type = 0;
						std::memcpy(&type, bytes, 4);
						if (type == TextureParameterType)
						{
							std::uint16_t id = 0;
							std::memcpy(&id, bytes + 4, 2);
							uv.TextureIds.push_back(id);
						}
					}
				}
			}
			if (uv.Data != nullptr && !uv.TextureIds.empty())
			{
				uv.Base.assign(uv.Data, uv.Data + uv.PairCount * 2);
				uvSets.push_back(std::move(uv));
			}
		}
	}
	LoadedLandTable Load(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			throw std::runtime_error("Could not open " + path.string());
		}
		LoadedLandTable result;
		result.Image.assign(
			std::istreambuf_iterator<char>(stream),
			std::istreambuf_iterator<char>());
		auto& image = result.Image;
		if (image.size() < 16 || std::memcmp(image.data(), "SA2BLVL", 7) != 0)
		{
			throw std::runtime_error("Not an SA2BLVL file: " + path.string());
		}

		Relocator relocator(image);
		const auto landTableOffset = relocator.Read(8);
		relocator.Require(landTableOffset, LandTableSize);

		std::uint16_t colCount;
		std::memcpy(&colCount, image.data() + landTableOffset, 2);
		std::uint16_t gcColCount;
		std::memcpy(&gcColCount, image.data() + landTableOffset + 2, 2);
		if (gcColCount > colCount)
		{
			throw std::runtime_error("The .sa2blvl GC COL count exceeds its COL count.");
		}
		const auto colListOffset = relocator.Fix(landTableOffset + 0x10);
		relocator.Fix(landTableOffset + 0x14);
		relocator.Fix(landTableOffset + 0x18);
		// SA Tools preserves the source game's external texture-list address.
		// The caller supplies the live SA2 texture list instead.
		std::memset(image.data() + landTableOffset + 0x1C, 0, 4);
		relocator.Require(colListOffset, colCount * ColSize);

		std::set<std::uint32_t> objectsSeen;
		std::vector<std::pair<std::uint32_t, bool>> pending;
		for (std::uint32_t i = 0; i < colCount; ++i)
		{
			const auto objectOffset =
				relocator.Fix(colListOffset + i * ColSize + 0x10);
			if (objectOffset != 0)
			{
				pending.emplace_back(objectOffset, i < gcColCount);
			}
		}
		while (!pending.empty())
		{
			const auto [objectOffset, isGcModel] = pending.back();
			pending.pop_back();
			if (!objectsSeen.insert(objectOffset).second)
			{
				continue;
			}
			relocator.Require(objectOffset, ObjectSize);
			const auto attachOffset = relocator.Fix(objectOffset + 4);
			if (attachOffset != 0)
			{
				if (isGcModel)
				{
					RelocateGcAttach(relocator, attachOffset, result.UvSets);
				}
				else
				{
					RelocateBasicAttach(relocator, attachOffset);
				}
			}
			for (const auto link : { 0x2Cu, 0x30u })
			{
				const auto next = relocator.Fix(objectOffset + link);
				if (next != 0)
				{
					pending.emplace_back(next, isGcModel);
				}
			}
		}

		result.Table = reinterpret_cast<LandTable*>(
			image.data() + landTableOffset);
		return result;
	}
}

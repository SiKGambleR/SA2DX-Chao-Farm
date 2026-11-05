// SPDX-License-Identifier: GPL-3.0-only
// Checksum table and routine adapted from dfrood/SA2-Save-Converter,
// Checksum.cs at commit 05f3a606283e4aac131eaa6b87ac1c1ef25e7b3a.
// This test-only translation unit is distributed under GPL-3.0; see LICENSE-GPL-3.0.txt.
#include "ChaoSaveChecksum.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace
{
	constexpr std::array<std::size_t, 4> ChaoSaveChecksumOffsets{
		0xCA69,
		0xCA64,
		0xCA6B,
		0xCA66
	};
	constexpr std::array<std::size_t, 5> ChaoSaveChecksumInputZeroOffsets{
		0xCA64,
		0xCA66,
		0xCA67,
		0xCA69,
		0xCA6B
	};
	constexpr std::array<std::uint32_t, 256> ChaoSaveChecksumTable{
		0x00000000U, 0xC9073096U, 0x920E612CU, 0xA50951BAU,
		0xFF6DC419U, 0xCA6AF48FU, 0x9163A535U, 0xA66495A3U,
		0xFEDB8832U, 0xCFDCB8A4U, 0x94D5E91EU, 0xA3D2D988U,
		0xF9B64C2BU, 0xCCB17CBDU, 0x97B82D07U, 0xA0BF1D91U,
		0xFDB71064U, 0xC4B020F2U, 0x9FB97148U, 0xA8BE41DEU,
		0xF2DAD47DU, 0xC7DDE4EBU, 0x9CD4B551U, 0xABD385C7U,
		0xF36C9856U, 0xC26BA8C0U, 0x9962F97AU, 0xAE65C9ECU,
		0xF4015C4FU, 0xC1066CD9U, 0x9A0F3D63U, 0xAD080DF5U,
		0xFB6E20C8U, 0xD269105EU, 0x896041E4U, 0xBE677172U,
		0xE403E4D1U, 0xD104D447U, 0x8A0D85FDU, 0xBD0AB56BU,
		0xE5B5A8FAU, 0xD4B2986CU, 0x8FBBC9D6U, 0xB8BCF940U,
		0xE2D86CE3U, 0xD7DF5C75U, 0x8CD60DCFU, 0xBBD13D59U,
		0xE6D930ACU, 0xDFDE003AU, 0x84D75180U, 0xB3D06116U,
		0xE9B4F4B5U, 0xDCB3C423U, 0x87BA9599U, 0xB0BDA50FU,
		0xE802B89EU, 0xD9058808U, 0x820CD9B2U, 0xB50BE924U,
		0xEF6F7C87U, 0xDA684C11U, 0x81611DABU, 0xB6662D3DU,
		0xF6DC4190U, 0xFFDB7106U, 0xA4D220BCU, 0x93D5102AU,
		0xC9B18589U, 0xFCB6B51FU, 0xA7BFE4A5U, 0x90B8D433U,
		0xC807C9A2U, 0xF900F934U, 0xA209A88EU, 0x950E9818U,
		0xCF6A0DBBU, 0xFA6D3D2DU, 0xA1646C97U, 0x96635C01U,
		0xCB6B51F4U, 0xF26C6162U, 0xA96530D8U, 0x9E62004EU,
		0xC40695EDU, 0xF101A57BU, 0xAA08F4C1U, 0x9D0FC457U,
		0xC5B0D9C6U, 0xF4B7E950U, 0xAFBEB8EAU, 0x98B9887CU,
		0xC2DD1DDFU, 0xF7DA2D49U, 0xACD37CF3U, 0x9BD44C65U,
		0xCDB26158U, 0xE4B551CEU, 0xBFBC0074U, 0x88BB30E2U,
		0xD2DFA541U, 0xE7D895D7U, 0xBCD1C46DU, 0x8BD6F4FBU,
		0xD369E96AU, 0xE26ED9FCU, 0xB9678846U, 0x8E60B8D0U,
		0xD4042D73U, 0xE1031DE5U, 0xBA0A4C5FU, 0x8D0D7CC9U,
		0xD005713CU, 0xE90241AAU, 0xB20B1010U, 0x850C2086U,
		0xDF68B525U, 0xEA6F85B3U, 0xB166D409U, 0x8661E49FU,
		0xDEDEF90EU, 0xEFD9C998U, 0xB4D09822U, 0x83D7A8B4U,
		0xD9B33D17U, 0xECB40D81U, 0xB7BD5C3BU, 0x80BA6CADU,
		0xEDB88320U, 0xA4BFB3B6U, 0xFFB6E20CU, 0xC8B1D29AU,
		0x92D54739U, 0xA7D277AFU, 0xFCDB2615U, 0xCBDC1683U,
		0x93630B12U, 0xA2643B84U, 0xF96D6A3EU, 0xCE6A5AA8U,
		0x940ECF0BU, 0xA109FF9DU, 0xFA00AE27U, 0xCD079EB1U,
		0x900F9344U, 0xA908A3D2U, 0xF201F268U, 0xC506C2FEU,
		0x9F62575DU, 0xAA6567CBU, 0xF16C3671U, 0xC66B06E7U,
		0x9ED41B76U, 0xAFD32BE0U, 0xF4DA7A5AU, 0xC3DD4ACCU,
		0x99B9DF6FU, 0xACBEEFF9U, 0xF7B7BE43U, 0xC0B08ED5U,
		0x96D6A3E8U, 0xBFD1937EU, 0xE4D8C2C4U, 0xD3DFF252U,
		0x89BB67F1U, 0xBCBC5767U, 0xE7B506DDU, 0xD0B2364BU,
		0x880D2BDAU, 0xB90A1B4CU, 0xE2034AF6U, 0xD5047A60U,
		0x8F60EFC3U, 0xBA67DF55U, 0xE16E8EEFU, 0xD669BE79U,
		0x8B61B38CU, 0xB266831AU, 0xE96FD2A0U, 0xDE68E236U,
		0x840C7795U, 0xB10B4703U, 0xEA0216B9U, 0xDD05262FU,
		0x85BA3BBEU, 0xB4BD0B28U, 0xEFB45A92U, 0xD8B36A04U,
		0x82D7FFA7U, 0xB7D0CF31U, 0xECD99E8BU, 0xDBDEAE1DU,
		0x9B64C2B0U, 0x9263F226U, 0xC96AA39CU, 0xFE6D930AU,
		0xA40906A9U, 0x910E363FU, 0xCA076785U, 0xFD005713U,
		0xA5BF4A82U, 0x94B87A14U, 0xCFB12BAEU, 0xF8B61B38U,
		0xA2D28E9BU, 0x97D5BE0DU, 0xCCDCEFB7U, 0xFBDBDF21U,
		0xA6D3D2D4U, 0x9FD4E242U, 0xC4DDB3F8U, 0xF3DA836EU,
		0xA9BE16CDU, 0x9CB9265BU, 0xC7B077E1U, 0xF0B74777U,
		0xA8085AE6U, 0x990F6A70U, 0xC2063BCAU, 0xF5010B5CU,
		0xAF659EFFU, 0x9A62AE69U, 0xC16BFFD3U, 0xF66CCF45U,
		0xA00AE278U, 0x890DD2EEU, 0xD2048354U, 0xE503B3C2U,
		0xBF672661U, 0x8A6016F7U, 0xD169474DU, 0xE66E77DBU,
		0xBED16A4AU, 0x8FD65ADCU, 0xD4DF0B66U, 0xE3D83BF0U,
		0xB9BCAE53U, 0x8CBB9EC5U, 0xD7B2CF7FU, 0xE0B5FFE9U,
		0xBDBDF21CU, 0x84BAC28AU, 0xDFB39330U, 0xE8B4A3A6U,
		0xB2D03605U, 0x87D70693U, 0xDCDE5729U, 0xEBD967BFU,
		0xB3667A2EU, 0x82614AB8U, 0xD9681B02U, 0xEE6F2B94U,
		0xB40BBE37U, 0x810C8EA1U, 0xDA05DF1BU, 0xED02EF8DU
	};

	std::uint32_t ComputeChaoSaveChecksum(
		const garden_pools::ChaoSaveBlock& chaoSaveBlock)
	{
		std::uint32_t checksum = 0x6368616FU;
		for (std::size_t index = 0; index < chaoSaveBlock.size(); ++index)
		{
			const bool isChecksumByte = std::find(
				ChaoSaveChecksumInputZeroOffsets.begin(),
				ChaoSaveChecksumInputZeroOffsets.end(),
				index) != ChaoSaveChecksumInputZeroOffsets.end();
			const auto byte = isChecksumByte
				? std::uint8_t{ 0 }
				: chaoSaveBlock[index];
			const auto tableIndex = static_cast<std::uint8_t>(
				byte ^ static_cast<std::uint8_t>(checksum));
			checksum = ChaoSaveChecksumTable[tableIndex] ^ (checksum >> 8);
		}
		return checksum ^ 0x686F6765U;
	}

	void ValidateChaoSaveBlock(const garden_pools::ChaoSaveBlock& chaoSaveBlock)
	{
		if (chaoSaveBlock.size() != garden_pools::ChaoSaveBlockSize)
		{
			throw std::invalid_argument(
				"A Chao save block must contain exactly 0xCA6C bytes.");
		}
	}

	std::uint32_t StoredChaoSaveChecksum(
		const garden_pools::ChaoSaveBlock& chaoSaveBlock)
	{
		return static_cast<std::uint32_t>(chaoSaveBlock[ChaoSaveChecksumOffsets[0]])
			| (static_cast<std::uint32_t>(chaoSaveBlock[ChaoSaveChecksumOffsets[1]]) << 8)
			| (static_cast<std::uint32_t>(chaoSaveBlock[ChaoSaveChecksumOffsets[2]]) << 16)
			| (static_cast<std::uint32_t>(chaoSaveBlock[ChaoSaveChecksumOffsets[3]]) << 24);
	}

}

namespace garden_pools
{
	bool HasValidChaoSaveChecksum(const ChaoSaveBlock& chaoSaveBlock)
	{
		ValidateChaoSaveBlock(chaoSaveBlock);
		return StoredChaoSaveChecksum(chaoSaveBlock)
			== ComputeChaoSaveChecksum(chaoSaveBlock);
	}

	void UpdateChaoSaveChecksum(ChaoSaveBlock& chaoSaveBlock)
	{
		ValidateChaoSaveBlock(chaoSaveBlock);
		const auto checksum = ComputeChaoSaveChecksum(chaoSaveBlock);
		for (std::size_t index = 0; index < ChaoSaveChecksumOffsets.size(); ++index)
		{
			chaoSaveBlock[ChaoSaveChecksumOffsets[index]] =
				static_cast<std::uint8_t>(checksum >> (index * 8));
		}
	}

}

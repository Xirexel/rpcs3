﻿#include "stdafx.h"
#include "Emu/Memory/vm.h"
#include "TextureUtils.h"
#include "../RSXThread.h"
#include "../rsx_utils.h"

namespace
{
	// FIXME: GSL as_span break build if template parameter is non const with current revision.
	// Replace with true as_span when fixed.
	template <typename T>
	gsl::span<T> as_span_workaround(gsl::span<gsl::byte> unformated_span)
	{
		return{ (T*)unformated_span.data(), ::narrow<int>(unformated_span.size_bytes() / sizeof(T)) };
	}

	template <typename T>
	gsl::span<T> as_const_span(gsl::span<const gsl::byte> unformated_span)
	{
		return{ (T*)unformated_span.data(), ::narrow<int>(unformated_span.size_bytes() / sizeof(T)) };
	}

	// TODO: Make this function part of GSL
	// Note: Doesn't handle overlapping range detection.
	template<typename T1, typename T2>
	constexpr void copy(gsl::span<T1> dst, gsl::span<T2> src)
	{
		if (std::is_same<T1, T2>::value)
		{
			std::memcpy(dst.data(), src.data(), src.size_bytes());
		}
		else
		{
			static_assert(std::is_convertible<T1, T2>::value, "Cannot convert source and destination span type.");
			verify(HERE), (dst.size() == src.size());
			std::copy(src.begin(), src.end(), dst.begin());
		}
	}

	u16 convert_rgb655_to_rgb565(const u16 bits)
	{
		// g6 = g5
		// r5 = (((bits & 0xFC00) >> 1) & 0xFC00) << 1 is equivalent to truncating the least significant bit
		return (bits & 0xF81F) | (bits & 0x3E0) << 1;
	}

struct copy_unmodified_block
{
	template<typename T, typename U>
	static void copy_mipmap_level(gsl::span<T> dst, gsl::span<const U> src, u16 words_per_block, u16 width_in_block, u16 row_count, u16 depth, u32 dst_pitch_in_block, u32 src_pitch_in_block)
	{
		static_assert(sizeof(T) == sizeof(U), "Type size doesn't match.");

		const u32 width_in_words = width_in_block * words_per_block;
		const u32 src_pitch_in_words = src_pitch_in_block * words_per_block;
		const u32 dst_pitch_in_words = dst_pitch_in_block * words_per_block;

		u32 src_offset = 0, dst_offset = 0;
		for (int row = 0; row < row_count * depth; ++row)
		{
			copy(dst.subspan(dst_offset, width_in_words), src.subspan(src_offset, width_in_words));

			src_offset += src_pitch_in_words;
			dst_offset += dst_pitch_in_words;
		}
	}
};

struct copy_unmodified_block_swizzled
{
	// NOTE: Pixel channel types are T (out) and const U (in). V is the pixel block type that consumes one whole pixel.
	// e.g 4x16-bit format can use u16, be_t<u16>, u64 as arguments
	template<typename T, typename U>
	static void copy_mipmap_level(gsl::span<T> dst, gsl::span<const U> src, u16 words_per_block, u16 width_in_block, u16 row_count, u16 depth, u32 dst_pitch_in_block)
	{
		if (std::is_same<T, U>::value && dst_pitch_in_block == width_in_block && words_per_block == 1)
		{
			rsx::convert_linear_swizzle_3d<T>((void*)src.data(), (void*)dst.data(), width_in_block, row_count, depth);
		}
		else
		{
			std::vector<U> tmp(width_in_block * 2 * words_per_block * row_count * depth);
			if (LIKELY(words_per_block == 1))
			{
				rsx::convert_linear_swizzle_3d<T>((void*)src.data(), tmp.data(), width_in_block, row_count, depth);
			}
			else
			{
				switch (words_per_block * sizeof(T))
				{
				case 4:
					rsx::convert_linear_swizzle_3d<u32>((void*)src.data(), tmp.data(), width_in_block, row_count, depth);
					break;
				case 8:
					rsx::convert_linear_swizzle_3d<u64>((void*)src.data(), tmp.data(), width_in_block, row_count, depth);
					break;
				case 16:
					rsx::convert_linear_swizzle_3d<u128>((void*)src.data(), tmp.data(), width_in_block, row_count, depth);
					break;
				default:
					fmt::throw_exception("Failed to decode swizzled format, words_per_block=%d, src_type_size=%d", words_per_block, sizeof(T));
				}
			}

			gsl::span<const U> src_span = tmp;
			copy_unmodified_block::copy_mipmap_level(dst, src_span, words_per_block, width_in_block, row_count, depth, dst_pitch_in_block, width_in_block);
		}
	}
};

struct copy_unmodified_block_vtc
{
	template<typename T, typename U>
	static void copy_mipmap_level(gsl::span<T> dst, gsl::span<const U> src, u16 width_in_block, u16 row_count, u16 depth, u32 dst_pitch_in_block, u32 src_pitch_in_block)
	{
		static_assert(sizeof(T) == sizeof(U), "Type size doesn't match.");
		u32 row_element_count = width_in_block * row_count;
		u32 dst_offset = 0;
		u32 src_offset = 0;
		const u16 depth_4 = (depth >> 2) * 4;	// multiple of 4

		// Undo Nvidia VTC tiling - place each 2D texture slice back to back in linear memory
		//
		// More info:
		// https://www.khronos.org/registry/OpenGL/extensions/NV/NV_texture_compression_vtc.txt
		//
		// Note that the memory is tiled 4 planes at a time in the depth direction.
		// e.g.  d0, d1, d2, d3 is tiled as a group then d4, d5, d6, d7
		//

		//  Tile as 4x4x4
		for (int d = 0; d < depth_4; d++)
		{
			// Copy one slice of the 3d texture
			for (u32 i = 0; i < row_element_count; i += 1)
			{
				// Copy one span (8 bytes for DXT1 or 16 bytes for DXT5)
				copy(dst.subspan(dst_offset + i, 1), src.subspan(src_offset + i * 4, 1));
			}

			dst_offset += row_element_count;

			// Last plane in the group of 4?
			if ((d & 0x3) == 0x3)
			{
				// Move forward to next group of 4 planes
				src_offset += row_element_count * 4 - 3;
			}
			else
			{
				src_offset += 1;
			}
		}

		// End Case - tile as 4x4x3 or 4x4x2 or 4x4x1
		const int vtc_tile_count = depth - depth_4;
		for (int d = 0; d < vtc_tile_count; d++)
		{
			// Copy one slice of the 3d texture
			for (u32 i = 0; i < row_element_count; i += 1)
			{
				// Copy one span (8 bytes for DXT1 or 16 bytes for DXT5)
				copy(dst.subspan(dst_offset + i, 1), src.subspan(src_offset + i * vtc_tile_count, 1));
			}

			dst_offset += row_element_count;
			src_offset += 1;
		}
	}
};

struct copy_decoded_rb_rg_block
{
	template<typename T, typename U>
	static void copy_mipmap_level(gsl::span<T> dst, gsl::span<const U> src, u16 width_in_block, u16 row_count, u16 depth, u32 dst_pitch_in_block, u32 src_pitch_in_block)
	{
		static_assert(sizeof(T) == 4, "Type size doesn't match.");
		static_assert(sizeof(U) == 2, "Type size doesn't match.");

		u32 src_offset = 0;
		u32 dst_offset = 0;
		for (int row = 0; row < row_count * depth; ++row)
		{
			for (int col = 0; col < width_in_block; col += 2)
			{
				// Process 2 pixels at a time and write in BGRA format
				const u16 src0 = src[src_offset + col];     // R,B
				const u16 src1 = src[src_offset + col + 1]; // R,G
				const u32 blue = (src0 & 0xFF00) >> 8;
				const u32 green = (src1 & 0xFF00);
				const u32 data0 = blue | green | (src0 & 0xFF) << 16 | 0xFF << 24;
				const u32 data1 = blue | green | (src1 & 0xFF) << 16 | 0xFF << 24;

				dst[dst_offset + col] = data0;
				if (!(width_in_block & 0x1))
				{
					// If size is even, fill in the second pixel
					dst[dst_offset + col + 1] = data1;
				}
			}

			src_offset += src_pitch_in_block;
			dst_offset += dst_pitch_in_block;
		}
	}
};

struct copy_rgb655_block
{
	template<typename T, typename U>
	static void copy_mipmap_level(gsl::span<T> dst, gsl::span<const U> src, u16 width_in_block, u16 row_count, u16 depth, u32 dst_pitch_in_block, u32 src_pitch_in_block)
	{
		static_assert(sizeof(T) == sizeof(U), "Type size doesn't match.");

		u32 src_offset = 0, dst_offset = 0;
		for (int row = 0; row < row_count * depth; ++row)
		{
			for (int col = 0; col < width_in_block; ++col)
			{
				dst[dst_offset + col] = convert_rgb655_to_rgb565(src[src_offset + col]);
			}

			src_offset += src_pitch_in_block;
			dst_offset += dst_pitch_in_block;
		}
	}
};

struct copy_rgb655_block_swizzled
{
	template<typename T, typename U>
	static void copy_mipmap_level(gsl::span<T> dst, gsl::span<const U> src, u16 width_in_block, u16 row_count, u16 depth, u32 dst_pitch_in_block)
	{
		std::vector<U> tmp(width_in_block * row_count * depth);
		rsx::convert_linear_swizzle_3d<U>((void*)src.data(), tmp.data(), width_in_block, row_count, depth);

		gsl::span<const U> src_span = tmp;
		copy_rgb655_block::copy_mipmap_level(dst, src_span, width_in_block, row_count, depth, dst_pitch_in_block, width_in_block);
	}
};

namespace
{
	/**
	 * Texture upload template.
	 *
	 * Source textures are stored as following (for power of 2 textures):
	 * - For linear texture every mipmap level share rowpitch (which is the one of mipmap 0). This means that for non 0 mipmap there's padding between row.
	 * - For swizzled texture row pitch is texture width X pixel/block size. There's not padding between row.
	 * - There is no padding between 2 mipmap levels. This means that next mipmap level starts at offset rowpitch X row count
	 * - Cubemap images are 128 bytes aligned.
	 *
	 * The template iterates over all depth (including cubemap) and over all mipmaps.
	 * Sometimes texture provides a pitch even if texture is swizzled (and then packed) and in such case it's ignored. It's passed via suggested_pitch and is used only if padded_row is false.
	 */
	template <u8 block_edge_in_texel, typename SRC_TYPE>
	std::vector<rsx_subresource_layout> get_subresources_layout_impl(const gsl::byte *texture_data_pointer, u16 width_in_texel, u16 height_in_texel, u16 depth, u8 layer_count, u16 mipmap_count, u32 suggested_pitch_in_bytes, bool padded_row)
	{
		/**
		* Note about size type: RSX texture width is stored in a 16 bits int and pitch is stored in a 20 bits int.
		*/

		// <= 128 so fits in u8
		u8 block_size_in_bytes = sizeof(SRC_TYPE);

		std::vector<rsx_subresource_layout> result;
		size_t offset_in_src = 0;
		// Always lower than width/height so fits in u16
		u16 texture_height_in_block = (height_in_texel + block_edge_in_texel - 1) / block_edge_in_texel;
		u16 texture_width_in_block = (width_in_texel + block_edge_in_texel - 1) / block_edge_in_texel;
		for (unsigned layer = 0; layer < layer_count; layer++)
		{
			u16 miplevel_height_in_block = texture_height_in_block, miplevel_width_in_block = texture_width_in_block;
			for (unsigned mip_level = 0; mip_level < mipmap_count; mip_level++)
			{
				rsx_subresource_layout current_subresource_layout = {};
				// Since <= width/height, fits on 16 bits
				current_subresource_layout.height_in_block = miplevel_height_in_block;
				current_subresource_layout.width_in_block = miplevel_width_in_block;
				current_subresource_layout.depth = depth;
				// src_pitch in texture can uses 20 bits so fits on 32 bits int.
				u32 src_pitch_in_block = padded_row ? suggested_pitch_in_bytes / block_size_in_bytes : miplevel_width_in_block;
				current_subresource_layout.pitch_in_block = src_pitch_in_block;

				current_subresource_layout.data = gsl::span<const gsl::byte>(texture_data_pointer + offset_in_src, src_pitch_in_block * block_size_in_bytes * miplevel_height_in_block * depth);

				result.push_back(current_subresource_layout);
				offset_in_src += miplevel_height_in_block * src_pitch_in_block * block_size_in_bytes * depth;
				miplevel_height_in_block = std::max(miplevel_height_in_block / 2, 1);
				miplevel_width_in_block = std::max(miplevel_width_in_block / 2, 1);
			}
			offset_in_src = align(offset_in_src, 128);
		}
		return result;
	}
}

template<typename T>
u32 get_row_pitch_in_block(u16 width_in_block, size_t multiple_constraints_in_byte)
{
	size_t divided = (width_in_block * sizeof(T) + multiple_constraints_in_byte - 1) / multiple_constraints_in_byte;
	return static_cast<u32>(divided * multiple_constraints_in_byte / sizeof(T));
}

u32 get_row_pitch_in_block(u16 block_size_in_bytes, u16 width_in_block, size_t multiple_constraints_in_byte)
{
	size_t divided = (width_in_block * block_size_in_bytes + multiple_constraints_in_byte - 1) / multiple_constraints_in_byte;
	return static_cast<u32>(divided * multiple_constraints_in_byte / block_size_in_bytes);
}

/**
 * Since rsx ignore unused dimensionnality some app set them to 0.
 * Use 1 value instead to be more general.
 */
template<typename RsxTextureType>
std::tuple<u16, u16, u8> get_height_depth_layer(const RsxTextureType &tex)
{
	switch (tex.get_extended_texture_dimension())
	{
	case rsx::texture_dimension_extended::texture_dimension_1d: return std::make_tuple(1, 1, 1);
	case rsx::texture_dimension_extended::texture_dimension_2d: return std::make_tuple(tex.height(), 1, 1);
	case rsx::texture_dimension_extended::texture_dimension_cubemap: return std::make_tuple(tex.height(), 1, 6);
	case rsx::texture_dimension_extended::texture_dimension_3d: return std::make_tuple(tex.height(), tex.depth(), 1);
	}
	fmt::throw_exception("Unsupported texture dimension" HERE);
}
}

template<typename RsxTextureType>
std::vector<rsx_subresource_layout> get_subresources_layout_impl(const RsxTextureType &texture)
{
	u16 w = texture.width();
	u16 h;
	u16 depth;
	u8 layer;

	std::tie(h, depth, layer) = get_height_depth_layer(texture);

	int format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);

	const u32 texaddr = rsx::get_address(texture.offset(), texture.location());
	auto pixels = reinterpret_cast<const gsl::byte*>(vm::_ptr<const u8>(texaddr));
	bool is_swizzled = !(texture.format() & CELL_GCM_TEXTURE_LN);
	switch (format)
	{
	case CELL_GCM_TEXTURE_B8:
		return get_subresources_layout_impl<1, u8>(pixels, w, h, depth, layer, texture.get_exact_mipmap_count(), texture.pitch(), !is_swizzled);
	case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
	case CELL_GCM_TEXTURE_DEPTH16:
	case CELL_GCM_TEXTURE_DEPTH16_FLOAT: // Untested
	case CELL_GCM_TEXTURE_D1R5G5B5:
	case CELL_GCM_TEXTURE_A1R5G5B5:
	case CELL_GCM_TEXTURE_A4R4G4B4:
	case CELL_GCM_TEXTURE_R5G5B5A1:
	case CELL_GCM_TEXTURE_R5G6B5:
	case CELL_GCM_TEXTURE_R6G5B5:
	case CELL_GCM_TEXTURE_G8B8:
	case CELL_GCM_TEXTURE_X16:
		return get_subresources_layout_impl<1, u16>(pixels, w, h, depth, layer, texture.get_exact_mipmap_count(), texture.pitch(), !is_swizzled);
	case CELL_GCM_TEXTURE_DEPTH24_D8: // Untested
	case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT: // Untested
	case CELL_GCM_TEXTURE_D8R8G8B8:
	case CELL_GCM_TEXTURE_A8R8G8B8:
	case CELL_GCM_TEXTURE_Y16_X16:
	case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
	case CELL_GCM_TEXTURE_X32_FLOAT:
		return get_subresources_layout_impl<1, u32>(pixels, w, h, depth, layer, texture.get_exact_mipmap_count(), texture.pitch(), !is_swizzled);
	case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
		return get_subresources_layout_impl<1, u64>(pixels, w, h, depth, layer, texture.get_exact_mipmap_count(), texture.pitch(), !is_swizzled);
	case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
		return get_subresources_layout_impl<1, u128>(pixels, w, h, depth, layer, texture.get_exact_mipmap_count(), texture.pitch(), !is_swizzled);
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
		return get_subresources_layout_impl<4, u64>(pixels, w, h, depth, layer, texture.get_exact_mipmap_count(), texture.pitch(), !is_swizzled);
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
		return get_subresources_layout_impl<4, u128>(pixels, w, h, depth, layer, texture.get_exact_mipmap_count(), texture.pitch(), !is_swizzled);
	}
	fmt::throw_exception("Wrong format 0x%x" HERE, format);
}

std::vector<rsx_subresource_layout> get_subresources_layout(const rsx::fragment_texture &texture)
{
	return get_subresources_layout_impl(texture);
}

std::vector<rsx_subresource_layout> get_subresources_layout(const rsx::vertex_texture &texture)
{
	return get_subresources_layout_impl(texture);
}

void upload_texture_subresource(gsl::span<gsl::byte> dst_buffer, const rsx_subresource_layout &src_layout, int format, bool is_swizzled, bool vtc_support, size_t dst_row_pitch_multiple_of)
{
	u16 w = src_layout.width_in_block;
	u16 h = src_layout.height_in_block;
	u16 depth = src_layout.depth;
	u32 pitch = src_layout.pitch_in_block;

	// Ignore when texture width > pitch
	if (w > pitch)
		return;

	// NOTE: Avoid block optimization for formats that can be modified internally by the GPU itself
	// Since the gpu code does not attempt to do wide translations (e.g WZYX32->XYZW32), only perform, per-channel transform and use proper swizzles to get the proper output
	switch (format)
	{
	case CELL_GCM_TEXTURE_B8:
	{
		if (is_swizzled)
			copy_unmodified_block_swizzled::copy_mipmap_level(as_span_workaround<u8>(dst_buffer), as_const_span<const u8>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u8>(w, dst_row_pitch_multiple_of));
		else
			copy_unmodified_block::copy_mipmap_level(as_span_workaround<u8>(dst_buffer), as_const_span<const u8>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u8>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		break;
	}

	case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	{
		copy_decoded_rb_rg_block::copy_mipmap_level(as_span_workaround<u32>(dst_buffer), as_const_span<const be_t<u16>>(src_layout.data), w, h, depth, get_row_pitch_in_block<u32>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		break;
	}

	case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
	{
		copy_decoded_rb_rg_block::copy_mipmap_level(as_span_workaround<u32>(dst_buffer), as_const_span<const u16>(src_layout.data), w, h, depth, get_row_pitch_in_block<u32>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		break;
	}

	case CELL_GCM_TEXTURE_R6G5B5:
	{
		if (is_swizzled)
			copy_rgb655_block_swizzled::copy_mipmap_level(as_span_workaround<u16>(dst_buffer), as_const_span<const be_t<u16>>(src_layout.data), w, h, depth, get_row_pitch_in_block<u16>(w, dst_row_pitch_multiple_of));
		else
			copy_rgb655_block::copy_mipmap_level(as_span_workaround<u16>(dst_buffer), as_const_span<const be_t<u16>>(src_layout.data), w, h, depth, get_row_pitch_in_block<u16>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		break;
	}

	case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
		// TODO: Test if the HILO compressed formats support swizzling (other compressed_* formats ignore this option)
	case CELL_GCM_TEXTURE_DEPTH16:
	case CELL_GCM_TEXTURE_DEPTH16_FLOAT: // Untested
	case CELL_GCM_TEXTURE_D1R5G5B5:
	case CELL_GCM_TEXTURE_A1R5G5B5:
	case CELL_GCM_TEXTURE_A4R4G4B4:
	case CELL_GCM_TEXTURE_R5G5B5A1:
	case CELL_GCM_TEXTURE_R5G6B5:
	case CELL_GCM_TEXTURE_G8B8:
	{
		if (is_swizzled)
			copy_unmodified_block_swizzled::copy_mipmap_level(as_span_workaround<u16>(dst_buffer), as_const_span<const be_t<u16>>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u16>(w, dst_row_pitch_multiple_of));
		else
			copy_unmodified_block::copy_mipmap_level(as_span_workaround<u16>(dst_buffer), as_const_span<const be_t<u16>>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u16>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		break;
	}

	case CELL_GCM_TEXTURE_DEPTH24_D8:
	case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT: // Untested
	{
		if (is_swizzled)
			copy_unmodified_block_swizzled::copy_mipmap_level(as_span_workaround<u32>(dst_buffer), as_const_span<const be_t<u32>>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u32>(w, dst_row_pitch_multiple_of));
		else
			copy_unmodified_block::copy_mipmap_level(as_span_workaround<u32>(dst_buffer), as_const_span<const be_t<u32>>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u32>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		break;
	}

	case CELL_GCM_TEXTURE_A8R8G8B8:
	case CELL_GCM_TEXTURE_D8R8G8B8:
	{
		if (is_swizzled)
			copy_unmodified_block_swizzled::copy_mipmap_level(as_span_workaround<u32>(dst_buffer), as_const_span<const u32>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u32>(w, dst_row_pitch_multiple_of));
		else
			copy_unmodified_block::copy_mipmap_level(as_span_workaround<u32>(dst_buffer), as_const_span<const u32>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u32>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		break;
	}

	// NOTE: Textures with WZYX notations refer to arbitrary data and not color swizzles as in common GPU lang
	// WZYX actually maps directly as a RGBA16 format in Cell memory! R=W, not R=X

	case CELL_GCM_TEXTURE_X16:
	case CELL_GCM_TEXTURE_Y16_X16:
	case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
	case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
	{
		const u16 block_size = get_format_block_size_in_bytes(format);
		const u16 words_per_block = block_size / 2;
		const auto dst_pitch_in_block = get_row_pitch_in_block(block_size, w, dst_row_pitch_multiple_of);

		if (is_swizzled)
			copy_unmodified_block_swizzled::copy_mipmap_level(as_span_workaround<u16>(dst_buffer), as_const_span<const be_t<u16>>(src_layout.data), words_per_block, w, h, depth, dst_pitch_in_block);
		else
			copy_unmodified_block::copy_mipmap_level(as_span_workaround<u16>(dst_buffer), as_const_span<const be_t<u16>>(src_layout.data), words_per_block, w, h, depth, dst_pitch_in_block, src_layout.pitch_in_block);
		break;
	}

	case CELL_GCM_TEXTURE_X32_FLOAT:
	case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
	{
		const u16 block_size = get_format_block_size_in_bytes(format);
		const u16 words_per_block = block_size / 4;
		const auto dst_pitch_in_block = get_row_pitch_in_block(block_size, w, dst_row_pitch_multiple_of);

		if (is_swizzled)
			copy_unmodified_block_swizzled::copy_mipmap_level(as_span_workaround<u32>(dst_buffer), as_const_span<const be_t<u32>>(src_layout.data), words_per_block, w, h, depth, dst_pitch_in_block);
		else
			copy_unmodified_block::copy_mipmap_level(as_span_workaround<u32>(dst_buffer), as_const_span<const be_t<u32>>(src_layout.data), words_per_block, w, h, depth, dst_pitch_in_block, src_layout.pitch_in_block);
		break;
	}

	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
	{
		if (depth > 1 && !vtc_support)
		{
			// PS3 uses the Nvidia VTC memory layout for compressed 3D textures.
			// This is only supported using Nvidia OpenGL.
			// Remove the VTC tiling to support ATI and Vulkan.
			copy_unmodified_block_vtc::copy_mipmap_level(as_span_workaround<u64>(dst_buffer), as_const_span<const u64>(src_layout.data), w, h, depth, get_row_pitch_in_block<u64>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		}
		else
		{
			copy_unmodified_block::copy_mipmap_level(as_span_workaround<u64>(dst_buffer), as_const_span<const u64>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u64>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		}
		break;
	}

	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
	{
		if (depth > 1 && !vtc_support)
		{
			// PS3 uses the Nvidia VTC memory layout for compressed 3D textures.
			// This is only supported using Nvidia OpenGL.
			// Remove the VTC tiling to support ATI and Vulkan.
			copy_unmodified_block_vtc::copy_mipmap_level(as_span_workaround<u128>(dst_buffer), as_const_span<const u128>(src_layout.data), w, h, depth, get_row_pitch_in_block<u128>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		}
		else
		{
			copy_unmodified_block::copy_mipmap_level(as_span_workaround<u128>(dst_buffer), as_const_span<const u128>(src_layout.data), 1, w, h, depth, get_row_pitch_in_block<u128>(w, dst_row_pitch_multiple_of), src_layout.pitch_in_block);
		}
		break;
	}

	default:
		fmt::throw_exception("Wrong format 0x%x" HERE, format);
	}
}

/**
 * A texture is stored as an array of blocks, where a block is a pixel for standard texture
 * but is a structure containing several pixels for compressed format
 */
u8 get_format_block_size_in_bytes(int format)
{
	switch (format)
	{
	case CELL_GCM_TEXTURE_B8: return 1;
	case CELL_GCM_TEXTURE_X16:
	case CELL_GCM_TEXTURE_G8B8:
	case CELL_GCM_TEXTURE_R6G5B5:
	case CELL_GCM_TEXTURE_R5G6B5:
	case CELL_GCM_TEXTURE_D1R5G5B5:
	case CELL_GCM_TEXTURE_R5G5B5A1:
	case CELL_GCM_TEXTURE_A1R5G5B5:
	case CELL_GCM_TEXTURE_A4R4G4B4:
	case CELL_GCM_TEXTURE_DEPTH16:
	case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8: return 2;
	case CELL_GCM_TEXTURE_A8R8G8B8:
	case CELL_GCM_TEXTURE_D8R8G8B8:
	case CELL_GCM_TEXTURE_DEPTH24_D8:
	case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
	case CELL_GCM_TEXTURE_X32_FLOAT:
	case CELL_GCM_TEXTURE_Y16_X16:
	case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
	case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8: return 4;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
	case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT: return 8;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
	case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT: return 16;
	default:
		LOG_ERROR(RSX, "Unimplemented block size in bytes for texture format: 0x%x", format);
		return 1;
	}
}

u8 get_format_block_size_in_texel(int format)
{
	switch (format)
	{
	case CELL_GCM_TEXTURE_B8:
	case CELL_GCM_TEXTURE_G8B8:
	case CELL_GCM_TEXTURE_D8R8G8B8:
	case CELL_GCM_TEXTURE_D1R5G5B5:
	case CELL_GCM_TEXTURE_A1R5G5B5:
	case CELL_GCM_TEXTURE_A4R4G4B4:
	case CELL_GCM_TEXTURE_A8R8G8B8:
	case CELL_GCM_TEXTURE_R5G5B5A1:
	case CELL_GCM_TEXTURE_R6G5B5:
	case CELL_GCM_TEXTURE_R5G6B5:
	case CELL_GCM_TEXTURE_DEPTH24_D8:
	case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
	case CELL_GCM_TEXTURE_DEPTH16:
	case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
	case CELL_GCM_TEXTURE_X16:
	case CELL_GCM_TEXTURE_Y16_X16:
	case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
	case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
	case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
	case CELL_GCM_TEXTURE_X32_FLOAT:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
	case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
	case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
	case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8: return 1;
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45: return 4;
	default:
		LOG_ERROR(RSX, "Unimplemented block size in texels for texture format: 0x%x", format);
		return 1;
	}
}

u8 get_format_block_size_in_bytes(rsx::surface_color_format format)
{
	switch (format)
	{
	case rsx::surface_color_format::b8:
		return 1;
	case rsx::surface_color_format::g8b8:
	case rsx::surface_color_format::r5g6b5:
	case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
	case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
		return 2;
	case rsx::surface_color_format::a8b8g8r8:
	case rsx::surface_color_format::a8r8g8b8:
	case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
	case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
	case rsx::surface_color_format::x8r8g8b8_o8r8g8b8:
	case rsx::surface_color_format::x8r8g8b8_z8r8g8b8:
	case rsx::surface_color_format::x32:
		return 4;
	case rsx::surface_color_format::w16z16y16x16:
		return 8;
	case rsx::surface_color_format::w32z32y32x32:
		return 16;
	default:
		fmt::throw_exception("Invalid color format 0x%x" HERE, (u32)format);
	}
}

u8 get_format_sample_count(rsx::surface_antialiasing antialias)
{
	switch (antialias)
	{
		case rsx::surface_antialiasing::center_1_sample:
			return 1;
		case rsx::surface_antialiasing::diagonal_centered_2_samples:
			return 2;
		case rsx::surface_antialiasing::square_centered_4_samples:
		case rsx::surface_antialiasing::square_rotated_4_samples:
			return 4;
		default:
			ASSUME(0);
			return 0;
	}
}

/**
 * Returns number of texel lines decoded in one pitch-length number of bytes
 */
u8 get_format_texel_rows_per_line(u32 format)
{
	switch (format)
	{
	case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
	case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
		// Layout is 4x4 blocks, i.e one row of pitch bytes in length actually encodes 4 texel rows
		return 4;
	default:
		return 1;
	}
}

u32 get_format_packed_pitch(u32 format, u16 width)
{
	const auto texels_per_block = get_format_block_size_in_texel(format);
	const auto bytes_per_block = get_format_block_size_in_bytes(format);

	return ((width + texels_per_block - 1) / texels_per_block) * bytes_per_block;
}

size_t get_placed_texture_storage_size(u16 width, u16 height, u32 depth, u8 format, u16 mipmap, bool cubemap, size_t row_pitch_alignment, size_t mipmap_alignment)
{
	format &= ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
	size_t block_edge = get_format_block_size_in_texel(format);
	size_t block_size_in_byte = get_format_block_size_in_bytes(format);

	size_t height_in_blocks = (height + block_edge - 1) / block_edge;
	size_t width_in_blocks  = (width + block_edge - 1) / block_edge;

	size_t result = 0;
	for (u16 i = 0; i < mipmap; ++i)
	{
		size_t rowPitch = align(block_size_in_byte * width_in_blocks, row_pitch_alignment);
		result += align(rowPitch * height_in_blocks * depth, mipmap_alignment);
		height_in_blocks = std::max<size_t>(height_in_blocks / 2, 1);
		width_in_blocks = std::max<size_t>(width_in_blocks / 2, 1);
	}

	// Mipmap, height and width aren't allowed to be zero
	return verify("Texture params" HERE, result) * (cubemap ? 6 : 1); 
}

size_t get_placed_texture_storage_size(const rsx::fragment_texture &texture, size_t row_pitch_alignment, size_t mipmap_alignment)
{
	return get_placed_texture_storage_size(texture.width(), texture.height(), texture.depth(), texture.format(), texture.mipmap(), texture.cubemap(),
		row_pitch_alignment, mipmap_alignment);
}

size_t get_placed_texture_storage_size(const rsx::vertex_texture &texture, size_t row_pitch_alignment, size_t mipmap_alignment)
{
	return get_placed_texture_storage_size(texture.width(), texture.height(), texture.depth(), texture.format(), texture.mipmap(), texture.cubemap(),
		row_pitch_alignment, mipmap_alignment);
}

static size_t get_texture_size(u32 format, u16 width, u16 height, u16 depth, u32 pitch, u16 mipmaps, u16 layers)
{
	const auto gcm_format = format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
	const bool packed = !(format & CELL_GCM_TEXTURE_LN);
	const auto texel_rows_per_line = get_format_texel_rows_per_line(gcm_format);

	if (!pitch && !packed)
	{
		if (width > 1 || height > 1)
		{
			// If width == 1, the scanning just returns texel 0, so it is a valid setup
			LOG_ERROR(RSX, "Invalid texture pitch setup, width=%d, height=%d, format=0x%x(0x%x)",
				width, height, format, gcm_format);
		}

		pitch = get_format_packed_pitch(gcm_format, width);
	}

	u32 size = 0;
	if (!packed)
	{
		// Constant pitch layout, simple scanning
		const u32 internal_height = (height + texel_rows_per_line - 1) / texel_rows_per_line;  // Convert texels to blocks
		for (u32 layer = 0; layer < layers; ++layer)
		{
			u32 mip_height = internal_height;
			for (u32 mipmap = 0; mipmap < mipmaps && mip_height > 0; ++mipmap)
			{
				size += pitch * mip_height * depth;
				mip_height = std::max(mip_height / 2u, 1u);
			}
		}
	}
	else
	{
		// Variable pitch per mipmap level
		const auto texels_per_block = get_format_block_size_in_texel(gcm_format);
		const auto bytes_per_block = get_format_block_size_in_bytes(gcm_format);

		const u32 internal_height = (height + texel_rows_per_line - 1) / texel_rows_per_line;  // Convert texels to blocks
		const u32 internal_width = (width + texels_per_block - 1) / texels_per_block;          // Convert texels to blocks
		for (u32 layer = 0; layer < layers; ++layer)
		{
			u32 mip_height = internal_height;
			u32 mip_width = internal_width;
			for (u32 mipmap = 0; mipmap < mipmaps && mip_height > 0; ++mipmap)
			{
				size += (mip_width * bytes_per_block * mip_height * depth);
				mip_height = std::max(mip_height / 2u, 1u);
				mip_width = std::max(mip_width / 2u, 1u);
			}
		}
	}

	return size;
}

size_t get_texture_size(const rsx::fragment_texture &texture)
{
	return get_texture_size(texture.format(), texture.width(), texture.height(), texture.depth(),
			texture.pitch(), texture.get_exact_mipmap_count(), texture.cubemap() ? 6 : 1);
}

size_t get_texture_size(const rsx::vertex_texture &texture)
{
	return get_texture_size(texture.format(), texture.width(), texture.height(), texture.depth(),
		texture.pitch(), texture.get_exact_mipmap_count(), texture.cubemap() ? 6 : 1);
}

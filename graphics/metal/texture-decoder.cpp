/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/metal/texture-decoder.h"
#include "common/endian.h"
#include "common/util.h"

namespace Graphics {
namespace Metal {

static void unpack565(uint16 value, byte *color) {
	uint r = value >> 11, g = (value >> 5) & 63, b = value & 31;
	color[0] = (r << 3) | (r >> 2);
	color[1] = (g << 2) | (g >> 4);
	color[2] = (b << 3) | (b >> 2);
	color[3] = 255;
}

bool decodeS3TC(const void *data, uint bytes, uint width, uint height,
		S3TCFormat format, Surface &surface) {
	if (!data || !width || !height || width > 16384 || height > 16384 || format > kDXT5)
		return false;
	uint blockSize = format == kDXT1 ? 8 : 16;
	uint blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
	if ((uint64)blocksX * blocksY * blockSize > bytes)
		return false;
	surface.free();
	surface.create(width, height, PixelFormat::createFormatRGBA32());
	const byte *block = static_cast<const byte *>(data);
	for (uint by = 0; by < blocksY; ++by) {
		for (uint bx = 0; bx < blocksX; ++bx, block += blockSize) {
			const byte *rgb = block + (format == kDXT1 ? 0 : 8);
			uint16 first = READ_LE_UINT16(rgb), second = READ_LE_UINT16(rgb + 2);
			byte colors[4][4] = {};
			unpack565(first, colors[0]); unpack565(second, colors[1]);
			bool transparent = format == kDXT1 && first <= second;
			for (uint channel = 0; channel < 3; ++channel) {
				colors[2][channel] = transparent ? (colors[0][channel] + colors[1][channel]) / 2 :
					(2 * colors[0][channel] + colors[1][channel]) / 3;
				colors[3][channel] = transparent ? 0 : (colors[0][channel] + 2 * colors[1][channel]) / 3;
			}
			colors[2][3] = 255; colors[3][3] = transparent ? 0 : 255;
			uint32 selectors = READ_LE_UINT32(rgb + 4);
			byte alphas[8] = {block[0], block[1], 0, 0, 0, 0, 0, 0};
			uint64 alphaBits = 0;
			if (format == kDXT4 || format == kDXT5) {
				uint count = alphas[0] > alphas[1] ? 6 : 4;
				for (uint i = 1; i <= count; ++i)
					alphas[i + 1] = ((count + 1 - i) * alphas[0] + i * alphas[1]) / (count + 1);
				if (count == 4) { alphas[6] = 0; alphas[7] = 255; }
				for (uint i = 0; i < 6; ++i) alphaBits |= (uint64)block[i + 2] << (8 * i);
			} else if (format == kDXT2 || format == kDXT3) {
				for (uint i = 0; i < 8; ++i) alphaBits |= (uint64)block[i] << (8 * i);
			}
			for (uint p = 0; p < 16; ++p) {
				uint x = bx * 4 + p % 4, y = by * 4 + p / 4;
				if (x >= width || y >= height) continue;
				const byte *color = colors[(selectors >> (2 * p)) & 3];
				byte a = color[3];
				if (format == kDXT2 || format == kDXT3) a = ((alphaBits >> (4 * p)) & 15) * 17;
				else if (format == kDXT4 || format == kDXT5) a = alphas[(alphaBits >> (3 * p)) & 7];
				byte r = color[0], g = color[1], b = color[2];
				if (format == kDXT2 || format == kDXT4) {
					r = a ? MIN(255U, (uint)r * 255 / a) : 0;
					g = a ? MIN(255U, (uint)g * 255 / a) : 0;
					b = a ? MIN(255U, (uint)b * 255 / a) : 0;
				}
				*(uint32 *)surface.getBasePtr(x, y) = surface.format.ARGBToColor(a, r, g, b);
			}
		}
	}
	return true;
}

} // End of namespace Metal
} // End of namespace Graphics

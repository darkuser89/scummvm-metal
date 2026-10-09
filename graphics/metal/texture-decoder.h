/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_TEXTURE_DECODER_H
#define GRAPHICS_METAL_TEXTURE_DECODER_H

#include "graphics/surface.h"

namespace Graphics {
namespace Metal {

enum S3TCFormat { kDXT1, kDXT2, kDXT3, kDXT4, kDXT5 };

// Decode the top mip level into RGBA8, including partial edge blocks.
bool decodeS3TC(const void *data, uint bytes, uint width, uint height,
                S3TCFormat format, Surface &surface);

} // End of namespace Metal
} // End of namespace Graphics

#endif

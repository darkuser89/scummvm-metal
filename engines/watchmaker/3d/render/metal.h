/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WATCHMAKER_RENDER_METAL_H
#define WATCHMAKER_RENDER_METAL_H

#include "watchmaker/3d/render/opengl_renderer.h"

namespace Watchmaker {

bool usingMetal();
Texture *createMetalTexture();
void renderMetalTexture(const gTexture &texture, const Common::Rect &source,
                        const Common::Rect &destination, uint width, uint height);

} // End of namespace Watchmaker

#endif

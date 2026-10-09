/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_IMGUI_H
#define GRAPHICS_METAL_IMGUI_H

#include "graphics/metal/renderer.h"

struct ImDrawData;
struct ImTextureData;
struct ImGuiViewport;
struct ImVec2;

namespace Graphics {
namespace Metal {

class ImGuiRenderer : public Common::NonCopyable {
public:
	explicit ImGuiRenderer(Renderer &renderer);
	~ImGuiRenderer();
	void render(ImDrawData *data);

private:
	void updateTexture(ImTextureData &texture);
	void renderDrawData(Renderer &renderer, ImDrawData *data);
	static void createWindow(ImGuiViewport *viewport);
	static void destroyWindow(ImGuiViewport *viewport);
	static void resizeWindow(ImGuiViewport *viewport, ImVec2 size);
	static void renderWindow(ImGuiViewport *viewport, void *argument);
	static void presentWindow(ImGuiViewport *viewport, void *argument);
	Renderer &_renderer;
};

} // End of namespace Metal
} // End of namespace Graphics

#endif

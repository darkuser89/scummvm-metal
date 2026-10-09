/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STARK_GFX_METAL_H
#define STARK_GFX_METAL_H

#include "stark/gfx/driver.h"
#include "stark/gfx/renderentry.h"
#include "graphics/metal/renderer.h"
#include "math/matrix4.h"

namespace Stark {
namespace Gfx {

struct MetalLight {
	float position[4], direction[4], color[4], parameters[4];
};

struct MetalUniforms {
	float modelView[16], projection[16], normal[16];
	MetalLight lights[10];
	float boneRotation[70][4], bonePosition[70][4];
	float ambient[4], color[4], shadowDirection[4];
	uint32 textured, doubleSided;
	float fadeLevel;
	uint32 padding;
};

class MetalDriver : public Driver {
public:
	MetalDriver();
	~MetalDriver() override;
	void init() override;
	void setScreenViewport(bool noScaling) override;
	void setViewport(const Common::Rect &rect) override;
	void clearScreen() override;
	void flipBuffer() override;
	Texture *createTexture() override;
	Bitmap *createBitmap(const Graphics::Surface *surface, const byte *palette) override;
	VisualActor *createActorRenderer() override;
	VisualProp *createPropRenderer() override;
	SurfaceRenderer *createSurfaceRenderer() override;
	FadeRenderer *createFadeRenderer() override;
	Graphics::Surface *getViewportScreenshot(uint w, uint h) const override;
	void set3DMode() override {}
	bool computeLightsEnabled() override { return true; }
	void setupUniforms(MetalUniforms &uniforms, const Math::Matrix4 &model, const LightEntryArray &lights) const;
	Common::Rect unscaledViewport() const { return _unscaledViewport; }
	Graphics::Metal::Renderer &renderer() const { return *_renderer; }
	Graphics::Metal::Program *actorProgram() const { return _actor; }
	Graphics::Metal::Program *propProgram() const { return _prop; }
	Graphics::Metal::Program *shadowProgram() const { return _shadow; }
	Graphics::Metal::Program *surfaceProgram() const { return _surface; }

private:
	Graphics::Metal::Renderer *_renderer;
	Graphics::Metal::Program *_actor = nullptr, *_prop = nullptr, *_shadow = nullptr, *_surface = nullptr;
	Common::Rect _unscaledViewport;
};

} // End of namespace Gfx
} // End of namespace Stark

#endif

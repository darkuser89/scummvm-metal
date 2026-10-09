/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BACKENDS_GRAPHICS_METALSDL_GRAPHICS_H
#define BACKENDS_GRAPHICS_METALSDL_GRAPHICS_H

#include "backends/graphics/surfacesdl/surfacesdl-graphics.h"
#include "graphics/metal/renderer.h"
#include "graphics/metal/retro/pipeline.h"
#include <SDL_metal.h>
#ifdef USE_IMGUI
#include "graphics/metal/imgui.h"
#endif

class MetalSdlGraphicsManager : public SurfaceSdlGraphicsManager {
public:
	MetalSdlGraphicsManager(SdlEventSource *eventSource, SdlWindow *window, Graphics::Metal::Renderer &renderer);
	~MetalSdlGraphicsManager() override;
	bool hasFeature(OSystem::Feature feature) const override;
	const OSystem::GraphicsMode *getSupportedGraphicsModes() const override;
	bool setGraphicsMode(int mode, uint flags = OSystem::kGfxModeNoFlags) override;
	bool setShader(const Common::Path &fileName) override;
	void beginGFXTransaction() override;
	OSystem::TransactionError endGFXTransaction() override;
	void initSize(uint width, uint height, const Graphics::PixelFormat *format = nullptr) override;
	int16 getWidth() const override;
	int16 getHeight() const override;
	SdlGraphicsManager::State getState() const override;
	void updateScreen() override;
	void presentBuffer() override;
	bool saveScreenshot(const Common::Path &filename) const override;
	Graphics::Surface *lockScreen() override;
	void showOverlay(bool inGUI) override;
	void hideOverlay() override;
	void clearOverlay() override;
	Graphics::Metal::Renderer *getMetalRenderer() override { return &_metal; }
	void notifyResize(int width, int height) override;
	void destroyingWindow() override;
	float getHiDPIScreenFactor() const override;
#ifdef USE_IMGUI
	void *getImGuiTexture(const Graphics::Surface &image, const byte *palette, int palCount) override;
	void freeImGuiTexture(void *texture) override;
#endif

protected:
	SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags) override;
	void SDL_UpdateRects(SDL_Surface *screen, int numrects, SDL_Rect *rects) override;
	void presentScreen() override;
	void internUpdateScreen() override;
	SDL_Surface *createOffscreenSurface(int width, int height) override;
	bool needsDisplayUpdate() const override { return _composition != nullptr; }
#ifdef USE_IMGUI
	bool initMetalImGui() override;
	void renderMetalImGui() override;
	void destroyMetalImGui() override;
#endif

private:
	enum { kMetalFXMode = 1 };
	Graphics::Metal::Renderer &_metal;
	Graphics::Metal::Texture *_texture;
	SDL_MetalView _view;
	bool _render3D;
	bool _oldRender3D = false;
	Graphics::Surface _gameFrame;
	Graphics::Surface _scaledFrame;
	Graphics::Metal::RenderTarget *_composition;
	Graphics::Metal::Texture *_cursorTexture;
	bool _cursorTextureDirty;
	Graphics::Metal::Texture *_gameTexture = nullptr;
	Graphics::Metal::Texture *_scaledGameTexture = nullptr;
	Graphics::Metal::Texture *_paletteTexture = nullptr;
	Graphics::Metal::RenderTarget *_paletteTarget = nullptr;
	Graphics::Metal::Program *_paletteProgram = nullptr;
	bool _gameTextureIsIndexed = false;
	Graphics::Metal::Retro::Pipeline *_shader = nullptr;
	Graphics::Metal::RenderTarget *_shaderInput = nullptr;
	Common::Path _shaderPath, _oldShaderPath;
	bool _shaderCursorComposed = false;
#ifdef USE_IMGUI
	Graphics::Metal::ImGuiRenderer *_imGuiMetal = nullptr;
#endif
	void captureGameFrame();
	void releaseRenderResources();
	void gameRenderSize(uint &width, uint &height) const;
	void drawNative2D();
	void drawInterface();
	void drawCursor(const Common::Rect *shaderRect = nullptr);
	void drawGameTexture(Graphics::Metal::Texture *texture, const Common::Rect &rect);
	bool loadShader();
	void blitCursor() override;
	Graphics::Metal::Texture *textureFromSurface(SDL_Surface *surface);
	Common::Rect mapScreenRect(const Common::Rect &source) const;
	void drawTexture(Graphics::Metal::Texture *texture, const Common::Rect &rect, bool linear, bool blend, float alpha = 1, bool rotate = true);
};

#endif

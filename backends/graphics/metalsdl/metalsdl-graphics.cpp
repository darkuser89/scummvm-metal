/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "backends/graphics/metalsdl/metalsdl-graphics.h"
#include "common/textconsole.h"
#include "common/file.h"
#include "common/zip-set.h"
#include "image/bmp.h"
#include "graphics/scalerplugin.h"
#include "engines/engine.h"
#include "gui/debugger.h"
#ifdef USE_PNG
#include "image/png.h"
#endif
#ifdef USE_IMGUI
#include "backends/imgui/imgui.h"
#include "backends/imgui/backends/imgui_impl_sdl2.h"
#endif

MetalSdlGraphicsManager::MetalSdlGraphicsManager(SdlEventSource *eventSource, SdlWindow *window, Graphics::Metal::Renderer &renderer) :
	SurfaceSdlGraphicsManager(eventSource, window), _metal(renderer), _texture(nullptr), _view(nullptr),
	_render3D(false), _composition(nullptr), _cursorTexture(nullptr), _cursorTextureDirty(true) {}

MetalSdlGraphicsManager::~MetalSdlGraphicsManager() {
	destroyingWindow();
	delete _shader; _shader = nullptr;
	_gameFrame.free();
}

bool MetalSdlGraphicsManager::saveScreenshot(const Common::Path &filename) const {
	Graphics::Metal::Renderer &renderer = const_cast<Graphics::Metal::Renderer &>(_metal);
	Graphics::Metal::Texture *screen = _composition ? &_composition->texture() : renderer.screenTexture();
	if (!screen) return false;
	Graphics::Surface surface;
	if (!renderer.readTexture(*screen, surface, Common::Rect(screen->width(), screen->height())))
		return false;
	Common::DumpFile output;
	bool success = output.open(filename);
	if (success) {
#ifdef USE_PNG
		success = Image::writePNG(output, surface);
#else
		success = Image::writeBMP(output, surface);
#endif
	}
	surface.free();
	return success;
}

bool MetalSdlGraphicsManager::hasFeature(OSystem::Feature feature) const {
	if (feature == OSystem::kFeatureShaders)
		return Graphics::Metal::Renderer::isSupported();
	if (feature == OSystem::kFeatureMetalForGame)
		return Graphics::Metal::Renderer::isSupported();
	return SurfaceSdlGraphicsManager::hasFeature(feature);
}

const OSystem::GraphicsMode *MetalSdlGraphicsManager::getSupportedGraphicsModes() const {
	static const OSystem::GraphicsMode modes[] = {
		{"metal", "Metal 3", GFX_SURFACESDL},
		{"metal-fx", "Metal 3 + MetalFX (Spatial)", kMetalFXMode}, {nullptr, nullptr, 0}
	};
	static const OSystem::GraphicsMode basic[] = {{"metal", "Metal 3", GFX_SURFACESDL}, {nullptr, nullptr, 0}};
	static const bool supportsMetalFX = Graphics::Metal::Renderer::isMetalFXSupported();
	return supportsMetalFX ? modes : basic;
}

bool MetalSdlGraphicsManager::setGraphicsMode(int mode, uint flags) {
	if ((flags & OSystem::kGfxModeRender3d) && !(flags & OSystem::kGfxModeMetal))
		return false;
	if (!SurfaceSdlGraphicsManager::setGraphicsMode(mode))
		return false;
	bool render3D = flags & OSystem::kGfxModeRender3d;
	if (render3D != _render3D || (render3D && _videoMode.mode != _oldVideoMode.mode))
		_transactionDetails.needHotswap = true;
	_render3D = render3D;
	return true;
}

void MetalSdlGraphicsManager::beginGFXTransaction() {
	_oldShaderPath = _shaderPath;
	_oldRender3D = _render3D;
	SurfaceSdlGraphicsManager::beginGFXTransaction();
}

bool MetalSdlGraphicsManager::setShader(const Common::Path &fileName) {
	assert(_transactionMode == kTransactionActive);
	Common::Path path = fileName == Common::Path("default", Common::Path::kNoSeparator) ? Common::Path() : fileName;
	if (_render3D && _videoMode.mode == kMetalFXMode && path.empty() != _shaderPath.empty())
		_transactionDetails.needHotswap = true;
	_shaderPath = path;
	return true;
}

bool MetalSdlGraphicsManager::loadShader() {
	if (_shaderPath.empty()) { delete _shader; _shader = nullptr; return true; }
	Common::SearchSet archives;
	Common::generateZipSet(archives, "shaders.dat", "shaders*.dat");
	Graphics::Metal::Retro::Pipeline *candidate = new Graphics::Metal::Retro::Pipeline(_metal);
	if (!candidate->open(_shaderPath, archives)) {
		warning("Metal preset '%s': %s", _shaderPath.toString().c_str(), candidate->error().c_str());
		delete candidate; return false;
	}
	delete _shader; _shader = candidate;
	_forceRedraw = true;
	return true;
}

OSystem::TransactionError MetalSdlGraphicsManager::endGFXTransaction() {
	bool active = _transactionMode == kTransactionActive;
	int errors = OSystem::kTransactionSuccess;
	if (_transactionMode == kTransactionRollback && _render3D != _oldRender3D) {
		_render3D = _oldRender3D;
		errors |= OSystem::kTransactionModeSwitchFailed;
	}
	errors |= SurfaceSdlGraphicsManager::endGFXTransaction();
	if (_videoMode.mode != kMetalFXMode)
		_metal.resetMetalFX();
	if (active && (_shaderPath != _oldShaderPath || (!_shaderPath.empty() && !_shader))) {
		if (!loadShader()) {
			_shaderPath = _oldShaderPath;
			errors |= OSystem::kTransactionShaderChangeFailed;
		}
	}
	return static_cast<OSystem::TransactionError>(errors);
}

int16 MetalSdlGraphicsManager::getWidth() const {
	return _render3D && _metal.screenTexture() ? _metal.screenTexture()->width() : SurfaceSdlGraphicsManager::getWidth();
}

void MetalSdlGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
	Graphics::PixelFormat gameFormat = Graphics::PixelFormat::createFormatRGBA32();
	SurfaceSdlGraphicsManager::initSize(width, height, _render3D ? &gameFormat : format);
}

int16 MetalSdlGraphicsManager::getHeight() const {
	return _render3D && _metal.screenTexture() ? _metal.screenTexture()->height() : SurfaceSdlGraphicsManager::getHeight();
}

SdlGraphicsManager::State MetalSdlGraphicsManager::getState() const {
	SdlGraphicsManager::State state = SdlGraphicsManager::getState();
	state.screenWidth = _videoMode.screenWidth;
	state.screenHeight = _videoMode.screenHeight;
	return state;
}

void MetalSdlGraphicsManager::captureGameFrame() {
	Graphics::Metal::RenderTarget *previousTarget = _metal.renderTarget();
	Common::Rect previousViewport = _metal.viewport();
	_metal.setRenderTarget(nullptr);
	if (!_metal.width() || !_metal.height() || !_screen) {
		_metal.setRenderTarget(previousTarget);
		_metal.setViewport(previousViewport);
		return;
	}
	if (!_metal.readPixels(_gameFrame, Common::Rect(_metal.width(), _metal.height())))
		error("Metal: could not capture game frame");
	_metal.setRenderTarget(previousTarget);
	_metal.setViewport(previousViewport);
	SDL_Surface *source = SDL_CreateRGBSurfaceWithFormatFrom(_gameFrame.getPixels(), _gameFrame.w, _gameFrame.h,
		32, _gameFrame.pitch, SDL_PIXELFORMAT_RGBA32);
	if (!source)
		error("Metal: could not wrap game frame: %s", SDL_GetError());
	SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
	if (SDL_BlitScaled(source, nullptr, _screen, nullptr) != 0)
		error("Metal: could not scale game frame: %s", SDL_GetError());
	SDL_FreeSurface(source);
	_forceRedraw = true;
}

Graphics::Surface *MetalSdlGraphicsManager::lockScreen() {
	if (_render3D) captureGameFrame();
	return SurfaceSdlGraphicsManager::lockScreen();
}

void MetalSdlGraphicsManager::updateScreen() {
	if (!_render3D) {
		SurfaceSdlGraphicsManager::updateScreen();
		return;
	}
	if ((!_view && !_displayDisabled) || !_composition)
		return;
	if (!_overlayVisible) {
		GUI::Debugger *debugger = g_engine ? g_engine->getDebugger() : nullptr;
		if (debugger) debugger->onFrame();
	}
	int width, height;
	width = _windowWidth; height = _windowHeight;
	if (_view) SDL_Metal_GetDrawableSize(_window->getSDLWindow(), &width, &height);
	if (width > 0 && height > 0 && (width != _windowWidth || height != _windowHeight))
		notifyResize(width, height);
	_metal.setRenderTarget(_composition);
	if (_overlayVisible) {
		SurfaceSdlGraphicsManager::updateScreen();
	} else {
		_metal.clear(0, 0, 0);
		drawGameTexture(_metal.screenTexture(), _gameDrawRect);
		if (!_shaderCursorComposed && _cursorVisible && _mouseSurface && _mouseCurState.w && _mouseCurState.h) {
			if (_cursorTextureDirty || !_cursorTexture) {
				delete _cursorTexture;
				_cursorTexture = textureFromSurface(_mouseSurface);
				_cursorTextureDirty = false;
			}
			Common::Point cursor = convertWindowToVirtual(_cursorX, _cursorY);
			int x = cursor.x * _videoMode.hardwareWidth / getWidth() - _mouseCurState.rHotX;
			int y = cursor.y * _videoMode.hardwareHeight / getHeight() - _mouseCurState.rHotY;
			drawTexture(_cursorTexture, mapScreenRect(Common::Rect(x, y, x + _mouseCurState.rW, y + _mouseCurState.rH)), false, true);
		}
#ifdef USE_OSD
		updateOSD();
		if (_osdMessageSurface) {
			Graphics::Metal::Texture *message = textureFromSurface(_osdMessageSurface);
			SDL_Rect rect = getOSDMessageRect();
			drawTexture(message, mapScreenRect(Common::Rect(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h)),
				false, true, _osdMessageAlpha / 255.f);
			delete message;
		}
		if (_osdIconSurface) {
			Graphics::Metal::Texture *icon = textureFromSurface(_osdIconSurface);
			SDL_Rect rect = getOSDIconRect();
			drawTexture(icon, mapScreenRect(Common::Rect(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h)), false, true);
			delete icon;
		}
#endif
#ifdef USE_IMGUI
		renderImGui();
#endif
		presentScreen();
		_forceRedraw = false;
		_cursorNeedsRedraw = false;
	}
	_metal.setRenderTarget(nullptr);
}

void MetalSdlGraphicsManager::blitCursor() {
	SurfaceSdlGraphicsManager::blitCursor();
	_cursorTextureDirty = true;
}

void MetalSdlGraphicsManager::internUpdateScreen() {
	if (_render3D) {
		SurfaceSdlGraphicsManager::internUpdateScreen();
		return;
	}
	if ((!_view && !_displayDisabled) || !_composition || !_screen) return;
	int width, height;
	width = _windowWidth; height = _windowHeight;
	if (_view) SDL_Metal_GetDrawableSize(_window->getSDLWindow(), &width, &height);
	if (width > 0 && height > 0 && (width != _windowWidth || height != _windowHeight))
		notifyResize(width, height);
	_metal.setRenderTarget(_composition);
	if (!_overlayVisible && _scalerPlugin)
		drawNative2D();
	else
		SurfaceSdlGraphicsManager::internUpdateScreen();
	_metal.setRenderTarget(nullptr);
}

void MetalSdlGraphicsManager::drawNative2D() {
	using namespace Graphics::Metal;
	GUI::Debugger *debugger = g_engine ? g_engine->getDebugger() : nullptr;
	if (debugger) debugger->onFrame();
	Graphics::Surface pixels;
	pixels.init(_screen->w, _screen->h, _screen->pitch, _screen->pixels, _screenFormat);
	bool indexed = _screenFormat.isCLUT8();
	bool recreate = !_gameTexture || _gameTexture->width() != (uint)pixels.w ||
		_gameTexture->height() != (uint)pixels.h || indexed != _gameTextureIsIndexed;
	if (recreate) {
		delete _gameTexture;
		_gameTexture = indexed ? _metal.createIndexTexture(pixels) : _metal.createTexture(pixels);
		_gameTextureIsIndexed = indexed;
	} else if (_forceRedraw) {
		if (indexed) _metal.updateIndexTexture(*_gameTexture, pixels, Common::Rect(pixels.w, pixels.h));
		else _metal.updateTexture(*_gameTexture, pixels, Common::Rect(pixels.w, pixels.h));
	} else {
		for (int i = 0; i < _numDirtyRects; ++i) {
			SDL_Rect &r = _dirtyRectList[i];
			Common::Rect area(r.x, r.y, r.x + r.w, r.y + r.h);
			if (indexed) _metal.updateIndexTexture(*_gameTexture, pixels, area);
			else _metal.updateTexture(*_gameTexture, pixels, area);
		}
	}
	if (!_gameTexture) error("Metal: could not allocate native game texture");
	Texture *image = _gameTexture;
	if (indexed) {
		bool paletteImageChanged = recreate || _forceRedraw || _numDirtyRects > 0 ||
			!_paletteTexture || _paletteDirtyEnd;
		if (!_paletteTexture || _paletteDirtyEnd) {
			Graphics::Surface palette;
			palette.create(256, 1, Graphics::PixelFormat::createFormatRGBA32());
			for (int i = 0; i < 256; ++i)
				((uint32 *)palette.getPixels())[i] = palette.format.ARGBToColor(255,
					_currentPalette[i].r, _currentPalette[i].g, _currentPalette[i].b);
			if (!_paletteTexture) _paletteTexture = _metal.createTexture(palette);
			else _metal.updateTexture(*_paletteTexture, palette, Common::Rect(256, 1));
			palette.free();
			SDL_SetColors(_screen, _currentPalette, 0, 256);
			_paletteDirtyEnd = 0;
		}
		if (!_paletteTarget || _paletteTarget->texture().width() != (uint)pixels.w ||
		    _paletteTarget->texture().height() != (uint)pixels.h) {
			delete _paletteTarget;
			_paletteTarget = _metal.createRenderTarget(pixels.w, pixels.h);
			paletteImageChanged = true;
		}
		if (!_paletteProgram) {
			static const char *const source = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex { packed_float3 position; packed_float2 uv; packed_float4 color; };
struct Varying { float4 position [[position]]; float2 uv; };
vertex Varying paletteVertex(uint id [[vertex_id]], const device Vertex *v [[buffer(0)]]) {
 Varying o; o.position=float4(float3(v[id].position),1); o.uv=float2(v[id].uv); return o;
}
fragment float4 paletteFragment(Varying in [[stage_in]], texture2d<float> indices [[texture(0)]],
 texture2d<float> palette [[texture(1)]], sampler filtering [[sampler(0)]]) {
 uint index=uint(round(indices.sample(filtering,in.uv).r*255.0));
 return palette.read(uint2(index,0));
}
)metal";
			_paletteProgram = _metal.createProgram(source, "paletteVertex", "paletteFragment");
		}
		if (!_paletteTexture || !_paletteTarget || !_paletteProgram)
			error("Metal: could not allocate GPU palette resources");
		// The palette target persists across frames. Cursor/OSD-only updates do
		// not change it; palette animation and dirty pixels still rebuild it.
		if (paletteImageChanged) {
			_metal.setRenderTarget(_paletteTarget);
			_metal.setViewport(Common::Rect(pixels.w, pixels.h));
			Vertex quad[] = {{{-1,1,0},{0,0},{1,1,1,1}},{{1,1,0},{1,0},{1,1,1,1}},
				{{-1,-1,0},{0,1},{1,1,1,1}},{{1,-1,0},{1,1},{1,1,1,1}}};
			DrawState state; state.primitive = kTriangleStrip; state.linear = false;
			TextureBinding bindings[2]; bindings[0].texture = _gameTexture; bindings[0].linear = false;
			bindings[1].texture = _paletteTexture; bindings[1].linear = false;
			_metal.drawProgram(*_paletteProgram, quad, 4, sizeof(Vertex), nullptr, 0, nullptr, 0, bindings, 2, state);
			_metal.setRenderTarget(_composition);
		}
		image = &_paletteTarget->texture();
	}
	if (strcmp(_scalerPlugin->getName(), "normal")) {
		// Match OpenGL's CPU scaler stage before its hardware shader pipeline.
		// The cursor and OSD are composited separately, at their original stages.
		SDL_Rect sourceRect = {0, 0, _screen->w, _screen->h};
		SDL_Rect destinationRect = {int(_maxExtraPixels), int(_maxExtraPixels), _screen->w, _screen->h};
		if (SDL_BlitSurface(_screen, &sourceRect, _tmpscreen, &destinationRect) != 0)
			error("Metal: could not convert scaler input: %s", SDL_GetError());
		restoreScalerSourceAfterOverlay();
		const int scaledWidth = _screen->w * _videoMode.scaleFactor;
		const int scaledHeight = _screen->h * _videoMode.scaleFactor;
		if (_scaledFrame.w != scaledWidth || _scaledFrame.h != scaledHeight)
			_scaledFrame.create(scaledWidth, scaledHeight, Graphics::PixelFormat::createFormatRGBA32());
		SDL_LockSurface(_tmpscreen);
		const byte *source = (byte *)_tmpscreen->pixels + _maxExtraPixels * (_tmpscreen->pitch + _tmpscreen->format->BytesPerPixel);
		_scaler->scale(source, _tmpscreen->pitch, (byte *)_scaledFrame.getPixels(), _scaledFrame.pitch, _screen->w, _screen->h, 0, 0);
		SDL_UnlockSurface(_tmpscreen);
		if (!_scaledGameTexture || _scaledGameTexture->width() != (uint)_scaledFrame.w || _scaledGameTexture->height() != (uint)_scaledFrame.h) {
			delete _scaledGameTexture; _scaledGameTexture = _metal.createTexture(_scaledFrame);
		} else _metal.updateTexture(*_scaledGameTexture, _scaledFrame, Common::Rect(_scaledFrame.w, _scaledFrame.h));
		if (!_scaledGameTexture) error("Metal: could not allocate CPU-scaled game texture");
		image = _scaledGameTexture;
	}
	_metal.clear(0, 0, 0);
	Common::Rect game = _gameDrawRect;
	// Match SDL's shake offsets without changing the game's logical framebuffer.
	int x = _gameScreenShakeXOffset * game.width() / MAX(1, _videoMode.screenWidth);
	int y = _gameScreenShakeYOffset * game.height() / MAX(1, _videoMode.screenHeight);
	if (_rotationMode == Common::kRotation90) { int previous = x; x = -y; y = previous; }
	else if (_rotationMode == Common::kRotation180) { x = -x; y = -y; }
	else if (_rotationMode == Common::kRotation270) { int previous = x; x = y; y = -previous; }
	game.translate(x, y);
	drawGameTexture(image, game);
	drawInterface();
#ifdef USE_IMGUI
	renderImGui();
#endif
	presentScreen();
	_numDirtyRects = 0;
	_forceRedraw = false;
	_cursorNeedsRedraw = false;
}

void MetalSdlGraphicsManager::drawInterface() {
	if (!_shaderCursorComposed) drawCursor();
#ifdef USE_OSD
	updateOSD();
	if (_osdMessageSurface) {
		Graphics::Metal::Texture *message = textureFromSurface(_osdMessageSurface);
		SDL_Rect rect = getOSDMessageRect();
		drawTexture(message, mapScreenRect(Common::Rect(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h)),
			false, true, _osdMessageAlpha / 255.f);
		delete message;
	}
	if (_osdIconSurface) {
		Graphics::Metal::Texture *icon = textureFromSurface(_osdIconSurface);
		SDL_Rect rect = getOSDIconRect();
		drawTexture(icon, mapScreenRect(Common::Rect(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h)), false, true);
		delete icon;
	}
#endif
}

void MetalSdlGraphicsManager::drawCursor(const Common::Rect *shaderRect) {
	if (_cursorVisible && _mouseSurface && _mouseCurState.w && _mouseCurState.h) {
		if (_cursorTextureDirty || !_cursorTexture) {
			delete _cursorTexture;
			_cursorTexture = textureFromSurface(_mouseSurface);
			_cursorTextureDirty = false;
		}
		Common::Point cursor = convertWindowToVirtual(_cursorX, _cursorY);
		int x = cursor.x * _videoMode.hardwareWidth / getWidth() - _mouseCurState.rHotX;
		int y = cursor.y * _videoMode.hardwareHeight / getHeight() - _mouseCurState.rHotY;
		Common::Rect rectangle(x, y, x + _mouseCurState.rW, y + _mouseCurState.rH);
		if (shaderRect) {
			rectangle = Common::Rect(x * shaderRect->width() / _videoMode.hardwareWidth,
				y * shaderRect->height() / _videoMode.hardwareHeight,
				(x + _mouseCurState.rW) * shaderRect->width() / _videoMode.hardwareWidth,
				(y + _mouseCurState.rH) * shaderRect->height() / _videoMode.hardwareHeight);
		} else rectangle = mapScreenRect(rectangle);
		drawTexture(_cursorTexture, rectangle, false, true, 1, !shaderRect);
	}
}

void MetalSdlGraphicsManager::drawGameTexture(Graphics::Metal::Texture *texture, const Common::Rect &rect) {
	_shaderCursorComposed = false;
	if (!texture) return;
	uint width = MAX<int>(0, rect.width()), height = MAX<int>(0, rect.height());
	if (_rotationMode == Common::kRotation90 || _rotationMode == Common::kRotation270) SWAP(width, height);
	// Presets control their own scaling and cursor composition. Avoid applying
	// a second sharpening/upscaling effect to an explicitly chosen preset.
	if (_videoMode.mode == kMetalFXMode && !(_shader && _shader->isOpen())) {
		Graphics::Metal::Texture *scaled = _metal.upscaleMetalFX(*texture, width, height);
		if (scaled) texture = scaled;
	}
	if (_shader && _shader->isOpen()) {
		if (!_shaderInput || _shaderInput->texture().width() != texture->width() || _shaderInput->texture().height() != texture->height()) {
			delete _shaderInput;
			_shaderInput = _metal.createRenderTarget(texture->width(), texture->height());
		}
		if (_shaderInput) {
			Common::Rect inputRect(texture->width(), texture->height());
			_metal.setRenderTarget(_shaderInput); _metal.clear(0, 0, 0, 1);
			drawTexture(texture, inputRect, false, false, 1, false);
			drawCursor(&inputRect);
			_metal.setRenderTarget(_composition);
			Graphics::Metal::Texture *result = _shader->render(_shaderInput->texture(), width, height, _videoMode.filtering);
			if (result) { texture = result; _shaderCursorComposed = true; }
			else {
				warning("Metal preset: %s", _shader->error().c_str());
				delete _shader; _shader = nullptr; _shaderPath.clear();
			}
		}
	}
	drawTexture(texture, rect, _videoMode.filtering, false);
}

Graphics::Metal::Texture *MetalSdlGraphicsManager::textureFromSurface(SDL_Surface *surface) {
	SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
	if (!rgba)
		error("Metal: could not convert compositing surface: %s", SDL_GetError());
	Uint32 key;
	if (SDL_GetColorKey(rgba, &key) == 0) {
		SDL_LockSurface(rgba);
		for (int y = 0; y < rgba->h; ++y) {
			uint32 *pixels = (uint32 *)((byte *)rgba->pixels + y * rgba->pitch);
			for (int x = 0; x < rgba->w; ++x)
				if (pixels[x] == key) pixels[x] = 0;
		}
		SDL_UnlockSurface(rgba);
	}
	Graphics::Surface data;
	data.init(rgba->w, rgba->h, rgba->pitch, rgba->pixels, Graphics::PixelFormat::createFormatRGBA32());
	Graphics::Metal::Texture *texture = _metal.createTexture(data);
	SDL_FreeSurface(rgba);
	if (!texture)
		error("Metal: could not allocate compositing texture");
	return texture;
}

Common::Rect MetalSdlGraphicsManager::mapScreenRect(const Common::Rect &source) const {
	float x = source.left, y = source.top, w = source.width(), h = source.height();
	float sourceW = _videoMode.hardwareWidth, sourceH = _videoMode.hardwareHeight;
	switch (_rotationMode) {
	case Common::kRotation90: {
		float tmp = x;
		x = sourceH - y - h;
		y = tmp;
		SWAP(w, h); SWAP(sourceW, sourceH);
		break;
	}
	case Common::kRotation180: x = sourceW - x - w; y = sourceH - y - h; break;
	case Common::kRotation270: {
		float tmp = y;
		y = sourceW - x - w;
		x = tmp;
		SWAP(w, h); SWAP(sourceW, sourceH);
		break;
	}
	default: break;
	}
	int left = _gameDrawRect.left + x * _gameDrawRect.width() / sourceW;
	int top = _gameDrawRect.top + y * _gameDrawRect.height() / sourceH;
	return Common::Rect(left, top, left + w * _gameDrawRect.width() / sourceW, top + h * _gameDrawRect.height() / sourceH);
}

void MetalSdlGraphicsManager::presentBuffer() {
	_metal.flush();
}

void MetalSdlGraphicsManager::showOverlay(bool inGUI) {
	if (_render3D && !_overlayVisible)
		captureGameFrame();
	SurfaceSdlGraphicsManager::showOverlay(inGUI);
}

void MetalSdlGraphicsManager::hideOverlay() {
	SurfaceSdlGraphicsManager::hideOverlay();
}

void MetalSdlGraphicsManager::clearOverlay() {
	SurfaceSdlGraphicsManager::clearOverlay();
}

void MetalSdlGraphicsManager::releaseRenderResources() {
#ifdef USE_IMGUI
	destroyImGui();
#endif
	_metal.flush(true);
	_metal.resetMetalFX();
	delete _texture;
	_texture = nullptr;
	_metal.setRenderTarget(nullptr);
	delete _composition; _composition = nullptr;
	delete _cursorTexture; _cursorTexture = nullptr;
	delete _gameTexture; _gameTexture = nullptr;
	delete _scaledGameTexture; _scaledGameTexture = nullptr;
	_scaledFrame.free();
	delete _paletteTexture; _paletteTexture = nullptr;
	delete _paletteTarget; _paletteTarget = nullptr;
	delete _paletteProgram; _paletteProgram = nullptr;
	delete _shaderInput; _shaderInput = nullptr;
	_cursorTextureDirty = true;
}

void MetalSdlGraphicsManager::destroyingWindow() {
	releaseRenderResources();
	_metal.detachLayer();
	if (_view)
		SDL_Metal_DestroyView(_view);
	_view = nullptr;
}

float MetalSdlGraphicsManager::getHiDPIScreenFactor() const {
	// SurfaceSDL overlays use logical surface pixels. The Metal composition
	// scales them to the drawable; scaling GUI layout by Retina again clips it.
	return 1;
}

void MetalSdlGraphicsManager::gameRenderSize(uint &width, uint &height) const {
	if (!_render3D) return;
	if (g_engine && !g_engine->hasFeature(Engine::kSupportsArbitraryResolutions)) {
		width = _videoMode.screenWidth; height = _videoMode.screenHeight;
	} else if (_videoMode.mode == kMetalFXMode && _shaderPath.empty()) {
		// Quality input: two thirds of the drawable in each dimension. Engines
		// with fixed pixel coordinates keep their original render resolution.
		width = MAX(1U, width * 2 / 3); height = MAX(1U, height * 2 / 3);
	}
}

SDL_Surface *MetalSdlGraphicsManager::SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags) {
	// A graphics transaction does not necessarily replace the SDL window.
	// Keep its view/layer alive for Cocoa and asynchronous Metal HUD callbacks;
	// SdlWindow calls destroyingWindow() before an actual window replacement.
	if (_displayDisabled)
		destroyingWindow();
	else
		releaseRenderResources();
	if (_displayDisabled) {
		if (!_metal.attach(nullptr)) return nullptr;
		uint renderWidth = width, renderHeight = height;
		gameRenderSize(renderWidth, renderHeight);
		uint samples = _render3D && ConfMan.hasKey("antialiasing") ? MAX(1, ConfMan.getInt("antialiasing")) : 1;
		if (!_metal.supportsSamples(samples)) samples = 1;
		if (!_metal.resize(renderWidth, renderHeight, samples)) return nullptr;
		_composition = _metal.createRenderTarget(width, height);
		if (!_composition) return nullptr;
		_isDoubleBuf = _isHwPalette = false;
		handleResize(width, height);
		return SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
	}
	uint32 windowFlags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if (_videoMode.fullscreen)
		windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	if (!createOrUpdateWindow(width, height, windowFlags))
		return nullptr;
	if (!_view) {
		_view = SDL_Metal_CreateView(_window->getSDLWindow());
		if (!_view || !_metal.attach(SDL_Metal_GetLayer(_view))) {
			destroyingWindow();
			warning("Metal 3 requires a supported Apple M1–M6 GPU and macOS 13 or later");
			return nullptr;
		}
	}
	SDL_Metal_GetDrawableSize(_window->getSDLWindow(), &_windowWidth, &_windowHeight);
	uint renderWidth = _windowWidth;
	uint renderHeight = _windowHeight;
	// As with OpenGL, engines with fixed coordinates render at their game
	// resolution. Only the composition target follows the Retina drawable.
	gameRenderSize(renderWidth, renderHeight);
	uint samples = _render3D && ConfMan.hasKey("antialiasing") ? MAX(1, ConfMan.getInt("antialiasing")) : 1;
	if (!_metal.supportsSamples(samples))
		samples = 1;
	if (!_metal.resize(renderWidth, renderHeight, samples)) {
		destroyingWindow();
		return nullptr;
	}
	{
		_composition = _metal.createRenderTarget(_windowWidth, _windowHeight);
		if (!_composition) {
			destroyingWindow();
			return nullptr;
		}
	}
	_metal.setVSync(_videoMode.vsync);
	handleResize(_windowWidth, _windowHeight);
	SDL_Surface *screen = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
	if (!screen)
		destroyingWindow();
#ifdef USE_IMGUI
	else
		initImGui(nullptr, nullptr);
#endif
	return screen;
}

SDL_Surface *MetalSdlGraphicsManager::createOffscreenSurface(int width, int height) {
	return SDL_SetVideoMode(width, height, 32, 0);
}

void MetalSdlGraphicsManager::notifyResize(int width, int height) {
	if (!_view)
		return;
	SDL_Metal_GetDrawableSize(_window->getSDLWindow(), &width, &height);
	if (width <= 0 || height <= 0)
		return;
	if (width == _windowWidth && height == _windowHeight)
		return;
	_metal.setRenderTarget(nullptr);
	uint renderWidth = width, renderHeight = height;
	gameRenderSize(renderWidth, renderHeight);
	if (!_metal.resize(renderWidth, renderHeight, _metal.sampleCount())) {
		warning("Metal: could not resize render target to %dx%d", width, height);
		return;
	}
	{
		_metal.setRenderTarget(nullptr);
		_metal.flush(true);
		Graphics::Metal::RenderTarget *resized = _metal.createRenderTarget(width, height);
		if (!resized) {
			warning("Metal: could not resize composition target");
			return;
		}
		delete _composition;
		_composition = resized;
	}
	handleResize(width, height);
	// SurfaceSDL keeps a fixed game surface on window resize. Metal exposes
	// the resized 3D target through getWidth()/getHeight(), so engines must
	// receive EVENT_SCREEN_CHANGED to rebuild their viewports and input layout.
	++_screenChangeCount;
	_forceRedraw = true;
}

void MetalSdlGraphicsManager::SDL_UpdateRects(SDL_Surface *screen, int numrects, SDL_Rect *rects) {
	if ((!_view && !_displayDisabled) || !screen)
		return;
	int width, height;
	width = _windowWidth; height = _windowHeight;
	if (_view) SDL_Metal_GetDrawableSize(_window->getSDLWindow(), &width, &height);
	if (width <= 0 || height <= 0)
		return;
	if (width != _windowWidth || height != _windowHeight)
		notifyResize(width, height);
	_metal.setRenderTarget(_composition);
	Graphics::Surface surface;
	surface.init(screen->w, screen->h, screen->pitch, screen->pixels, convertSDLPixelFormat(screen->format));
	if (!_texture || _texture->width() != (uint)screen->w || _texture->height() != (uint)screen->h) {
		delete _texture;
		_texture = _metal.createTexture(surface);
	} else {
		_metal.updateTexture(*_texture, surface, Common::Rect(screen->w, screen->h));
	}
	if (!_texture)
		error("Metal: could not allocate screen texture");
	_metal.clear(0, 0, 0);
	if (_overlayVisible)
		drawTexture(_texture, _overlayDrawRect, _videoMode.filtering, false);
	else {
		// CPU scalers have already composited their cursor and OSD into this surface.
		bool cursorVisible = _cursorVisible; _cursorVisible = false;
		drawGameTexture(_texture, _gameDrawRect);
		_cursorVisible = cursorVisible;
	}
}

void MetalSdlGraphicsManager::drawTexture(Graphics::Metal::Texture *texture, const Common::Rect &rect,
		bool linear, bool blend, float alpha, bool rotate) {
	if (!texture)
		return;
	int width = _metal.width(), height = _metal.height();
	_metal.setViewport(Common::Rect(width, height));
	float left = rect.left * 2.0f / width - 1, right = rect.right * 2.0f / width - 1;
	float top = 1 - rect.top * 2.0f / height, bottom = 1 - rect.bottom * 2.0f / height;
	Graphics::Metal::Vertex vertices[] = {
		{{left, top, 0}, {0, 0}, {1, 1, 1, 1}}, {{left, bottom, 0}, {0, 1}, {1, 1, 1, 1}},
		{{right, top, 0}, {1, 0}, {1, 1, 1, 1}}, {{right, bottom, 0}, {1, 1}, {1, 1, 1, 1}}
	};
	for (uint i = 0; rotate && i < ARRAYSIZE(vertices); ++i) {
		float u = vertices[i].texcoord[0], v = vertices[i].texcoord[1];
		switch (_rotationMode) {
		case Common::kRotation90: vertices[i].texcoord[0] = v; vertices[i].texcoord[1] = 1 - u; break;
		case Common::kRotation180: vertices[i].texcoord[0] = 1 - u; vertices[i].texcoord[1] = 1 - v; break;
		case Common::kRotation270: vertices[i].texcoord[0] = 1 - v; vertices[i].texcoord[1] = u; break;
		default: break;
		}
	}
	Graphics::Metal::DrawState state;
	state.primitive = Graphics::Metal::kTriangleStrip;
	state.linear = linear;
	state.blend = true;
	if (!blend) {
		// The opaque OpenGL composition mode writes RGB and keeps target alpha.
		state.blendColor[0] = state.blendColor[1] = state.blendColor[2] = 1;
		state.blendColor[3] = 0;
		state.srcRGB = Graphics::Metal::kConstantColor;
		state.dstRGB = Graphics::Metal::kOneMinusConstantColor;
		state.srcAlpha = Graphics::Metal::kConstantAlpha;
		state.dstAlpha = Graphics::Metal::kOneMinusConstantAlpha;
	}
	Graphics::Metal::Uniforms uniforms;
	uniforms.tint[3] = alpha;
	_metal.draw(vertices, ARRAYSIZE(vertices), texture, state, uniforms);
}

void MetalSdlGraphicsManager::presentScreen() {
	_metal.setVSync(_videoMode.vsync);
	if (!_metal.present(_composition))
		_forceRedraw = true;
}

#ifdef USE_IMGUI
bool MetalSdlGraphicsManager::initMetalImGui() {
	if (!ImGui_ImplSDL2_InitForMetal(_window->getSDLWindow()))
		return false;
	_imGuiMetal = new Graphics::Metal::ImGuiRenderer(_metal);
	return true;
}

void MetalSdlGraphicsManager::renderMetalImGui() {
	_imGuiMetal->render(ImGui::GetDrawData());
	ImGui::UpdatePlatformWindows();
	ImGui::RenderPlatformWindowsDefault();
}

void MetalSdlGraphicsManager::destroyMetalImGui() {
	delete _imGuiMetal;
	_imGuiMetal = nullptr;
}

void *MetalSdlGraphicsManager::getImGuiTexture(const Graphics::Surface &image, const byte *palette, int palCount) {
	Graphics::Surface *rgba = image.convertTo(Graphics::PixelFormat::createFormatRGBA32(), palette, palCount);
	Graphics::Metal::Texture *texture = _metal.createTexture(*rgba);
	if (!texture)
		error("Metal: could not allocate ImGui image");
	rgba->free();
	delete rgba;
	return texture;
}

void MetalSdlGraphicsManager::freeImGuiTexture(void *texture) {
	delete static_cast<Graphics::Metal::Texture *>(texture);
}
#endif

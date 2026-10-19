/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone SDL/Metal integration fixture. Link with the application's objects
 * except macosx-main.o; all writable paths are supplied by the test runner.
 */
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "backends/platform/sdl/macosx/macosx.h"
#include "backends/graphics/metalsdl/metalsdl-graphics.h"
#include "base/commandLine.h"
#include "base/plugins.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/fs.h"
#include "image/png.h"
#include "graphics/scalerplugin.h"
#include <cstdio>
#include <cstdlib>

static uint checks;
static void check(bool ok, const char *name) {
	printf("%s %s\n", ok ? "PASS" : "FAIL", name); ++checks;
	if (!ok) exit(1);
}
class TestSystem : public OSystem_MacOSX {
public:
	MetalSdlGraphicsManager *graphics() { return dynamic_cast<MetalSdlGraphicsManager *>(_graphicsManager); }
};

static const Graphics::Surface *snapshot(const Common::Path &path, Image::PNGDecoder &decoder) {
	check(dynamic_cast<TestSystem *>(g_system)->graphics()->saveScreenshot(path), "backend composition screenshot");
	Common::File file; check(file.open(Common::FSNode(path)), "backend screenshot opens");
	check(decoder.loadStream(file), "backend screenshot decodes");
	return decoder.getSurface();
}
static void center(const Common::Path &path, int red, int green, int blue, const char *name) {
	Image::PNGDecoder decoder; const Graphics::Surface *image = snapshot(path, decoder);
	byte a, r, g, b; image->format.colorToARGB(image->getPixel(image->w / 2, image->h / 2), a, r, g, b);
	if (abs(r - red) > 2 || abs(g - green) > 2 || abs(b - blue) > 2)
		fprintf(stderr, "center pixel %u,%u,%u expected %d,%d,%d\n", r, g, b, red, green, blue);
	check(abs(r - red) <= 2 && abs(g - green) <= 2 && abs(b - blue) <= 2, name);
}

int main(int argc, char **argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc != 3) return 2;
	Common::Path root(argv[1]); Common::Path shot = root.join("composition.png");
	TestSystem *system = new TestSystem(); g_system = system; system->init();
	ConfMan.loadConfigFile(root.join("fixture.ini"), Common::Path()); Base::registerDefaults();
	ConfMan.set("gfx_mode", argv[2]); ConfMan.setBool("disable_display", true);
	ConfMan.setBool("mute", true); ConfMan.setBool("fullscreen", false);
	ConfMan.setBool("filtering", false); ConfMan.setBool("aspect_ratio", false);
	ConfMan.setPath("logfile", root.join("backend.log"));
	ConfMan.setPath("savepath", root); ConfMan.setPath("screenshotpath", root);
	PluginManager::instance().loadAllPlugins();
	system->initBackend();
	Graphics::PixelFormat format = Graphics::PixelFormat::createFormatRGBA32();
	system->beginGFXTransaction(); check(g_system->setGraphicsMode(argv[2]), "backend Metal graphics mode");
	check(system->setScaler(ScalerMan.findScalerPluginIndex("normal"), 1), "normal scaler selection");
	system->initSize(64, 48, &format);
	check(system->endGFXTransaction() == OSystem::kTransactionSuccess, "backend graphics transaction");
	Graphics::Surface game; game.create(64, 48, format);
	game.fillRect(Common::Rect(64, 48), format.ARGBToColor(255, 64, 128, 192));
	system->copyRectToScreen(game.getPixels(), game.pitch, 0, 0, game.w, game.h); system->showMouse(false);
	system->updateScreen(); center(shot, 64, 128, 192, "native backend RGBA composition");
	Graphics::Metal::Renderer *renderer = system->getMetalRenderer(); check(renderer != nullptr, "backend native renderer available");
	renderer->resetStatistics(); for (uint i = 0; i < 20; ++i) system->updateScreen();
	check(renderer->statistics().submissions == 0 && renderer->statistics().uploadedBytes == 0, "unchanged 2D frames neither upload nor submit");
	check(system->hasFeature(OSystem::kFeatureOverlaySupportsAlpha), "backend advertises implemented overlay alpha");
	system->showOverlay(false); system->clearOverlay();
	Graphics::Surface overlay; overlay.create(system->getOverlayWidth(), system->getOverlayHeight(), system->getOverlayFormat());
	overlay.fillRect(Common::Rect(overlay.w, overlay.h), overlay.format.ARGBToColor(128, 255, 0, 0));
	system->copyRectToOverlay(overlay.getPixels(), overlay.pitch, 0, 0, overlay.w, overlay.h); system->updateScreen();
	center(shot, 160, 64, 96, "half-transparent RGBA overlay blends over game");
	system->clearOverlay(); system->updateScreen(); center(shot, 64, 128, 192, "clearOverlay restores transparent live background");
	system->hideOverlay(); system->updateScreen();
	check(system->hasFeature(OSystem::kFeatureCursorMaskInvert), "backend advertises implemented inverted cursor masks");
	uint32 cursor[] = {format.ARGBToColor(255, 0, 255, 0), 0xffffffff, 0xffffffff};
	byte mask[] = {kCursorMaskOpaque, kCursorMaskTransparent, kCursorMaskInvert};
	system->setMouseCursor(cursor, 3, 1, 0, 0, 0, &format, mask, FRAC_ONE, FRAC_ONE);
	Common::Point mouse(32, 24); system->graphics()->notifyMousePosition(mouse);
	system->showMouse(true); system->updateScreen();
	{
		Image::PNGDecoder decoder; const Graphics::Surface *image = snapshot(shot, decoder);
		uint green = 0, inverted = 0;
		for (int y = 0; y < image->h; ++y) for (int x = 0; x < image->w; ++x) {
			byte a, r, g, b; image->format.colorToARGB(image->getPixel(x, y), a, r, g, b);
			green += r == 0 && g == 255 && b == 0;
			inverted += abs(int(r) - 191) <= 1 && abs(int(g) - 127) <= 1 && abs(int(b) - 63) <= 1;
		}
		check(green > 0 && inverted == green, "opaque and inverted cursor masks occupy equal expected pixel areas");
	}
	system->showMouse(false); system->updateScreen();
	Graphics::PixelFormat indexed = Graphics::PixelFormat::createFormatCLUT8();
	system->beginGFXTransaction();
	check(system->setScaler(ScalerMan.findScalerPluginIndex("advmame"), 2), "CPU scaler selection");
	system->initSize(64, 48, &indexed);
	check(system->endGFXTransaction() == OSystem::kTransactionSuccess, "indexed CPU scaler transaction");
	byte palette[] = {64, 128, 192, 255, 0, 0}; system->getPaletteManager()->setPalette(palette, 0, 2);
	byte indices[64 * 48] = {}; system->copyRectToScreen(indices, 64, 0, 0, 64, 48); system->updateScreen();
	center(shot, 64, 128, 192, "indexed palette through CPU scaler");
	renderer->resetStatistics();
	byte patch[16]; memset(patch, 1, sizeof(patch)); system->copyRectToScreen(patch, 4, 30, 22, 4, 4); system->updateScreen();
	check(renderer->statistics().uploadedBytes < 64 * 48 * 4, "CPU scaler dirty patch avoids full-frame upload");
	center(shot, 255, 0, 0, "CPU scaler dirty patch updates center pixels");
	palette[0] = 0; palette[1] = 255; palette[2] = 0;
	system->getPaletteManager()->setPalette(palette, 0, 1); system->updateScreen();
	{
		Image::PNGDecoder decoder; const Graphics::Surface *image = snapshot(shot, decoder);
		byte a, r, g, b; image->format.colorToARGB(image->getPixel(8, 8), a, r, g, b);
		check(r == 0 && g == 255 && b == 0, "palette change refreshes untouched CPU-scaled pixels");
	}
#ifdef USE_OSD
	Graphics::Surface icon; icon.create(4, 4, format); icon.fillRect(Common::Rect(4, 4), 0xffffffff);
	system->displayActivityIconOnOSD(&icon); system->updateScreen(); renderer->flush(true); renderer->resetStatistics();
	for (uint i = 0; i < 10; ++i) system->updateScreen();
	check(renderer->statistics().uploadedBytes == 0 && renderer->statistics().submissions == 0,
		"unchanged OSD icon retains texture without continuous redraw");
	system->displayActivityIconOnOSD(nullptr); system->updateScreen(); icon.free();
#endif
	system->beginGFXTransaction();
	check(g_system->setGraphicsMode(g_system->getGraphicsMode(), OSystem::kGfxModeRender3d | OSystem::kGfxModeMetal), "3D Metal backend mode");
	system->initSize(64, 48, &format);
	check(system->endGFXTransaction() == OSystem::kTransactionSuccess, "3D backend transaction");
	renderer->setRenderTarget(nullptr); renderer->clear(64.f / 255, 128.f / 255, 192.f / 255);
	system->updateScreen(); center(shot, 64, 128, 192, "native GPU 3D target reaches composition");
	system->showOverlay(false); system->clearOverlay(); overlay.free();
	overlay.create(system->getOverlayWidth(), system->getOverlayHeight(), system->getOverlayFormat());
	overlay.fillRect(Common::Rect(overlay.w, overlay.h), overlay.format.ARGBToColor(128, 255, 0, 0));
	system->copyRectToOverlay(overlay.getPixels(), overlay.pitch, 0, 0, overlay.w, overlay.h); system->updateScreen();
	center(shot, 160, 64, 96, "alpha overlay blends over live native 3D target");
	system->clearOverlay(); system->updateScreen(); center(shot, 64, 128, 192, "transparent overlay preserves native 3D target");
	system->hideOverlay();
	game.free(); overlay.free(); system->destroy();
	printf("Completed %u checks\n", checks); return 0;
}

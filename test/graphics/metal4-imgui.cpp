/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone GPU regression for native ImGui list uploads and draw offsets.
 */
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "graphics/metal/imgui.h"
#include "backends/imgui/imgui.h"
#include "common/system.h"
#include <cstdio>
#include <cstdlib>

OSystem *g_system = nullptr;
using namespace Graphics;
using namespace Graphics::Metal;
static uint checks, callbacks;
static Renderer *callbackRenderer;
static void check(bool ok, const char *name) {
	printf("%s %s\n", ok ? "PASS" : "FAIL", name); ++checks;
	if (!ok) exit(1);
}
static void callback(const ImDrawList *, const ImDrawCmd *) { ++callbacks; if (callbackRenderer) callbackRenderer->flush(true); }

int main() {
	setvbuf(stdout, nullptr, _IONBF, 0);
	Renderer r;
	check(r.attach(nullptr) && r.resize(32, 32), "ImGui Metal 4 attachment");
	ImGui::CreateContext();
	{
		ImGuiRenderer ui(r);
		Surface source; source.create(2, 2, PixelFormat::createFormatRGBA32());
		source.fillRect(Common::Rect(2, 2), 0xffffffff);
		Texture *texture = r.createTexture(source); source.free();
		check(texture != nullptr, "ImGui texture allocation");
		ImDrawList list(ImGui::GetDrawListSharedData());
		list.VtxBuffer.resize(8); list.IdxBuffer.resize(12);
		const ImVec2 positions[] = {{10, 20}, {42, 20}, {42, 52}, {10, 52}};
		const ImDrawIdx indices[] = {0, 1, 2, 0, 2, 3};
		for (uint i = 0; i < 8; ++i) {
			list.VtxBuffer[i].pos = positions[i % 4];
			list.VtxBuffer[i].uv = ImVec2(0.5f, 0.5f);
			list.VtxBuffer[i].col = i < 4 ? IM_COL32(255, 0, 0, 255) : IM_COL32(0, 255, 0, 255);
		}
		for (uint i = 0; i < 12; ++i) list.IdxBuffer[i] = indices[i % 6];
		ImDrawCmd command;
		command.ElemCount = 6; command.ClipRect = ImVec4(14, 24, 26, 48);
		command.TexRef = ImTextureRef((ImTextureID)(uintptr)texture);
		list.CmdBuffer.push_back(command);
		ImDrawCmd reset; reset.UserCallback = ImDrawCallback_ResetRenderState;
		list.CmdBuffer.push_back(reset);
		reset.UserCallback = callback; list.CmdBuffer.push_back(reset);
		command.IdxOffset = 6; command.VtxOffset = 4; command.ClipRect = ImVec4(26, 24, 38, 48);
		list.CmdBuffer.push_back(command);
		list._VtxWritePtr = list.VtxBuffer.Data + list.VtxBuffer.Size;
		list._IdxWritePtr = list.IdxBuffer.Data + list.IdxBuffer.Size;
		list._VtxCurrentIdx = list.VtxBuffer.Size;
		ImDrawData data; data.Valid = true; data.DisplayPos = ImVec2(10, 20);
		data.DisplaySize = ImVec2(32, 32); data.FramebufferScale = ImVec2(1, 1); data.AddDrawList(&list);
		r.clear(0, 0, 1, 1); r.setViewport(Common::Rect(1, 2, 30, 31)); r.resetStatistics();
		ui.render(&data);
		check(callbacks == 1, "ImGui custom callback invoked once");
		check(r.viewport() == Common::Rect(1, 2, 30, 31), "ImGui previous viewport restored after reset callbacks");
		check(r.statistics().uploadedBytes == 8 * sizeof(ImDrawVert) + 12 * sizeof(ImDrawIdx) + 16 * sizeof(float) &&
			r.statistics().persistentBufferBytes == 0, "ImGui list geometry uploaded once and projection shared across commands");
		Surface image;
		check(r.readPixels(image, Common::Rect(32, 32)), "ImGui readback after early texture deletion");
		for (uint x : {1u, 8u, 20u, 31u}) {
			byte a, red, green, blue; image.format.colorToARGB(image.getPixel(x, 16), a, red, green, blue);
			check(x == 8 ? red == 255 && green == 0 && blue == 0 :
				x == 20 ? red == 0 && green == 255 && blue == 0 : red == 0 && green == 0 && blue == 255,
				"ImGui display origin, scissor, vertex offset and index offset pixels");
		}
		image.free();
		// Warm the pool, then mutate the list every frame. No per-list MTLBuffer
		// allocation should remain once submissions have returned their pages.
		for (uint frame = 0; frame < 12; ++frame) {
			r.clear(0, 0, 1, 1); r.resetStatistics();
			list.VtxBuffer[0].uv.x = frame / 16.f;
			ui.render(&data); r.flush(true);
			check(r.statistics().persistentBufferBytes == 0 && r.statistics().uploadPages == 0 &&
				r.statistics().uploadPageReuses > 0, "changing ImGui frame reuses completed upload pages");
		}
		// A callback can submit while public slices are still used by later draws.
		callbackRenderer = &r;
		r.clear(0, 0, 1, 1); ui.render(&data); callbackRenderer = nullptr;
		delete texture;
		check(r.readPixels(image, Common::Rect(32, 32)), "ImGui readback after callback submission and texture deletion");
		byte a, red, green, blue; image.format.colorToARGB(image.getPixel(20, 16), a, red, green, blue);
		check(red == 0 && green == 255 && blue == 0, "ImGui later commands retain slices across a submitting callback");
		image.free();
	}
	ImGui::DestroyContext();
	printf("Completed %u checks (native index width %zu)\n", checks, sizeof(ImDrawIdx));
	return 0;
}

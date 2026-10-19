/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/metal/imgui.h"
#include "backends/imgui/imgui.h"
#include "common/textconsole.h"
#include "backends/platform/sdl/sdl-sys.h"
#include <SDL_metal.h>

namespace Graphics {
namespace Metal {

ImGuiRenderer::ImGuiRenderer(Renderer &renderer) : _renderer(renderer), _program(createProgram(renderer)) {
	ImGuiIO &io = ImGui::GetIO();
	io.BackendRendererName = "scummvm_metal4";
	io.BackendRendererUserData = this;
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports;
	io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	ImGuiPlatformIO &platform = ImGui::GetPlatformIO();
	platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = renderer.maxTextureSize();
	platform.Renderer_CreateWindow = createWindow;
	platform.Renderer_DestroyWindow = destroyWindow;
	platform.Renderer_SetWindowSize = resizeWindow;
	platform.Renderer_RenderWindow = renderWindow;
	platform.Renderer_SwapBuffers = presentWindow;
}

ImGuiRenderer::~ImGuiRenderer() {
	ImGui::DestroyPlatformWindows();
	ImGui::GetPlatformIO().ClearRendererHandlers();
	_renderer.flush(true);
	delete _program;
	for (ImTextureData *texture : ImGui::GetPlatformIO().Textures) {
		if (texture->BackendUserData && texture->RefCount == 1) {
			delete static_cast<Texture *>(texture->BackendUserData);
			texture->BackendUserData = nullptr;
			texture->SetTexID(ImTextureID_Invalid);
			texture->SetStatus(ImTextureStatus_Destroyed);
		}
	}
	ImGuiIO &io = ImGui::GetIO();
	io.BackendRendererName = nullptr;
	io.BackendRendererUserData = nullptr;
	io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);
}

void ImGuiRenderer::updateTexture(ImTextureData &data) {
	if (data.Status == ImTextureStatus_WantCreate || data.Status == ImTextureStatus_WantUpdates) {
		Surface surface, alpha;
		if (data.Format == ImTextureFormat_RGBA32) {
			surface.init(data.Width, data.Height, data.GetPitch(), data.GetPixels(), PixelFormat::createFormatRGBA32());
		} else {
			alpha.create(data.Width, data.Height, PixelFormat::createFormatRGBA32());
			for (int y = 0; y < data.Height; ++y) {
				for (int x = 0; x < data.Width; ++x) {
					byte a = *(byte *)data.GetPixelsAt(x, y);
					*(uint32 *)alpha.getBasePtr(x, y) = alpha.format.ARGBToColor(a, 255, 255, 255);
				}
			}
			surface.init(alpha.w, alpha.h, alpha.pitch, alpha.getPixels(), alpha.format);
		}
		if (data.Status == ImTextureStatus_WantCreate) {
			Texture *texture = _renderer.createTexture(surface);
			if (!texture)
				error("Metal: could not allocate ImGui texture");
			data.BackendUserData = texture;
			data.SetTexID((ImTextureID)(uintptr)texture);
		} else {
			Texture *texture = static_cast<Texture *>(data.BackendUserData);
			if (!texture)
				error("Metal: missing ImGui texture for update");
			for (const ImTextureRect &r : data.Updates)
				_renderer.updateTexture(*texture, surface, Common::Rect(r.x, r.y, r.x + r.w, r.y + r.h));
		}
		alpha.free();
		data.SetStatus(ImTextureStatus_OK);
	} else if (data.Status == ImTextureStatus_WantDestroy && data.UnusedFrames > 0) {
		delete static_cast<Texture *>(data.BackendUserData);
		data.BackendUserData = nullptr;
		data.SetTexID(ImTextureID_Invalid);
		data.SetStatus(ImTextureStatus_Destroyed);
	}
}

void ImGuiRenderer::render(ImDrawData *data) {
	if (!data)
		return;
	if (data->Textures)
		for (ImTextureData *texture : *data->Textures)
			if (texture->Status != ImTextureStatus_OK)
				updateTexture(*texture);
	renderDrawData(_renderer, data, *_program);
}

Program *ImGuiRenderer::createProgram(Renderer &renderer) {
	static_assert(sizeof(ImDrawIdx) == 2 || sizeof(ImDrawIdx) == 4, "Unsupported ImGui index width");
	Common::String source = Common::String::format(R"metal(
#include <metal_stdlib>
using namespace metal;
struct Input { float2 position [[attribute(0)]]; float2 uv [[attribute(1)]]; float4 color [[attribute(2)]]; };
struct Output { float4 position [[position]]; float2 uv; float4 color; };
vertex Output imguiVertex(Input in [[stage_in]], constant float4x4 &matrix [[buffer(1)]]) {
 Output o; o.position=matrix*float4(in.position,0,1); o.uv=in.uv;
 o.color=float4(in.color[%u],in.color[%u],in.color[%u],in.color[%u]); return o;
}
fragment float4 imguiFragment(Output in [[stage_in]], texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]]) {
 return in.color*image.sample(filtering,in.uv);
}
)metal", IM_COL32_R_SHIFT / 8, IM_COL32_G_SHIFT / 8, IM_COL32_B_SHIFT / 8, IM_COL32_A_SHIFT / 8);
	VertexAttribute attributes[3];
	attributes[0].index = 0; attributes[0].offset = offsetof(ImDrawVert, pos); attributes[0].components = 2;
	attributes[1].index = 1; attributes[1].offset = offsetof(ImDrawVert, uv); attributes[1].components = 2;
	attributes[2].index = 2; attributes[2].offset = offsetof(ImDrawVert, col); attributes[2].components = 4; attributes[2].type = VertexAttribute::kUNorm8;
	Program *program = renderer.createProgram(source.c_str(), "imguiVertex", "imguiFragment", attributes, 3, sizeof(ImDrawVert));
	if (!program) error("Metal: could not create native ImGui program");
	return program;
}

void ImGuiRenderer::renderDrawData(Renderer &renderer, ImDrawData *data, Program &program) {
	if (data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0)
		return;
	const int width = renderer.width(), height = renderer.height();
	const Common::Rect previousViewport = renderer.viewport();
	renderer.setViewport(Common::Rect(width, height));
	Uniforms uniforms;
	uniforms.transform[0] = 2 / data->DisplaySize.x;
	uniforms.transform[5] = -2 / data->DisplaySize.y;
	uniforms.transform[12] = -1 - data->DisplayPos.x * uniforms.transform[0];
	uniforms.transform[13] = 1 - data->DisplayPos.y * uniforms.transform[5];
	DrawState state;
	state.blend = true;
	state.scissor = true;
	Buffer *matrix = renderer.createTransientBuffer(uniforms.transform, sizeof(uniforms.transform));
	if (!matrix) error("Metal: could not upload ImGui projection");
	UniformBindings constants; constants.vertex = matrix;
	float scaleX = width / data->DisplaySize.x, scaleY = height / data->DisplaySize.y;
	for (const ImDrawList *list : data->CmdLists) {
		Buffer *vertices = list->VtxBuffer.Size > 0 ? renderer.createTransientBuffer(list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert)) : nullptr;
		Buffer *indices = list->IdxBuffer.Size > 0 ? renderer.createTransientBuffer(list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx)) : nullptr;
		if ((list->VtxBuffer.Size > 0 && !vertices) || (list->IdxBuffer.Size > 0 && !indices)) {
			delete vertices; delete indices; error("Metal: could not upload ImGui draw list");
		}
		for (const ImDrawCmd &command : list->CmdBuffer) {
			if (command.UserCallback) {
				if (command.UserCallback != ImDrawCallback_ResetRenderState)
					command.UserCallback(list, &command);
				renderer.setViewport(Common::Rect(width, height));
				continue;
			}
			Common::Rect clip((int)floorf((command.ClipRect.x - data->DisplayPos.x) * scaleX),
			                  (int)floorf((command.ClipRect.y - data->DisplayPos.y) * scaleY),
			                  (int)ceilf((command.ClipRect.z - data->DisplayPos.x) * scaleX),
			                  (int)ceilf((command.ClipRect.w - data->DisplayPos.y) * scaleY));
			clip.clip(Common::Rect(width, height));
			if (clip.isEmpty() || !command.ElemCount)
				continue;
			state.scissorRect = clip;
			if (command.VtxOffset >= uint(list->VtxBuffer.Size) || command.IdxOffset > uint(list->IdxBuffer.Size) ||
			    command.ElemCount > uint(list->IdxBuffer.Size) - command.IdxOffset)
				error("Metal: ImGui draw range out of bounds");
			for (uint i = 0; i < command.ElemCount; ++i)
				if (uint64(list->IdxBuffer[command.IdxOffset + i]) + command.VtxOffset >= uint(list->VtxBuffer.Size))
					error("Metal: ImGui vertex index out of bounds");
			GeometryBinding geometry;
			geometry.vertices = vertices; geometry.indices = indices;
			geometry.vertexOffset = command.VtxOffset * sizeof(ImDrawVert);
			geometry.indexOffset = command.IdxOffset * sizeof(ImDrawIdx);
			geometry.indices16 = sizeof(ImDrawIdx) == 2;
			TextureBinding texture; texture.texture = (Texture *)(uintptr)command.GetTexID(); texture.mipmaps = false;
			renderer.drawProgram(program, nullptr, list->VtxBuffer.Size - command.VtxOffset, sizeof(ImDrawVert), nullptr,
				command.ElemCount, nullptr, 0, &texture, 1, state, nullptr, 0, &geometry, &constants);
		}
		delete vertices; delete indices;
	}
	delete matrix;
	renderer.setViewport(previousViewport);
}


struct ViewportData {
	Renderer renderer;
	Program *program = nullptr;
	SDL_MetalView view = nullptr;
	SDL_Window *window = nullptr;
	~ViewportData() {
		delete program;
		renderer.detach();
		if (view) SDL_Metal_DestroyView(view);
	}
};

void ImGuiRenderer::createWindow(ImGuiViewport *viewport) {
	ImGuiRenderer *main = static_cast<ImGuiRenderer *>(ImGui::GetIO().BackendRendererUserData);
	ViewportData *data = new ViewportData();
	data->window = SDL_GetWindowFromID((Uint32)(uintptr)viewport->PlatformHandle);
	if (data->window) data->view = SDL_Metal_CreateView(data->window);
	if (!main || !data->view || !data->renderer.attach(SDL_Metal_GetLayer(data->view), &main->_renderer)) {
		delete data;
		error("Metal: could not initialize ImGui platform window");
	}
	data->program = createProgram(data->renderer);
	viewport->RendererUserData = data;
	resizeWindow(viewport, viewport->Size);
}

void ImGuiRenderer::destroyWindow(ImGuiViewport *viewport) {
	delete static_cast<ViewportData *>(viewport->RendererUserData);
	viewport->RendererUserData = nullptr;
}

void ImGuiRenderer::resizeWindow(ImGuiViewport *viewport, ImVec2) {
	ViewportData *data = static_cast<ViewportData *>(viewport->RendererUserData);
	if (!data) return;
	int width, height;
	SDL_Metal_GetDrawableSize(data->window, &width, &height);
	if (width > 0 && height > 0 &&
	    (data->renderer.width() != (uint)width || data->renderer.height() != (uint)height)) {
		debug(2, "Metal ImGui viewport: logical %.0fx%.0f, drawable %dx%d", viewport->Size.x, viewport->Size.y, width, height);
		if (!data->renderer.resize(width, height, 1, false)) error("Metal: could not resize ImGui platform window");
	}
}

void ImGuiRenderer::renderWindow(ImGuiViewport *viewport, void *) {
	ViewportData *data = static_cast<ViewportData *>(viewport->RendererUserData);
	ImGuiRenderer *main = static_cast<ImGuiRenderer *>(ImGui::GetIO().BackendRendererUserData);
	if (!data || !main || !viewport->DrawData) return;
	resizeWindow(viewport, viewport->Size);
	if (viewport->DrawData->Textures)
		for (ImTextureData *texture : *viewport->DrawData->Textures)
			if (texture->Status != ImTextureStatus_OK) main->updateTexture(*texture);
	// Atlas and engine-texture writes on the shared device finish before the
	// secondary queue samples those resources.
	data->renderer.waitFor(main->_renderer);
	if (!(viewport->Flags & ImGuiViewportFlags_NoRendererClear)) data->renderer.clear(0, 0, 0, 1);
	main->renderDrawData(data->renderer, viewport->DrawData, *data->program);
}

void ImGuiRenderer::presentWindow(ImGuiViewport *viewport, void *) {
	ViewportData *data = static_cast<ViewportData *>(viewport->RendererUserData);
	if (data) data->renderer.present();
}

void ImGuiRenderer::finishPlatformWindows() {
	// Join only after all secondary queues have been scheduled. They can read
	// the shared atlas concurrently; subsequent main-queue writes/frees wait
	// for those reads without blocking the CPU at every viewport.
	for (ImGuiViewport *viewport : ImGui::GetPlatformIO().Viewports) {
		ViewportData *data = static_cast<ViewportData *>(viewport->RendererUserData);
		if (data) _renderer.waitFor(data->renderer);
	}
}

} // End of namespace Metal
} // End of namespace Graphics

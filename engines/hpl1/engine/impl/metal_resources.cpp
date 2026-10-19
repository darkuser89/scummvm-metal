/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hpl1/engine/impl/metal_resources.h"
#include "hpl1/engine/impl/low_level_graphics_metal.h"
#include "hpl1/engine/impl/metal_shader_sources.h"
#include "hpl1/engine/math/Math.h"
#include "hpl1/engine/system/String.h"
#include "common/textconsole.h"
#include "graphics/metal/shader-source.h"

namespace hpl {

static const MetalShaderSource *shaderSource(const tString &name, const char *stage) {
	tString key = cString::GetFileName(name);
	if (key.hasPrefix("hpl1_")) key = key.substr(5);
	if (key.hasSuffix(".vertex")) key = key.substr(0, key.size() - 7);
	if (key.hasSuffix(".fragment")) key = key.substr(0, key.size() - 9);
	for (const MetalShaderSource &source : kMetalShaderSources)
		if (key.equalsIgnoreCase(source.name) && !strcmp(stage, source.stage)) return &source;
	return nullptr;
}

MetalGpuProgram::MetalGpuProgram(LowLevelGraphicsMetal *graphics, const tString &vertex, const tString &fragment)
	: iGpuProgram(vertex + " " + fragment), _graphics(graphics), _vertex(vertex), _fragment(fragment) {
	if (!reload()) error("HPL1: native shader pair unavailable: %s / %s", vertex.c_str(), fragment.c_str());
}
MetalGpuProgram::~MetalGpuProgram() { unload(); }
bool MetalGpuProgram::reload() {
	const MetalShaderSource *vertex = shaderSource(_vertex, "vertex"), *fragment = shaderSource(_fragment, "fragment");
	if (!vertex || !fragment) return false;
	tString source(kMetalShaderPrelude);
	Common::String vertexOverride = Graphics::Metal::shaderOverride("hpl1", (Common::String(vertex->name) + ".vertex").c_str());
	Common::String fragmentOverride = Graphics::Metal::shaderOverride("hpl1", (Common::String(fragment->name) + ".fragment").c_str());
	source += vertexOverride.empty() ? vertex->source : vertexOverride.c_str();
	source += fragmentOverride.empty() ? fragment->source : fragmentOverride.c_str();
	Graphics::Metal::Program *program = _graphics->metal().createProgram(source.c_str(), vertex->entry, fragment->entry);
	if (!program) return false;
	delete _program; _program = program;
	memcpy(_textureKinds, fragment->textureKinds, sizeof(_textureKinds));
	return true;
}
void MetalGpuProgram::unload() {
	_graphics->releaseProgram(this);
	delete _program; _program = nullptr;
}
void MetalGpuProgram::Bind() { _graphics->bindProgram(this); }
void MetalGpuProgram::UnBind() { _graphics->bindProgram(nullptr); }
bool MetalGpuProgram::set(const tString &name, const float *values, uint count) {
	for (const MetalUniformBinding &binding : kMetalUniformBindings) {
		if (name != binding.name) continue;
		if (binding.components != count) return false;
		memcpy((byte *)&_uniforms + binding.offset, values, count * sizeof(float));
		return true;
	}
	// As with GL, setting an optimized-out or absent uniform is harmless.
	return true;
}
bool MetalGpuProgram::SetFloat(const tString &name, float x) { return set(name, &x, 1); }
bool MetalGpuProgram::SetVec2f(const tString &name, float x, float y) { float values[] = {x, y}; return set(name, values, 2); }
bool MetalGpuProgram::SetVec3f(const tString &name, float x, float y, float z) { float values[] = {x, y, z}; return set(name, values, 3); }
bool MetalGpuProgram::SetVec4f(const tString &name, float x, float y, float z, float w) { float values[] = {x, y, z, w}; return set(name, values, 4); }
bool MetalGpuProgram::SetMatrixf(const tString &name, const cMatrixf &matrix) {
	cMatrixf columns = matrix.GetTranspose();
	return set(name, columns.v, 16);
}
bool MetalGpuProgram::SetMatrixf(const tString &name, eGpuProgramMatrix type, eGpuProgramMatrixOp operation) {
	cMatrixf matrix = cMatrixf::Identity;
	switch (type) {
	case eGpuProgramMatrix_View: matrix = _graphics->matrix(eMatrix_ModelView); break;
	case eGpuProgramMatrix_Projection: matrix = _graphics->matrix(eMatrix_Projection); break;
	case eGpuProgramMatrix_Texture: matrix = _graphics->matrix(eMatrix_Texture); break;
	case eGpuProgramMatrix_ViewProjection: matrix = cMath::MatrixMul(_graphics->matrix(eMatrix_Projection), _graphics->matrix(eMatrix_ModelView)); break;
	default: return false;
	}
	if (operation == eGpuProgramMatrixOp_Inverse || operation == eGpuProgramMatrixOp_InverseTranspose) matrix = cMath::MatrixInverse(matrix);
	if (operation == eGpuProgramMatrixOp_Transpose || operation == eGpuProgramMatrixOp_InverseTranspose) matrix = matrix.GetTranspose();
	return SetMatrixf(name, matrix);
}

static int nextTextureId = 1;
MetalTexture::MetalTexture(const tString &name, Graphics::PixelFormat *format, iLowLevelGraphics *graphics,
		eTextureType type, bool mipmaps, eTextureTarget target)
	: iTexture(name, "Metal", format, graphics, type, mipmaps, target),
	  _graphics(static_cast<LowLevelGraphicsMetal *>(graphics)), _id(nextTextureId++) {
	_width = _height = 0;
	if (target == eTextureTarget_CubeMap) mbUseMipMaps = false;
}
MetalTexture::~MetalTexture() {
	_graphics->releaseTexture(this);
	clear();
}
void MetalTexture::clear() {
	if (_target && _graphics->currentRenderTarget() == this)
		_graphics->SetRenderTarget(nullptr);
	for (Graphics::Metal::Texture *frame : _frames) delete frame;
	_frames.clear(); delete _target; _target = nullptr;
}
Graphics::Metal::Texture *MetalTexture::texture() const {
	if (_target) return &_target->texture();
	if (_frames.empty()) return nullptr;
	int index = CLIP((int)_time, 0, (int)_frames.size() - 1);
	return _frames[index];
}
bool MetalTexture::appendBitmap(Bitmap2D *bitmap) {
	if (!bitmap || !bitmap->getWidth() || !bitmap->getHeight()) return false;
	_width = bitmap->getWidth(); _height = bitmap->getHeight(); _alpha = bitmap->hasAlpha();
	Graphics::Surface original;
	original.init(_width, _height, _width * bitmap->format().bytesPerPixel, const_cast<void *>(bitmap->getRawData()), bitmap->format());
	Graphics::Surface *rgba = original.convertTo(Graphics::PixelFormat::createFormatRGBA32());
	if (!rgba) return false;
	if (mlSizeLevel && (int)_width > mvMinLevelSize.x * 2) {
		uint divisor = 1U << MIN(mlSizeLevel, 14U);
		uint w = MAX(1U, _width / divisor), h = MAX(1U, _height / divisor);
		while (w < (uint)mvMinLevelSize.x && divisor > 1) { w *= 2; h *= 2; divisor /= 2; }
		Graphics::Surface *scaled = rgba->scale(w, h); rgba->free(); delete rgba; rgba = scaled;
		_width = w; _height = h;
	}
	Graphics::Metal::Texture *texture = _graphics->metal().createTexture(*rgba, mbUseMipMaps && mTarget != eTextureTarget_Rect);
	rgba->free(); delete rgba;
	if (!texture) return false;
	_frames.push_back(texture); _bpp = 32; return true;
}
bool MetalTexture::CreateFromBitmap(Bitmap2D *bitmap) { clear(); return appendBitmap(bitmap); }
bool MetalTexture::CreateAnimFromBitmapVec(tBitmap2DVec *bitmaps) {
	clear(); if (!bitmaps) return false;
	for (Bitmap2D *bitmap : *bitmaps) if (!appendBitmap(bitmap)) { clear(); return false; }
	return !_frames.empty();
}
bool MetalTexture::CreateCubeFromBitmapVec(tBitmap2DVec *bitmaps) {
	if (!bitmaps || bitmaps->size() < 6 || mTarget != eTextureTarget_CubeMap || mType == eTextureType_RenderTarget) return false;
	Graphics::Surface faces[6];
	bool valid = true;
	for (uint i = 0; i < 6; ++i) {
		Bitmap2D *bitmap = (*bitmaps)[i];
		if (!bitmap) { valid = false; break; }
		Graphics::Surface source;
		source.init(bitmap->getWidth(), bitmap->getHeight(), bitmap->getWidth() * bitmap->format().bytesPerPixel,
			const_cast<void *>(bitmap->getRawData()), bitmap->format());
		Graphics::Surface *rgba = source.convertTo(Graphics::PixelFormat::createFormatRGBA32());
		if (!rgba) { valid = false; break; }
		faces[i].copyFrom(*rgba); rgba->free(); delete rgba;
	}
	Graphics::Metal::Texture *texture = valid ? _graphics->metal().createCubeTexture(faces) : nullptr;
	for (Graphics::Surface &face : faces) face.free();
	if (!texture) return false;
	clear(); _frames.push_back(texture); _width = texture->width(); _height = texture->height(); _bpp = 32;
	return true;
}
bool MetalTexture::Create(uint width, uint height, cColor color) {
	if (!width || !height) return false;
	clear(); _width = width; _height = height; _bpp = 32; _alpha = color.a < 1;
	if (mType == eTextureType_RenderTarget) {
		_target = _graphics->metal().createRenderTarget(width, height);
		if (_target) {
			iTexture *previous = _graphics->currentRenderTarget();
			Common::Rect viewport = _graphics->metal().viewport();
			_graphics->SetRenderTarget(this);
			_graphics->metal().clear(color.r, color.g, color.b, color.a);
			_graphics->SetRenderTarget(previous);
			_graphics->metal().setViewport(viewport);
		}
		return _target != nullptr;
	}
	Graphics::Surface surface;
	surface.create(width, height, Graphics::PixelFormat::createFormatRGBA32());
	surface.fillRect(Common::Rect(width, height), surface.format.ARGBToColor(CLIP((int)(color.a * 255), 0, 255), CLIP((int)(color.r * 255), 0, 255), CLIP((int)(color.g * 255), 0, 255), CLIP((int)(color.b * 255), 0, 255)));
	Graphics::Metal::Texture *texture = _graphics->metal().createTexture(surface, mbUseMipMaps);
	surface.free(); if (!texture) return false; _frames.push_back(texture); return true;
}
bool MetalTexture::CreateFromArray(unsigned char *pixels, int channels, const cVector3l &size) {
	if (!pixels || channels < 1 || channels > 4 || size.x <= 0 || size.y <= 0 || size.z <= 0) return false;
	uint64 length = (uint64)size.x * size.y * size.z * 4;
	if (length > 0xffffffffULL) return false;
	Common::Array<byte> rgba; rgba.resize((uint)length);
	for (uint i = 0; i < length / 4; ++i) {
		const byte *pixel = pixels + i * channels;
		rgba[i * 4] = pixel[0]; rgba[i * 4 + 1] = channels > 1 ? pixel[1] : 0;
		rgba[i * 4 + 2] = channels > 2 ? pixel[2] : 0; rgba[i * 4 + 3] = channels > 3 ? pixel[3] : 255;
	}
	Graphics::Metal::Texture *texture = nullptr;
	if (mTarget == eTextureTarget_3D) texture = _graphics->metal().createVolumeTexture(rgba.data(), rgba.size(), size.x, size.y, size.z);
	else {
		Graphics::Surface surface;
		surface.init(size.x, size.y, size.x * 4, rgba.data(), Graphics::PixelFormat::createFormatRGBA32());
		texture = _graphics->metal().createTexture(surface, mbUseMipMaps && mTarget != eTextureTarget_Rect);
	}
	if (!texture) return false;
	clear(); _frames.push_back(texture); _width = size.x; _height = size.y; _bpp = channels * 8; _alpha = channels == 4;
	return true;
}
void MetalTexture::advance(float amount) {
	if (_frames.size() < 2 || mAnimMode == eTextureAnimMode_None) return;
	_time += amount * _direction;
	float maximum = _frames.size();
	if (_direction > 0 && _time >= maximum) {
		if (mAnimMode == eTextureAnimMode_Loop) _time = 0;
		else { _time = maximum - 1; _direction = -1; }
	} else if (_direction < 0 && _time < 0) { _time = 1; _direction = 1; }
}
void MetalTexture::Update(float step) { if (mfFrameTime > 0) advance(step / mfFrameTime); }
void MetalTexture::NextFrame() { advance(1); }
void MetalTexture::PrevFrame() { advance(-1); }
float MetalTexture::GetT() { return cMath::Modulus(_time, 1.0f); }
int MetalTexture::GetCurrentLowlevelHandle() { return _id + CLIP((int)_time, 0, MAX(0, (int)_frames.size() - 1)); }

} // End of namespace hpl

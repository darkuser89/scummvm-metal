/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hpl1/engine/impl/low_level_graphics_metal.h"
#include "hpl1/engine/impl/metal_resources.h"
#include "hpl1/engine/impl/metal_vertex_buffer.h"
#include "hpl1/engine/impl/metal_fixed_shaders.h"
#include "hpl1/engine/graphics/OcclusionQuery.h"
#include "hpl1/engine/graphics/font_data.h"
#include "hpl1/engine/math/Math.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "graphics/cursorman.h"
#include "engines/util.h"
#include <cmath>

namespace hpl {

namespace {

Graphics::Metal::Compare compareFunction(uint value) {
	static const Graphics::Metal::Compare functions[] = {
		Graphics::Metal::kNever, Graphics::Metal::kLess, Graphics::Metal::kLessEqual,
		Graphics::Metal::kGreater, Graphics::Metal::kGreaterEqual, Graphics::Metal::kEqual,
		Graphics::Metal::kNotEqual, Graphics::Metal::kAlways
	};
	return value < ARRAYSIZE(functions) ? functions[value] : Graphics::Metal::kAlways;
}

Graphics::Metal::BlendFactor blendFactor(eBlendFunc value) {
	static const Graphics::Metal::BlendFactor factors[] = {
		Graphics::Metal::kBlendZero, Graphics::Metal::kBlendOne,
		Graphics::Metal::kSrcColor, Graphics::Metal::kOneMinusSrcColor,
		Graphics::Metal::kDstColor, Graphics::Metal::kOneMinusDstColor,
		Graphics::Metal::kSrcAlpha, Graphics::Metal::kOneMinusSrcAlpha,
		Graphics::Metal::kDstAlpha, Graphics::Metal::kOneMinusDstAlpha,
		Graphics::Metal::kSrcAlphaSaturate
	};
	return uint(value) < ARRAYSIZE(factors) ? factors[value] : Graphics::Metal::kBlendOne;
}

Graphics::Metal::Wrap textureWrap(eTextureWrap value) {
	if (value == eTextureWrap_Repeat) return Graphics::Metal::kRepeat;
	if (value == eTextureWrap_ClampToBorder) return Graphics::Metal::kBorder;
	return Graphics::Metal::kClamp;
}

void colorFloats(float *destination, const cColor &color) {
	destination[0] = color.r; destination[1] = color.g;
	destination[2] = color.b; destination[3] = color.a;
}

void vectorFloats(float *destination, const cVector3f &vector, float w) {
	destination[0] = vector.x; destination[1] = vector.y;
	destination[2] = vector.z; destination[3] = w;
}

void matrixFloats(float *destination, const cMatrixf &matrix) {
	cMatrixf transposed = matrix.GetTranspose();
	memcpy(destination, transposed.v, 16 * sizeof(float));
}

MetalShaderVertex vertex(const cVertex &source, const cVector3f &normal = cVector3f(0, 0, 1)) {
	MetalShaderVertex result = {};
	vectorFloats(result.position, source.pos, 1);
	vectorFloats(result.normal, normal, 0);
	colorFloats(result.color, source.col);
	for (uint i = 0; i < 8; ++i)
		vectorFloats(result.texcoord[i], source.tex, 1);
	return result;
}

class MetalQuery : public iOcclusionQuery {
public:
	explicit MetalQuery(Graphics::Metal::Renderer &renderer)
		: _renderer(renderer), _query(renderer.createOcclusionQuery()) {
		if (!_query) error("HPL1: cannot allocate native visibility query");
	}
	~MetalQuery() override { delete _query; }
	void Begin() override { _renderer.beginOcclusionQuery(*_query); }
	void End() override { _renderer.endOcclusionQuery(*_query); }
	bool FetchResults() override {
		if (!_renderer.occlusionQueryReady(*_query)) return false;
		_samples = MIN<uint64>(_renderer.occlusionQueryResult(*_query, false), 0xffffffffu);
		return true;
	}
	unsigned int GetSampleCount() override { return _samples; }
private:
	Graphics::Metal::Renderer &_renderer;
	Graphics::Metal::OcclusionQuery *_query;
	uint32 _samples = 0;
};

} // End of anonymous namespace

LowLevelGraphicsMetal::LowLevelGraphicsMetal() {
	for (uint i = 0; i < 10; ++i) _matrices[i] = cMatrixf::Identity;
	_uniforms.nativeParameters[2] = Graphics::Metal::kAlways;
	colorFloats(_uniforms.currentColor, _currentColor);
	for (uint i = 0; i < 8; ++i) {
		MetalTextureEnvironment &env = _uniforms.environments[i];
		env.functions[2] = env.functions[3] = 1;
		env.rgbSources[0] = env.alphaSources[0] = eTextureSource_Texture;
		env.rgbSources[1] = env.alphaSources[1] = eTextureSource_Previous;
		env.rgbSources[2] = env.alphaSources[2] = eTextureSource_Constant;
		env.rgbOperands[2] = eTextureOp_Alpha;
		for (uint j = 0; j < 3; ++j) env.alphaOperands[j] = eTextureOp_Alpha;
	}
}

LowLevelGraphicsMetal::~LowLevelGraphicsMetal() {
	if (_metal) _metal->flush(true);
	for (auto &entry : _fixedPrograms) delete entry._value;
	delete _gammaProgram;
	delete _gammaTexture;
	delete _white;
	delete _whiteCube;
	delete _whiteVolume;
}

bool LowLevelGraphicsMetal::Init(int width, int height, int bpp, int fullscreen,
		int multisampling, const tString &caption) {
	initGraphics3d(width, height, true);
	_metal = g_system->getMetalRenderer();
	if (!_metal) return false;
	_screenSize = cVector2f(_metal->width(), _metal->height());
	_virtualSize = cVector2f(width, height);
	_multisampling = _metal->sampleCount() > 1 ? _metal->sampleCount() : 0;
	Graphics::Surface white;
	white.create(1, 1, _pixelFormat);
	white.fillRect(Common::Rect(1, 1), _pixelFormat.RGBToColor(255, 255, 255));
	_white = _metal->createTexture(white);
	Graphics::Surface faces[6];
	for (uint i = 0; i < 6; ++i) faces[i].copyFrom(white);
	_whiteCube = _metal->createCubeTexture(faces);
	for (uint i = 0; i < 6; ++i) faces[i].free();
	_whiteVolume = _metal->createVolumeTexture(white.getPixels(), 4, 1, 1, 1);
	white.free();
	_gammaProgram = _metal->createProgram(kMetalGammaSource, "hplGammaVertex", "hplGammaFragment");
	if (!_white || !_whiteCube || !_whiteVolume || !_gammaProgram)
		error("HPL1: cannot initialize native Metal resources");
	SetupGL();
	ShowCursor(false);
	return true;
}

void LowLevelGraphicsMetal::SetupGL() {
	_state.depthTest = _state.depthWrite = true;
	_state.depthCompare = Graphics::Metal::kLessEqual;
	_state.srcAlpha = Graphics::Metal::kSrcAlpha;
	_metal->setViewport(Common::Rect(_metal->width(), _metal->height()));
	ClearScreen();
}

int LowLevelGraphicsMetal::GetCaps(eGraphicCaps type) const {
	switch (type) {
	case eGraphicCaps_MaxTextureImageUnits:
	case eGraphicCaps_MaxTextureCoordUnits: return 8;
	case eGraphicCaps_MaxAnisotropicFiltering: return 16;
	case eGraphicCaps_TextureTargetRectangle:
	case eGraphicCaps_VertexBufferObject:
	case eGraphicCaps_TwoSideStencil:
	case eGraphicCaps_AnisotropicFiltering:
	case eGraphicCaps_Multisampling:
	case eGraphicCaps_GL_GpuPrograms:
	case eGraphicCaps_GL_BlendFunctionSeparate:
	case eGraphicCaps_GL_MultiTexture: return 1;
	default: return 0;
	}
}

void LowLevelGraphicsMetal::ShowCursor(bool show) { CursorMan.showMouse(show); }
void LowLevelGraphicsMetal::SetMultisamplingActive(bool active) {
	_metal->setMultisamplingActive(active);
}
void LowLevelGraphicsMetal::SetGammaCorrection(float gamma) { _gamma = gamma; }
float LowLevelGraphicsMetal::GetGammaCorrection() { return _gamma; }
cVector2f LowLevelGraphicsMetal::GetScreenSize() { return _screenSize; }
cVector2f LowLevelGraphicsMetal::GetVirtualSize() { return _virtualSize; }
void LowLevelGraphicsMetal::SetVirtualSize(cVector2f size) { _virtualSize = size; }
Graphics::PixelFormat *LowLevelGraphicsMetal::GetPixelFormat() { return &_pixelFormat; }

Bitmap2D *LowLevelGraphicsMetal::CreateBitmap2D(const cVector2l &size) { return new Bitmap2D(size, _pixelFormat); }
FontData *LowLevelGraphicsMetal::CreateFontData(const tString &name) { return new FontData(name, this); }
iTexture *LowLevelGraphicsMetal::CreateTexture(bool mipmaps, eTextureType type, eTextureTarget target) {
	return CreateTexture("", mipmaps, type, target);
}
iTexture *LowLevelGraphicsMetal::CreateTexture(const tString &name, bool mipmaps, eTextureType type, eTextureTarget target) {
	return new MetalTexture(name, &_pixelFormat, this, type, mipmaps, target);
}
iTexture *LowLevelGraphicsMetal::CreateTexture(Bitmap2D *bitmap, bool mipmaps, eTextureType type, eTextureTarget target) {
	iTexture *texture = CreateTexture(mipmaps, type, target);
	if (!texture->CreateFromBitmap(bitmap)) { delete texture; return nullptr; }
	return texture;
}
iTexture *LowLevelGraphicsMetal::CreateTexture(const cVector2l &size, int bpp, cColor color,
		bool mipmaps, eTextureType type, eTextureTarget target) {
	iTexture *texture = CreateTexture(mipmaps, type, target);
	if (!texture->Create(size.x, size.y, color)) { delete texture; return nullptr; }
	return texture;
}
iGpuProgram *LowLevelGraphicsMetal::CreateGpuProgram(const tString &vertexName, const tString &fragmentName) {
	return new MetalGpuProgram(this, vertexName, fragmentName);
}
void LowLevelGraphicsMetal::SaveScreenToBMP(const tString &file) {
	_metal->flush(true);
	g_system->saveScreenshot();
}

cMatrixf LowLevelGraphicsMetal::matrix(eMatrix type) const {
	return _matrices[type == eMatrix_Texture ? 2 + _activeUnit : uint(type)];
}
void LowLevelGraphicsMetal::PushMatrix(eMatrix type) {
	uint i = type == eMatrix_Texture ? 2 + _activeUnit : uint(type);
	_matrixStacks[i].push_back(_matrices[i]);
}
void LowLevelGraphicsMetal::PopMatrix(eMatrix type) {
	uint i = type == eMatrix_Texture ? 2 + _activeUnit : uint(type);
	if (_matrixStacks[i].empty()) error("HPL1: matrix stack underflow");
	_matrices[i] = _matrixStacks[i].back();
	_matrixStacks[i].pop_back();
}
void LowLevelGraphicsMetal::SetMatrix(eMatrix type, const cMatrixf &value) {
	_matrices[type == eMatrix_Texture ? 2 + _activeUnit : uint(type)] = value;
}
void LowLevelGraphicsMetal::SetIdentityMatrix(eMatrix type) { SetMatrix(type, cMatrixf::Identity); }
void LowLevelGraphicsMetal::TranslateMatrix(eMatrix type, const cVector3f &position) {
	SetMatrix(type, cMath::MatrixMul(matrix(type), cMath::MatrixTranslate(position)));
}
void LowLevelGraphicsMetal::ScaleMatrix(eMatrix type, const cVector3f &scale) {
	SetMatrix(type, cMath::MatrixMul(matrix(type), cMath::MatrixScale(scale)));
}
void LowLevelGraphicsMetal::RotateMatrix(eMatrix type, const cVector3f &axis) {
	cVector3f n = axis;
	if (!n.x && !n.y && !n.z) return;
	n.Normalise();
	// The legacy API rotates one degree about the supplied axis.
	float c = cos(kPif / 180), s = sin(kPif / 180), t = 1 - c;
	cMatrixf rotation = cMatrixf::Identity;
	rotation.v[0] = t*n.x*n.x+c; rotation.v[1] = t*n.x*n.y-s*n.z; rotation.v[2] = t*n.x*n.z+s*n.y;
	rotation.v[4] = t*n.x*n.y+s*n.z; rotation.v[5] = t*n.y*n.y+c; rotation.v[6] = t*n.y*n.z-s*n.x;
	rotation.v[8] = t*n.x*n.z-s*n.y; rotation.v[9] = t*n.y*n.z+s*n.x; rotation.v[10] = t*n.z*n.z+c;
	SetMatrix(type, cMath::MatrixMul(matrix(type), rotation));
}
void LowLevelGraphicsMetal::SetOrthoProjection(const cVector2f &size, float near, float far) {
	cMatrixf projection = cMatrixf::Identity;
	projection.v[0] = 2 / size.x; projection.v[3] = -1;
	projection.v[5] = -2 / size.y; projection.v[7] = 1;
	projection.v[10] = -2 / (far - near); projection.v[11] = -(far + near) / (far - near);
	SetMatrix(eMatrix_Projection, projection);
}
void LowLevelGraphicsMetal::SetClipPlane(int index, const cPlanef &plane) {
	if (index < 0 || index >= 6) error("HPL1: invalid clip plane");
	_clipPlanes[index] = plane;
	cMatrixf inverseTranspose = cMath::MatrixInverse(matrix(eMatrix_ModelView)).GetTranspose();
	const float p[] = {plane.a, plane.b, plane.c, plane.d};
	for (uint row = 0; row < 4; ++row) {
		_uniforms.clipPlanes[index][row] = 0;
		for (uint column = 0; column < 4; ++column)
			_uniforms.clipPlanes[index][row] += inverseTranspose.v[row * 4 + column] * p[column];
	}
}
cPlanef LowLevelGraphicsMetal::GetClipPlane(int index, const cPlanef &unused) {
	return index >= 0 && index < 6 ? _clipPlanes[index] : cPlanef();
}
void LowLevelGraphicsMetal::SetClipPlaneActive(int index, bool active) {
	if (index < 0 || index >= 6) error("HPL1: invalid clip plane");
	if (active) _uniforms.controls[0] |= 1u << index;
	else _uniforms.controls[0] &= ~(1u << index);
}

iOcclusionQuery *LowLevelGraphicsMetal::CreateOcclusionQuery() { return new MetalQuery(*_metal); }
void LowLevelGraphicsMetal::DestroyOcclusionQuery(iOcclusionQuery *query) { delete query; }
void LowLevelGraphicsMetal::SetClearColor(const cColor &color) { _clearColor = color; }
void LowLevelGraphicsMetal::SetClearDepth(float depth) { _clearDepth = depth; }
void LowLevelGraphicsMetal::SetClearStencil(int value) { _clearStencil = value; }
void LowLevelGraphicsMetal::SetClearColorActive(bool active) { _clearColorActive = active; }
void LowLevelGraphicsMetal::SetClearDepthActive(bool active) { _clearDepthActive = active; }
void LowLevelGraphicsMetal::SetClearStencilActive(bool active) { _clearStencilActive = active; }
void LowLevelGraphicsMetal::ClearScreen() {
	Common::Rect rect = _state.scissor ? _state.scissorRect : Common::Rect(_metal->width(), _metal->height());
	_metal->clearRect(rect, _clearColor.r, _clearColor.g, _clearColor.b, _clearColor.a,
		_clearColorActive, _clearDepthActive && _state.depthWrite, _clearStencilActive,
		_state.colorMask, _clearDepth, _clearStencil);
}
void LowLevelGraphicsMetal::SetColorWriteActive(bool r, bool g, bool b, bool a) { _state.colorMask = r | (g << 1) | (b << 2) | (a << 3); }
void LowLevelGraphicsMetal::SetDepthWriteActive(bool active) { _state.depthWrite = active; }
void LowLevelGraphicsMetal::SetDepthTestActive(bool active) { _state.depthTest = active; }
void LowLevelGraphicsMetal::SetDepthTestFunc(eDepthTestFunc function) { _state.depthCompare = compareFunction(function); }
void LowLevelGraphicsMetal::SetCullActive(bool active) { _state.cull = active; _state.cullFront = false; }
void LowLevelGraphicsMetal::SetCullMode(eCullMode mode) { _state.clockwise = mode != eCullMode_Clockwise; }
void LowLevelGraphicsMetal::SetAlphaTestActive(bool active) {
	_alphaActive = active;
	_uniforms.nativeParameters[2] = active ? _alphaFunction : Graphics::Metal::kAlways;
}
void LowLevelGraphicsMetal::SetAlphaTestFunc(eAlphaTestFunc function, float reference) {
	_alphaFunction = compareFunction(function);
	_uniforms.nativeParameters[1] = reference;
	if (_alphaActive) _uniforms.nativeParameters[2] = _alphaFunction;
}
void LowLevelGraphicsMetal::SetStencilActive(bool active) { _state.stencil = active; }
void LowLevelGraphicsMetal::SetStencil(eStencilFunc function, int reference, unsigned int mask,
		eStencilOp fail, eStencilOp depthFail, eStencilOp pass) {
	SetStencilTwoSide(function, function, reference, mask, fail, depthFail, pass, fail, depthFail, pass);
}
void LowLevelGraphicsMetal::SetStencilTwoSide(eStencilFunc front, eStencilFunc back, int reference,
		unsigned int mask, eStencilOp frontFail, eStencilOp frontDepthFail, eStencilOp frontPass,
		eStencilOp backFail, eStencilOp backDepthFail, eStencilOp backPass) {
	_state.stencilReference = reference;
	_state.frontStencil.compare = compareFunction(front);
	_state.backStencil.compare = compareFunction(back);
	_state.frontStencil.readMask = _state.backStencil.readMask = mask;
	_state.frontStencil.fail = Graphics::Metal::StencilOp(frontFail);
	_state.frontStencil.depthFail = Graphics::Metal::StencilOp(frontDepthFail);
	_state.frontStencil.pass = Graphics::Metal::StencilOp(frontPass);
	_state.backStencil.fail = Graphics::Metal::StencilOp(backFail);
	_state.backStencil.depthFail = Graphics::Metal::StencilOp(backDepthFail);
	_state.backStencil.pass = Graphics::Metal::StencilOp(backPass);
}
void LowLevelGraphicsMetal::SetStencilTwoSide(bool active) {
	// Metal always supplies both face descriptors; the single-face setter updates both.
}
void LowLevelGraphicsMetal::SetScissorActive(bool active) { _state.scissor = active; }
void LowLevelGraphicsMetal::SetScissorRect(const cRect2l &rect) { _state.scissorRect = Common::Rect(rect.x, rect.y + 1, rect.x + rect.w, rect.y + rect.h + 1); }
void LowLevelGraphicsMetal::SetBlendActive(bool active) { _state.blend = active; }
void LowLevelGraphicsMetal::SetBlendFunc(eBlendFunc source, eBlendFunc destination) {
	SetBlendFuncSeparate(source, destination, source, destination);
}
void LowLevelGraphicsMetal::SetBlendFuncSeparate(eBlendFunc srcRGB, eBlendFunc dstRGB, eBlendFunc srcAlpha, eBlendFunc dstAlpha) {
	_state.srcRGB = blendFactor(srcRGB); _state.dstRGB = blendFactor(dstRGB);
	_state.srcAlpha = blendFactor(srcAlpha); _state.dstAlpha = blendFactor(dstAlpha);
}
void LowLevelGraphicsMetal::SetTexture(unsigned int unit, iTexture *texture) {
	if (unit >= 8) error("HPL1: invalid texture unit");
	_textures[unit] = texture;
	_activeUnit = unit;
}
void LowLevelGraphicsMetal::releaseTexture(iTexture *texture) {
	for (uint i = 0; i < 8; ++i)
		if (_textures[i] == texture) _textures[i] = nullptr;
	if (_renderTarget == texture) SetRenderTarget(nullptr);
}
void LowLevelGraphicsMetal::SetActiveTextureUnit(unsigned int unit) {
	if (unit >= 8) error("HPL1: invalid texture unit");
	_activeUnit = unit;
}
void LowLevelGraphicsMetal::SetTextureEnv(eTextureParam parameter, int value) {
	MetalTextureEnvironment &env = _uniforms.environments[_activeUnit];
	if (parameter < 2) env.functions[parameter] = value;
	else if (parameter < 5) env.rgbSources[parameter - 2] = value;
	else if (parameter < 8) env.alphaSources[parameter - 5] = value;
	else if (parameter < 11) env.rgbOperands[parameter - 8] = value;
	else if (parameter < 14) env.alphaOperands[parameter - 11] = value;
	else if (parameter < 16) env.functions[parameter - 12] = value;
}
void LowLevelGraphicsMetal::SetTextureConstantColor(const cColor &color) { colorFloats(_uniforms.environments[_activeUnit].color, color); }
void LowLevelGraphicsMetal::SetColor(const cColor &color) { _currentColor = color; colorFloats(_uniforms.currentColor, color); }

iVertexBuffer *LowLevelGraphicsMetal::CreateVertexBuffer(tVertexFlag flags, eVertexBufferDrawType type,
		eVertexBufferUsageType usage, int vertexReserve, int indexReserve) {
	return new MetalVertexBuffer(this, flags, type, usage, vertexReserve, indexReserve);
}

void LowLevelGraphicsMetal::drawVertices(const MetalShaderVertex *vertices, uint count,
		const uint32 *indices, uint indexCount, eVertexBufferDrawType type, const Graphics::Metal::GeometryBinding *geometry) {
	if (!count || (!vertices && !(geometry && geometry->vertices)) || (indexCount && !indices && !(geometry && geometry->indices))) return;
	matrixFloats(_uniforms.mvp, cMath::MatrixMul(_matrices[1], _matrices[0]));
	matrixFloats(_uniforms.backendModelView, _matrices[0]);
	for (uint i = 0; i < 8; ++i) matrixFloats(_uniforms.textureMatrices[i], _matrices[2 + i]);
	_uniforms.nativeParameters[0] = _metal->height();
	_uniforms.controls[2] = 0;
	Graphics::Metal::TextureBinding bindings[8];
	uint kinds[8], key = 0;
	for (uint i = 0; i < 8; ++i) {
		MetalTexture *texture = static_cast<MetalTexture *>(_textures[i]);
		kinds[i] = _program ? _program->textureKind(i) : texture ? uint(texture->GetTarget()) + 1 : 2;
		key |= kinds[i] << (3 * i);
		bindings[i].texture = texture ? texture->texture() : kinds[i] == 4 ? _whiteCube : kinds[i] == 5 ? _whiteVolume : _white;
		if (texture) {
			_uniforms.controls[2] |= 1u << i;
			bindings[i].mipLinear = texture->GetFilter() == eTextureFilter_Trilinear;
			bindings[i].wrapS = textureWrap(texture->GetWrapS());
			bindings[i].wrapT = textureWrap(texture->GetWrapT());
			bindings[i].wrapR = textureWrap(texture->GetWrapR());
			bindings[i].anisotropy = CLIP<uint>(uint(texture->GetAnisotropyDegree(0)), 1, 16);
		}
		_uniforms.textureSizes[i][0] = bindings[i].texture->width();
		_uniforms.textureSizes[i][1] = bindings[i].texture->height();
		_uniforms.textureSizes[i][2] = bindings[i].texture->depth();
		_uniforms.textureSizes[i][3] = texture && texture->target() ? 1 : 0;
	}
	MetalShaderUniforms uniforms = _program ? _program->uniforms() : _uniforms;
	if (_program) memcpy(&uniforms, &_uniforms, offsetof(MetalShaderUniforms, EyePos));
	Graphics::Metal::Program *program = _program ? _program->program() : nullptr;
	if (!_program) {
		if (!_fixedPrograms.contains(key)) {
			Common::String source = metalFixedSource(kinds);
			_fixedPrograms[key] = _metal->createProgram(source.c_str(), "hplFixedVertex", "hplFixedFragment");
			if (!_fixedPrograms[key]) error("HPL1: cannot create native texture environment pipeline");
		}
		program = _fixedPrograms[key];
	}
	Common::Array<uint32> triangles;
	if (type == eVertexBufferDrawType_Quad) {
		uint elements = indexCount ? indexCount : count;
		for (uint i = 0; i + 3 < elements; i += 4) {
			uint32 q[4];
			for (uint j = 0; j < 4; ++j) q[j] = indexCount ? indices[i + j] : i + j;
			triangles.push_back(q[0]); triangles.push_back(q[1]); triangles.push_back(q[2]);
			triangles.push_back(q[0]); triangles.push_back(q[2]); triangles.push_back(q[3]);
		}
		indices = triangles.data(); indexCount = triangles.size();
	}
	Graphics::Metal::DrawState state = _state;
	state.primitive = type == eVertexBufferDrawType_Lines ? Graphics::Metal::kLines : Graphics::Metal::kTriangles;
	Graphics::Metal::GeometryBinding actualGeometry = geometry ? *geometry : Graphics::Metal::GeometryBinding();
	if (type == eVertexBufferDrawType_Quad) actualGeometry.indices = nullptr;
	_metal->drawProgram(*program, vertices, count, sizeof(*vertices), indices, indexCount,
		&uniforms, sizeof(uniforms), bindings, 8, state, nullptr, 0, &actualGeometry);
}

void LowLevelGraphicsMetal::DrawTri(const tVertexVec &vertices) { assert(vertices.size() == 3); DrawTri(vertices.data()); }
void LowLevelGraphicsMetal::DrawTri(const cVertex *vertices) {
	MetalShaderVertex packed[3];
	for (uint i = 0; i < 3; ++i) packed[i] = vertex(vertices[i], _normal);
	drawVertices(packed, 3, nullptr, 0, eVertexBufferDrawType_Tri);
	SetColor(vertices[2].col);
}
void LowLevelGraphicsMetal::DrawQuad(const tVertexVec &vertices) {
	assert(vertices.size() == 4);
	MetalShaderVertex packed[4];
	for (uint i = 0; i < 4; ++i) packed[i] = vertex(vertices[i], _normal);
	drawVertices(packed, 4, nullptr, 0, eVertexBufferDrawType_Quad);
	SetColor(vertices[3].col);
}
void LowLevelGraphicsMetal::DrawQuad(const tVertexVec &vertices, const cColor color) {
	tVertexVec colored(vertices);
	for (cVertex &v : colored) v.col = color;
	DrawQuad(colored);
}
void LowLevelGraphicsMetal::DrawQuad(const tVertexVec &vertices, const float z) {
	tVertexVec copy(vertices);
	for (cVertex &v : copy) v.tex.z = z;
	DrawQuad(copy);
}
void LowLevelGraphicsMetal::DrawQuad(const tVertexVec &vertices, const float z, const cColor &color) {
	tVertexVec copy(vertices);
	for (cVertex &v : copy) { v.tex.z = z; v.col = color; }
	DrawQuad(copy);
}
void LowLevelGraphicsMetal::DrawQuadMultiTex(const tVertexVec &vertices, const tVector3fVec &uvs) {
	assert(vertices.size() == 4);
	MetalShaderVertex packed[4];
	for (uint i = 0; i < 4; ++i) {
		packed[i] = vertex(vertices[i], _normal);
		for (uint unit = 0; unit < MIN<uint>(uvs.size() / 4, 7); ++unit)
			vectorFloats(packed[i].texcoord[unit + 1], uvs[unit * 4 + i], 1);
	}
	drawVertices(packed, 4, nullptr, 0, eVertexBufferDrawType_Quad);
	SetColor(vertices[3].col);
}
void LowLevelGraphicsMetal::DrawRect(const cVector2f &position, const cVector2f &size, float z) {
	tVertexVec vertices;
	vertices.push_back(cVertex(cVector3f(position.x, position.y, z), cVector2f(0, 0), cColor(1)));
	vertices.push_back(cVertex(cVector3f(position.x + size.x, position.y, z), cVector2f(1, 0), cColor(1)));
	vertices.push_back(cVertex(cVector3f(position.x + size.x, position.y + size.y, z), cVector2f(1, 1), cColor(1)));
	vertices.push_back(cVertex(cVector3f(position.x, position.y + size.y, z), cVector2f(0, 1), cColor(1)));
	DrawQuad(vertices);
}

void LowLevelGraphicsMetal::AddVertexToBatch(const cVertex &v) { _batch.push_back(vertex(v, v.norm)); }
void LowLevelGraphicsMetal::AddVertexToBatch(const cVertex *v, const cVector3f *translation) {
	cVertex copy = *v; copy.pos += *translation; AddVertexToBatch(copy);
}
void LowLevelGraphicsMetal::AddVertexToBatch(const cVertex *v, const cMatrixf *transform) {
	cVertex copy = *v; copy.pos = cMath::MatrixMul(*transform, copy.pos);
	copy.norm = cMath::MatrixMul(transform->GetRotation(), copy.norm); AddVertexToBatch(copy);
}
void LowLevelGraphicsMetal::AddVertexToBatch_Size2D(const cVertex *v, const cVector3f *translation,
		const cColor *color, const float &width, const float &height) {
	cVertex copy = *v; copy.pos = cVector3f(translation->x + width, translation->y + height, translation->z);
	copy.col = *color; AddVertexToBatch(copy);
}
void LowLevelGraphicsMetal::AddVertexToBatch_Raw(const cVector3f &position, const cColor &color, const cVector3f &uv) {
	AddVertexToBatch(cVertex(position, uv, color));
}
void LowLevelGraphicsMetal::AddTexCoordToBatch(unsigned int unit, const cVector3f *uv) {
	if (unit >= 8) error("HPL1: invalid batch texture unit");
	_batchTexcoords[unit].push_back(*uv);
}
void LowLevelGraphicsMetal::SetBatchTextureUnitActive(unsigned int unit, bool active) {
	if (unit >= 8) error("HPL1: invalid batch texture unit");
	_batchUnitActive[unit] = active;
}
void LowLevelGraphicsMetal::AddIndexToBatch(int index) { _batchIndices.push_back(index); }
void LowLevelGraphicsMetal::flushBatch(eVertexBufferDrawType type, tVtxBatchFlag flags, bool clear) {
	Common::Array<MetalShaderVertex> vertices(_batch);
	for (uint i = 0; i < vertices.size(); ++i) {
		if (!(flags & eVtxBatchFlag_Color0)) colorFloats(vertices[i].color, _currentColor);
		if (!(flags & eVtxBatchFlag_Normal)) vectorFloats(vertices[i].normal, _normal, 0);
		for (uint unit = 0; unit < 8; ++unit) {
			bool enabled = unit < 3 ? bool(flags & (eVtxBatchFlag_Texture0 << unit)) : _batchUnitActive[unit];
			if (_batchUnitActive[unit] && i < _batchTexcoords[unit].size())
				vectorFloats(vertices[i].texcoord[unit], _batchTexcoords[unit][i], 1);
			if (!enabled) vectorFloats(vertices[i].texcoord[unit], cVector3f(0), 1);
		}
	}
	if (!_batchIndices.empty()) drawVertices(vertices.data(), vertices.size(), _batchIndices.data(), _batchIndices.size(), type);
	if (clear) ClearBatch();
}
void LowLevelGraphicsMetal::FlushTriBatch(tVtxBatchFlag flags, bool clear) { flushBatch(eVertexBufferDrawType_Tri, flags, clear); }
void LowLevelGraphicsMetal::FlushQuadBatch(tVtxBatchFlag flags, bool clear) { flushBatch(eVertexBufferDrawType_Quad, flags, clear); }
void LowLevelGraphicsMetal::ClearBatch() {
	_batch.clear(); _batchIndices.clear();
	for (uint i = 0; i < 8; ++i) _batchTexcoords[i].clear();
}

void LowLevelGraphicsMetal::DrawLine(const cVector3f &begin, const cVector3f &end, cColor color) {
	MetalShaderVertex vertices[] = {vertex(cVertex(begin, cVector3f(0), color)), vertex(cVertex(end, cVector3f(0), color))};
	drawVertices(vertices, 2, nullptr, 0, eVertexBufferDrawType_Lines);
	SetColor(color);
}
void LowLevelGraphicsMetal::DrawBoxMaxMin(const cVector3f &max, const cVector3f &min, cColor color) {
	cVector3f corners[8];
	for (uint i = 0; i < 8; ++i) corners[i] = cVector3f(i & 1 ? max.x : min.x, i & 2 ? max.y : min.y, i & 4 ? max.z : min.z);
	for (uint i = 0; i < 8; ++i)
		for (uint bit = 1; bit <= 4; bit <<= 1)
			if (!(i & bit)) DrawLine(corners[i], corners[i | bit], color);
}
void LowLevelGraphicsMetal::DrawSphere(const cVector3f &center, float radius, cColor color) {
	for (uint plane = 0; plane < 3; ++plane) {
		for (uint i = 0; i < 32; ++i) {
			float a = i * k2Pif / 32, b = (i + 1) * k2Pif / 32;
			cVector3f first(0), second(0);
			if (plane == 0) { first = cVector3f(cos(a), sin(a), 0); second = cVector3f(cos(b), sin(b), 0); }
			if (plane == 1) { first = cVector3f(cos(a), 0, sin(a)); second = cVector3f(cos(b), 0, sin(b)); }
			if (plane == 2) { first = cVector3f(0, cos(a), sin(a)); second = cVector3f(0, cos(b), sin(b)); }
			DrawLine(center + first * radius, center + second * radius, color);
		}
	}
}
void LowLevelGraphicsMetal::DrawLine2D(const cVector2f &begin, const cVector2f &end, float z, cColor color) {
	SetTexture(0, nullptr); SetBlendActive(false);
	DrawLine(cVector3f(begin.x, begin.y, z), cVector3f(end.x, end.y, z), color);
}
void LowLevelGraphicsMetal::DrawLineRect2D(const cRect2f &rect, float z, cColor color) {
	DrawLine2D(cVector2f(rect.x, rect.y), cVector2f(rect.x + rect.w, rect.y), z, color);
	DrawLine2D(cVector2f(rect.x + rect.w, rect.y), cVector2f(rect.x + rect.w, rect.y + rect.h), z, color);
	DrawLine2D(cVector2f(rect.x + rect.w, rect.y + rect.h), cVector2f(rect.x, rect.y + rect.h), z, color);
	DrawLine2D(cVector2f(rect.x, rect.y + rect.h), cVector2f(rect.x, rect.y), z, color);
}
void LowLevelGraphicsMetal::DrawLineCircle2D(const cVector2f &center, float radius, float z, cColor color) {
	for (uint i = 0; i < 32; ++i) {
		float a = i * k2Pif / 32, b = (i + 1) * k2Pif / 32;
		DrawLine2D(cVector2f(center.x + cos(a) * radius, center.y + sin(a) * radius),
			cVector2f(center.x + cos(b) * radius, center.y + sin(b) * radius), z, color);
	}
}
void LowLevelGraphicsMetal::DrawFilledRect2D(const cRect2f &rect, float z, cColor color) {
	SetTexture(0, nullptr);
	tVertexVec vertices;
	vertices.push_back(cVertex(cVector3f(rect.x, rect.y, z), cVector3f(0), color));
	vertices.push_back(cVertex(cVector3f(rect.x + rect.w, rect.y, z), cVector3f(0), color));
	vertices.push_back(cVertex(cVector3f(rect.x + rect.w, rect.y + rect.h, z), cVector3f(0), color));
	vertices.push_back(cVertex(cVector3f(rect.x, rect.y + rect.h, z), cVector3f(0), color));
	DrawQuad(vertices);
}

void LowLevelGraphicsMetal::CopyContextToTexure(iTexture *texture, const cVector2l &position,
		const cVector2l &size, const cVector2l &offset) {
	if (!texture) return;
	MetalTexture *native = static_cast<MetalTexture *>(texture);
	Graphics::Metal::Texture *source = _renderTarget ? &static_cast<MetalTexture *>(_renderTarget)->target()->texture() : _metal->screenTexture();
	// GL framebuffer copies used bottom-origin storage. The shader flips target textures,
	// while ordinary copied images are physically inverted to preserve their UV convention.
	bool target = native->target() != nullptr;
	int y = target ? offset.y : native->texture()->height() - offset.y - size.y;
	_metal->copyTexture(*native->texture(), Common::Rect(offset.x, y, offset.x + size.x, y + size.y),
		*source, Common::Rect(position.x, position.y, position.x + size.x, position.y + size.y), !target);
	SetTexture(0, texture);
}
void LowLevelGraphicsMetal::SetRenderTarget(iTexture *texture) {
	if (texture && !static_cast<MetalTexture *>(texture)->target()) error("HPL1: texture is not a render target");
	_renderTarget = texture;
	_metal->setRenderTarget(texture ? static_cast<MetalTexture *>(texture)->target() : nullptr);
	_metal->setViewport(Common::Rect(_metal->width(), _metal->height()));
}
bool LowLevelGraphicsMetal::RenderTargetHasZBuffer() { return true; }
void LowLevelGraphicsMetal::FlushRenderTarget() { _metal->flush(); }
void LowLevelGraphicsMetal::FlushRendering() { _metal->flush(); }
void LowLevelGraphicsMetal::SwapBuffers() {
	if (_gamma != 1) {
		uint width = _metal->width(), height = _metal->height();
		if (!_gammaTexture || _gammaTexture->width() != width || _gammaTexture->height() != height) {
			delete _gammaTexture;
			Graphics::Surface surface;
			surface.create(width, height, _pixelFormat);
			_gammaTexture = _metal->createTexture(surface);
			surface.free();
		}
		_metal->copyTexture(*_gammaTexture, Common::Rect(width, height), *_metal->screenTexture(), Common::Rect(width, height));
		Graphics::Metal::Vertex vertices[] = {
			{{-1, 1, 0}, {0, 0}, {1, 1, 1, 1}}, {{-1, -1, 0}, {0, 1}, {1, 1, 1, 1}},
			{{1, 1, 0}, {1, 0}, {1, 1, 1, 1}}, {{1, -1, 0}, {1, 1}, {1, 1, 1, 1}}
		};
		Graphics::Metal::DrawState state;
		state.primitive = Graphics::Metal::kTriangleStrip;
		Graphics::Metal::TextureBinding binding;
		binding.texture = _gammaTexture;
		Common::Rect viewport = _metal->viewport();
		_metal->setViewport(Common::Rect(width, height));
		_metal->drawProgram(*_gammaProgram, vertices, 4, sizeof(vertices[0]), nullptr, 0,
			&_gamma, sizeof(_gamma), &binding, 1, state);
		_metal->setViewport(viewport);
	}
	_metal->flush();
	g_system->updateScreen();
}

} // End of namespace hpl

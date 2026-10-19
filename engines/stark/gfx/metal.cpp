/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "stark/gfx/metal.h"
#include "stark/gfx/metal-shaders.h"
#include "stark/gfx/bitmap.h"
#include "stark/gfx/texture.h"
#include "stark/gfx/color.h"
#include "stark/gfx/surfacerenderer.h"
#include "stark/gfx/faderenderer.h"
#include "stark/visual/actor.h"
#include "stark/visual/prop.h"
#include "stark/model/model.h"
#include "stark/model/animhandler.h"
#include "stark/formats/biffmesh.h"
#include "stark/scene.h"
#include "stark/services/services.h"
#include "stark/services/settings.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "math/vector2d.h"

namespace Stark {
namespace Gfx {

namespace Metal = Graphics::Metal;

static void copyVector(float *dest, const Math::Vector3d &v) {
	dest[0] = v.x(); dest[1] = v.y(); dest[2] = v.z();
}

static void copyMatrix(float *dest, Math::Matrix4 matrix) {
	matrix.transpose();
	memcpy(dest, matrix.getData(), sizeof(float) * 16);
}

class MetalTexture : public Texture {
public:
	explicit MetalTexture(Metal::Renderer &renderer) : _renderer(renderer) {}
	~MetalTexture() override { delete _texture; }
	void bind() const override {}
	void setLevelCount(uint32 count) override { _levelCount = count; }
	void addLevel(uint32 level, const Graphics::Surface *surface, const byte *palette) override {
		assert(surface && level < _levelCount);
		Graphics::Surface *converted = surface->format == Driver::getRGBAPixelFormat() ? nullptr :
			surface->convertTo(Driver::getRGBAPixelFormat(), palette);
		const Graphics::Surface *rgba = converted ? converted : surface;
		if (level == 0) {
			delete _texture;
			_texture = _renderer.allocateTexture(rgba->w, rgba->h, true, _levelCount);
			if (!_texture)
				error("Stark: could not allocate Metal mipmap texture");
			_renderer.updateTextureLevel(*_texture, 0, *rgba, Common::Rect(rgba->w, rgba->h));
		} else {
			assert(_texture);
			_renderer.updateTextureLevel(*_texture, level, *rgba, Common::Rect(rgba->w, rgba->h));
		}
		if (converted) {
			converted->free();
			delete converted;
		}
	}
	Metal::Texture *texture() const { return _texture; }

private:
	Metal::Renderer &_renderer;
	Metal::Texture *_texture = nullptr;
	uint32 _levelCount = 0;
};

class MetalBitmap : public Bitmap {
public:
	explicit MetalBitmap(Metal::Renderer &renderer) : _renderer(renderer) {}
	~MetalBitmap() override { delete _texture; }
	void bind() const override {}
	void update(const Graphics::Surface *surface, const byte *palette) override {
		assert(surface);
		// Empty help text produces a valid zero-sized bitmap. It has no pixel
		// storage to convert or upload, and must discard any previous image.
		if (!surface->w || !surface->h) {
			delete _texture;
			_texture = nullptr;
			_width = surface->w;
			_height = surface->h;
			return;
		}
		Graphics::Surface *converted = surface->format == getBestPixelFormat() ? nullptr :
			surface->convertTo(getBestPixelFormat(), palette);
		const Graphics::Surface *rgba = converted ? converted : surface;
		if (!_texture || _width != (uint)rgba->w || _height != (uint)rgba->h) {
			delete _texture;
			_texture = _renderer.createTexture(*rgba);
		} else {
			_renderer.updateTexture(*_texture, *rgba, Common::Rect(rgba->w, rgba->h));
		}
		if (!_texture) error("Stark: could not allocate Metal bitmap");
		_width = rgba->w; _height = rgba->h;
		if (converted) {
			converted->free();
			delete converted;
		}
	}
	void setSamplingFilter(SamplingFilter filter) override { _linear = filter == kLinear; }
	Graphics::PixelFormat getBestPixelFormat() const override { return Driver::getRGBAPixelFormat(); }
	Metal::Texture *texture() const { return _texture; }
	bool linear() const { return _linear; }

private:
	Metal::Renderer &_renderer;
	Metal::Texture *_texture = nullptr;
	bool _linear = false;
};

class MetalSurfaceRenderer : public SurfaceRenderer {
public:
	explicit MetalSurfaceRenderer(MetalDriver &driver) : _driver(driver) {}
	void render(const Bitmap *bitmap, const Common::Point &dest) override {
		render(bitmap, dest, bitmap->width(), bitmap->height());
	}
	void render(const Bitmap *bitmap, const Common::Point &dest, uint width, uint height) override {
		const MetalBitmap *image = dynamic_cast<const MetalBitmap *>(bitmap);
		assert(image);
		if (!image->width() || !image->height() || !width || !height)
			return;
		draw(dest, width, height, image->texture(), image->linear(), Color(255,255,255,255));
	}
	void fill(const Color &color, const Common::Point &dest, uint width, uint height) override {
		draw(dest, width, height, nullptr, false, color);
	}

private:
	void draw(const Common::Point &dest, uint width, uint height, Metal::Texture *texture, bool linear, const Color &color) {
		Common::Rect viewport = _driver.renderer().viewport(), logical = _driver.unscaledViewport();
		if (viewport.isEmpty() || logical.isEmpty()) return;
		float x = dest.x / (float)logical.width(), y = dest.y / (float)logical.height();
		float w = width / (float)(_noScalingOverride ? viewport.width() : logical.width());
		float h = height / (float)(_noScalingOverride ? viewport.height() : logical.height());
		Metal::Vertex vertices[4];
		for (uint i = 0; i < 4; ++i) {
			float px = x + (i & 1 ? w : 0), py = y + (i & 2 ? h : 0);
			if (_snapToGrid) {
				px = floorf(px * viewport.width() + 0.5f) / viewport.width();
				py = floorf(py * viewport.height() + 0.5f) / viewport.height();
			}
			vertices[i] = {{px*2-1, 1-py*2, 0}, {(float)(i&1), (float)((i>>1)&1)},
			               {color.r/255.0f, color.g/255.0f, color.b/255.0f, color.a/255.0f}};
		}
		MetalMaterialUniforms uniforms;
		Metal::UniformBindings constants; constants.extraData = &uniforms; constants.extraSize = sizeof(uniforms);
		uniforms.textured = texture != nullptr;
		uniforms.fadeLevel = _fadeLevel;
		Metal::TextureBinding binding;
		binding.texture = texture; binding.linear = linear;
		Metal::DrawState state;
		state.primitive = Metal::kTriangleStrip; state.blend = true;
		state.srcRGB = state.srcAlpha = Metal::kBlendOne;
		_driver.renderer().drawProgram(*_driver.surfaceProgram(), vertices, 4, sizeof(Metal::Vertex),
			nullptr, 0, nullptr, 0, &binding, 1, state, nullptr, 0, nullptr, &constants);
	}
	MetalDriver &_driver;
};

class MetalFadeRenderer : public FadeRenderer {
public:
	explicit MetalFadeRenderer(MetalDriver &driver) : _driver(driver) {}
	void render(float fadeLevel) override {
		Metal::Vertex vertices[] = {{{-1,1,0},{0,0},{0,0,0,1-fadeLevel}},
		                           {{1,1,0},{0,0},{0,0,0,1-fadeLevel}},
		                           {{-1,-1,0},{0,0},{0,0,0,1-fadeLevel}},
		                           {{1,-1,0},{0,0},{0,0,0,1-fadeLevel}}};
		Metal::DrawState state;
		state.primitive = Metal::kTriangleStrip; state.blend = true;
		state.srcRGB = state.srcAlpha = Metal::kBlendOne;
		_driver.renderer().draw(vertices, 4, nullptr, state, Metal::Uniforms());
	}

private:
	MetalDriver &_driver;
};

struct MetalActorVertex {
	float position1[3], position2[3], bone1, bone2, weight, normal[3], texcoord[2];
};

static_assert(sizeof(MetalMaterialUniforms) == 48, "Stark material layout");
static_assert(sizeof(MetalActorVertex) == 56, "Stark Metal actor vertex layout");
static_assert(sizeof(MetalUniforms) == 3088, "Stark Metal uniform layout");

static Math::Vector3d shadowDirection(const LightEntryArray &lights, const Math::Vector3d &position,
		const Math::Matrix3 &worldToModel) {
	Math::Vector3d sum;
	bool hasLight = false;
	for (uint i = 1; i < lights.size(); ++i) {
		const LightEntry &light = *lights[i];
		float brightness = (light.color.x() + light.color.y() + light.color.z()) / 3;
		if (brightness <= 0) continue;
		Math::Vector3d direction;
		if (light.type == LightEntry::kDirectional) {
			direction = light.direction.getNormalized() * brightness;
		} else if (light.type == LightEntry::kPoint || light.type == LightEntry::kSpot) {
			float distance = light.position.getDistanceTo(position);
			if (distance > light.falloffFar) continue;
			float factor = distance <= light.falloffNear ? 1 :
				(light.falloffFar - light.falloffNear > 1 ? 1 - (distance-light.falloffNear)/(light.falloffFar-light.falloffNear) : 0);
			direction = (position-light.position).getNormalized();
			if (light.type == LightEntry::kSpot) {
				float cosine = MAX(0.0f, direction.dotProduct(light.direction));
				factor *= CLIP((cosine-light.innerConeAngle.getCosine()) /
					MAX(0.001f, light.outerConeAngle.getCosine()-light.innerConeAngle.getCosine()), 0.0f, 1.0f);
			}
			if (factor <= 0) continue;
			direction *= factor * brightness;
		} else continue;
		sum += direction;
		hasLight = true;
	}
	if (hasLight) {
		Math::Vector2d horizontal(sum.x(), sum.y());
		float length = MIN(horizontal.getMagnitude(), StarkScene->getMaxShadowLength());
		horizontal.normalize(); horizontal *= length;
		sum.x() = horizontal.getX(); sum.y() = horizontal.getY();
	}
	sum.z() = -1;
	return worldToModel * sum;
}

struct MetalMeshBuffers {
	Metal::Buffer *vertices = nullptr, *indices = nullptr;
	Common::Array<uint32> offsets;
	~MetalMeshBuffers() { delete vertices; delete indices; }
	void appendFace(Common::Array<uint32> &all, const Common::Array<uint32> &face, uint vertexCount) {
		offsets.push_back(all.size() * sizeof(uint32));
		for (uint32 index : face) {
			if (index >= vertexCount) error("Stark: persistent mesh index out of range");
			all.push_back(index);
		}
	}
	void upload(Metal::Renderer &renderer, const void *data, uint bytes, const Common::Array<uint32> &all) {
		delete vertices; delete indices;
		vertices = bytes ? renderer.createBuffer(data, bytes) : nullptr;
		indices = all.empty() ? nullptr : renderer.createBuffer(all.data(), all.size() * sizeof(uint32));
		if ((bytes && !vertices) || (!all.empty() && !indices)) error("Stark: could not allocate persistent mesh");
	}
	Metal::GeometryBinding binding(uint face) const {
		Metal::GeometryBinding result; result.vertices = vertices; result.indices = indices; result.indexOffset = offsets[face]; return result;
	}
};

class MetalActorRenderer : public VisualActor {
public:
	explicit MetalActorRenderer(MetalDriver &driver) : _driver(driver) {}
	~MetalActorRenderer() override { delete _frameUniforms; }
	void render(const Math::Vector3d &position, float direction, const LightEntryArray &lights) override {
		if (_modelIsDirty) {
			const Common::Array<VertNode *> &source = _model->getVertices();
			_vertices.resize(source.size());
			for (uint i = 0; i < source.size(); ++i) {
				const VertNode &v = *source[i];
				MetalActorVertex &out = _vertices[i];
				copyVector(out.position1, v._pos1); copyVector(out.position2, v._pos2);
				copyVector(out.normal, v._normal);
				out.bone1 = v._bone1; out.bone2 = v._bone2; out.weight = v._boneWeight;
				out.texcoord[0] = -v._texS; out.texcoord[1] = v._texT;
			}
			_mesh.offsets.clear();
			Common::Array<uint32> indices;
			for (const Face *face : _model->getFaces()) _mesh.appendFace(indices, face->vertexIndices, _vertices.size());
			_mesh.upload(_driver.renderer(), _vertices.data(), _vertices.size() * sizeof(MetalActorVertex), indices);
			_modelIsDirty = false;
		}
		_animHandler->animate(_time);
		_model->updateBoundingBox();
		const Common::Array<BoneNode *> &bones = _model->getBones();
		if (bones.size() > 70) error("Stark: Metal actor exceeds 70 bones");
		Math::Matrix4 model = getModelMatrix(position, direction);
		MetalUniforms uniforms;
		_driver.setupUniforms(uniforms, model, lights);
		for (uint i = 0; i < bones.size(); ++i) {
			copyVector(uniforms.bonePosition[i], bones[i]->_animPos);
			const Math::Quaternion &rotation = bones[i]->_animRot;
			uniforms.boneRotation[i][0] = rotation.x(); uniforms.boneRotation[i][1] = rotation.y();
			uniforms.boneRotation[i][2] = rotation.z(); uniforms.boneRotation[i][3] = rotation.w();
		}
		for (uint i = 0; i < _vertices.size(); ++i)
			if (_vertices[i].bone1 >= bones.size() || _vertices[i].bone2 >= bones.size())
				error("Stark: invalid Metal actor bone index");
		if (_frameUniforms) _driver.renderer().updateBuffer(*_frameUniforms, &uniforms, sizeof(uniforms));
		else _frameUniforms = _driver.renderer().createBuffer(&uniforms, sizeof(uniforms));
		if (!_frameUniforms) error("Stark: could not allocate frame uniforms");
		MetalMaterialUniforms drawUniforms;
		Metal::UniformBindings constants; constants.vertex = _frameUniforms;
		constants.extraData = &drawUniforms; constants.extraSize = sizeof(drawUniforms);
		Metal::DrawState state;
		state.depthTest = state.depthWrite = true; state.depthCompare = Metal::kLess;
		const Common::Array<Material *> &materials = _model->getMaterials();
		uint faceIndex = 0;
		for (const Face *face : _model->getFaces()) {
			Metal::GeometryBinding geometry = _mesh.binding(faceIndex++);
			if (face->vertexIndices.empty()) continue;
			const Material *material = materials[face->materialId];
			const MetalTexture *texture = dynamic_cast<const MetalTexture *>(resolveTexture(material));
			drawUniforms.textured = texture != nullptr;
			drawUniforms.color[0] = material->r; drawUniforms.color[1] = material->g; drawUniforms.color[2] = material->b;
			Metal::TextureBinding binding;
			binding.texture = texture ? texture->texture() : nullptr;
			binding.wrapS = binding.wrapT = Metal::kMirror; binding.mipLinear = false;
			_driver.renderer().drawProgram(*_driver.actorProgram(), nullptr, _vertices.size(), sizeof(MetalActorVertex),
				nullptr, face->vertexIndices.size(), nullptr, 0, &binding, 1, state, nullptr, 0, &geometry, &constants);
		}
		if (_castsShadow && StarkScene->shouldRenderShadows() && StarkSettings->getBoolSetting(Settings::kShadow)) {
			Math::Matrix4 inverse = model; inverse.inverse();
			Math::Vector3d shadowVector = shadowDirection(lights, position, inverse.getRotation());
			// Models whose ground-plane direction is parallel have no finite projection.
			if (fabsf(shadowVector.y()) < 0.00001f) return;
			copyVector(drawUniforms.shadowDirection, shadowVector);
			drawUniforms.textured = 0;
			state.blend = true; state.srcAlpha = Metal::kSrcAlpha;
			state.stencil = true;
			state.frontStencil.compare = state.backStencil.compare = Metal::kEqual;
			state.frontStencil.pass = state.backStencil.pass = Metal::kIncrementClamp;
			faceIndex = 0;
			for (const Face *face : _model->getFaces()) {
				Metal::GeometryBinding geometry = _mesh.binding(faceIndex++);
				if (face->vertexIndices.empty()) continue;
				_driver.renderer().drawProgram(*_driver.shadowProgram(), nullptr, _vertices.size(), sizeof(MetalActorVertex),
					nullptr, face->vertexIndices.size(), nullptr, 0, nullptr, 0, state, nullptr, 0, &geometry, &constants);
			}
		}
	}

private:
	MetalDriver &_driver;
	Common::Array<MetalActorVertex> _vertices;
	MetalMeshBuffers _mesh;
	Metal::Buffer *_frameUniforms = nullptr;
};

class MetalPropRenderer : public VisualProp {
public:
	explicit MetalPropRenderer(MetalDriver &driver) : _driver(driver) {}
	~MetalPropRenderer() override { delete _frameUniforms; }
	void render(const Math::Vector3d &position, float direction, const LightEntryArray &lights) override {
		MetalUniforms uniforms;
		_driver.setupUniforms(uniforms, getModelMatrix(position, direction), lights);
		const Common::Array<Formats::BiffMesh::Vertex> &source = _model->getVertices();
		if (_uploadedRevision != _modelRevision || !_mesh.vertices) {
			Common::Array<float> vertices;
			vertices.resize(source.size() * 9);
			for (uint i = 0; i < source.size(); ++i) {
				copyVector(vertices.data() + i*9, source[i].position);
				copyVector(vertices.data() + i*9+3, source[i].normal);
				copyVector(vertices.data() + i*9+6, source[i].texturePosition);
			}
			_mesh.offsets.clear();
			Common::Array<uint32> indices;
			for (const Face &face : _model->getFaces()) _mesh.appendFace(indices, face.vertexIndices, source.size());
			_mesh.upload(_driver.renderer(), vertices.data(), vertices.size() * sizeof(float), indices);
			_uploadedRevision = _modelRevision;
		}
		if (_frameUniforms) _driver.renderer().updateBuffer(*_frameUniforms, &uniforms, sizeof(uniforms));
		else _frameUniforms = _driver.renderer().createBuffer(&uniforms, sizeof(uniforms));
		if (!_frameUniforms) error("Stark: could not allocate frame uniforms");
		MetalMaterialUniforms drawUniforms;
		Metal::UniformBindings constants; constants.vertex = _frameUniforms;
		constants.extraData = &drawUniforms; constants.extraSize = sizeof(drawUniforms);
		Metal::DrawState state;
		state.depthTest = state.depthWrite = true; state.depthCompare = Metal::kLess;
		uint faceIndex = 0;
		for (const Face &face : _model->getFaces()) {
			Metal::GeometryBinding geometry = _mesh.binding(faceIndex++);
			if (face.vertexIndices.empty()) continue;
			const Material &material = _model->getMaterials()[face.materialId];
			const MetalTexture *texture = dynamic_cast<const MetalTexture *>(_texture->getTexture(material.texture));
			drawUniforms.textured = texture != nullptr; drawUniforms.doubleSided = material.doubleSided;
			drawUniforms.color[0] = material.r; drawUniforms.color[1] = material.g; drawUniforms.color[2] = material.b;
			Metal::TextureBinding binding;
			binding.texture = texture ? texture->texture() : nullptr;
			binding.wrapS = binding.wrapT = Metal::kMirror; binding.mipLinear = false;
			_driver.renderer().drawProgram(*_driver.propProgram(), nullptr, source.size(), 9*sizeof(float),
				nullptr, face.vertexIndices.size(), nullptr, 0, &binding, 1, state, nullptr, 0, &geometry, &constants);
		}
	}

private:
	MetalDriver &_driver;
	MetalMeshBuffers _mesh;
	Metal::Buffer *_frameUniforms = nullptr;
	uint64 _uploadedRevision = 0;
};

MetalDriver::MetalDriver() : _renderer(g_system->getMetalRenderer()) {
	_computeLights = true;
	if (!_renderer) error("Stark: Metal renderer unavailable");
}

MetalDriver::~MetalDriver() {
	_renderer->flush(true);
	delete _actor; delete _prop; delete _shadow; delete _surface;
}

void MetalDriver::init() {
	computeScreenViewport();
	setScreenViewport(false);
	_actor = _renderer->createProgram(kMetalShaderSource, "starkActor", "starkFragment");
	_prop = _renderer->createProgram(kMetalShaderSource, "starkProp", "starkFragment");
	_shadow = _renderer->createProgram(kMetalShaderSource, "starkShadow", "starkFragment");
	_surface = _renderer->createProgram(kMetalShaderSource, "starkSurface", "starkFragment");
	if (!_actor || !_prop || !_shadow || !_surface) error("Stark: could not create Metal shader programs");
}

void MetalDriver::setScreenViewport(bool noScaling) {
	_renderer->setViewport(noScaling ? Common::Rect(g_system->getWidth(),g_system->getHeight()) : _screenViewport);
	_unscaledViewport = noScaling ? _renderer->viewport() : Common::Rect(kOriginalWidth,kOriginalHeight);
}

void MetalDriver::setViewport(const Common::Rect &r) {
	Common::Rect viewport(_screenViewport.width()*r.width()/kOriginalWidth, _screenViewport.height()*r.height()/kOriginalHeight);
	viewport.translate(_screenViewport.left+_screenViewport.width()*r.left/kOriginalWidth,
		_screenViewport.top+_screenViewport.height()*r.top/kOriginalHeight);
	_renderer->setViewport(viewport);
	_unscaledViewport = r;
}

void MetalDriver::clearScreen() { _renderer->clear(0,0,0,0); }
void MetalDriver::flipBuffer() { g_system->updateScreen(); }
Texture *MetalDriver::createTexture() { return new MetalTexture(*_renderer); }
Bitmap *MetalDriver::createBitmap(const Graphics::Surface *surface, const byte *palette) {
	MetalBitmap *bitmap = new MetalBitmap(*_renderer);
	if (surface) bitmap->update(surface,palette);
	return bitmap;
}
VisualActor *MetalDriver::createActorRenderer() { return new MetalActorRenderer(*this); }
VisualProp *MetalDriver::createPropRenderer() { return new MetalPropRenderer(*this); }
SurfaceRenderer *MetalDriver::createSurfaceRenderer() { return new MetalSurfaceRenderer(*this); }
FadeRenderer *MetalDriver::createFadeRenderer() { return new MetalFadeRenderer(*this); }

Graphics::Surface *MetalDriver::getViewportScreenshot(uint w, uint h) const {
	Graphics::Surface pixels;
	if (!_renderer->readPixels(pixels, _renderer->viewport())) return nullptr;
	Graphics::Surface *result = pixels.scale(w,h,false);
	pixels.free();
	return result;
}

void MetalDriver::setupUniforms(MetalUniforms &uniforms, const Math::Matrix4 &model, const LightEntryArray &lights) const {
	memset(&uniforms,0,sizeof(uniforms));
	Math::Matrix4 view = StarkScene->getViewMatrix();
	Math::Matrix4 mv = view * model;
	copyMatrix(uniforms.modelView,mv);
	copyMatrix(uniforms.projection,StarkScene->getProjectionMatrix());
	mv.transpose();
	mv.invertAffineOrthonormal();
	Math::Matrix3 normal = mv.getRotation();
	const float *n = normal.getData();
	for (uint c=0;c<3;++c) for (uint r=0;r<3;++r) uniforms.normal[c*4+r] = n[c*3+r];
	uniforms.normal[15] = 1;
	if (!lights.empty()) copyVector(uniforms.ambient,lights[0]->color);
	for (uint i = 1; i < lights.size() && i <= 10; ++i) {
		const LightEntry &light = *lights[i];
		MetalLight &out = uniforms.lights[i-1];
		Math::Vector4d position = view * Math::Vector4d(light.position.x(),light.position.y(),light.position.z(),1);
		copyVector(out.position,position.getXYZ()); out.position[3] = light.type;
		Math::Vector3d direction = (view.getRotation()*light.direction).getNormalized();
		copyVector(out.direction,direction); copyVector(out.color,light.color);
		out.parameters[0] = light.falloffNear; out.parameters[1] = light.falloffFar;
		out.parameters[2] = light.innerConeAngle.getCosine(); out.parameters[3] = light.outerConeAngle.getCosine();
	}
}

} // End of namespace Gfx
} // End of namespace Stark

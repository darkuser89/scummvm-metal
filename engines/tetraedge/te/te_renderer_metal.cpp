/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tetraedge/te/te_renderer_metal.h"
#include "tetraedge/te/te_metal_shaders.h"
#include "tetraedge/tetraedge.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "engines/util.h"

namespace Tetraedge {

namespace Metal = Graphics::Metal;

static TeRendererMetal &nativeRenderer() {
	TeRendererMetal *renderer = dynamic_cast<TeRendererMetal *>(g_engine->getRenderer());
	if (!renderer) error("Tetraedge: Metal renderer unavailable");
	return *renderer;
}

static void colorFloats(float *values, const TeColor &color) {
	values[0] = color.r()/255.0f; values[1] = color.g()/255.0f;
	values[2] = color.b()/255.0f; values[3] = color.a()/255.0f;
}

static void vectorFloats(float *values, const TeVector3f32 &v) {
	values[0] = v.x(); values[1] = v.y(); values[2] = v.z();
}

static TeMetalVertex makeVertex(const TeVector3f32 &position, const TeVector3f32 &normal,
		const TeVector2f32 &uv, const TeColor &color) {
	TeMetalVertex vertex;
	vectorFloats(vertex.position, position); vectorFloats(vertex.normal, normal);
	vertex.uv[0] = uv.getX(); vertex.uv[1] = uv.getY();
	colorFloats(vertex.color, color);
	return vertex;
}

static_assert(sizeof(TeMetalVertex) == 48, "Tetraedge Metal vertex layout");
static_assert(sizeof(TeMetalUniforms) == 1328, "Tetraedge Metal uniform layout");

TeRendererMetal::TeRendererMetal() {}
TeRendererMetal::~TeRendererMetal() {
	if (_metal) { _metal->setRenderTarget(nullptr); _metal->flush(true); }
	delete _program;
}

void TeRendererMetal::init(uint width, uint height) {
	initGraphics3d(width,height,true);
	_metal = g_system->getMetalRenderer();
	if (!_metal) error("Tetraedge: no Metal backend");
	_program = _metal->createProgram(kTeMetalShaderSource,"teVertex","teFragment");
	if (!_program) error("Tetraedge: could not create Metal programs");
	_state.blend = true; _state.depthTest = _state.depthWrite = true;
	_state.depthCompare = Metal::kLessEqual;
	_currentColor = TeColor(255,255,255,255); _clearColor = TeColor(0,0,0,255);
	for (uint i=0;i<8;++i) _uniforms.lights[i].parameters[0] = -1;
	updateGlobalLight();
}

void TeRendererMetal::clearBuffer(Buffer buffers) {
	Common::Rect area(_metal->width(),_metal->height());
	if (_scissorEnabled)
		area.clip(Common::Rect(_scissorX, (int)_metal->height()-_scissorY-_scissorHeight,
			_scissorX+_scissorWidth, (int)_metal->height()-_scissorY));
	_metal->clearRect(area,_clearColor.r()/255.0f,_clearColor.g()/255.0f,_clearColor.b()/255.0f,_clearColor.a()/255.0f,
		buffers&ColorBuffer,buffers&DepthBuffer,buffers&StencilBuffer,_state.colorMask);
}

void TeRendererMetal::colorMask(bool r,bool g,bool b,bool a) { _state.colorMask=(r?1:0)|(g?2:0)|(b?4:0)|(a?8:0); }

void TeRendererMetal::drawLine(const TeVector3f32 &from,const TeVector3f32 &to) {
	TeMetalVertex vertices[] = {makeVertex(from,TeVector3f32(0,0,1),TeVector2f32(),_currentColor),
	                           makeVertex(to,TeVector3f32(0,0,1),TeVector2f32(),_currentColor)};
	const uint32 indices[] = {0,1};
	setMatrixMode(MM_GL_MODELVIEW); loadCurrentMatrixToGL();
	draw(vertices,2,indices,2,TeMesh::MeshMode_Lines,0);
}

void TeRendererMetal::loadProjectionMatrix(const TeMatrix4x4 &matrix) {
	_matrixMode = MM_GL_PROJECTION; _matriciesStacks[_matrixMode].loadMatrix(matrix);
	_loadedMatrices[_matrixMode] = matrix;
	_matrixMode = MM_GL_MODELVIEW; _matriciesStacks[_matrixMode].loadIdentity();
}

void TeRendererMetal::reset() {
	clearBuffer(AllBuffers);
	for (uint i=0;i<3;++i) {
		_matriciesStacks[i].loadIdentity(); _loadedMatrices[i].setToIdentity();
	}
	_matrixMode = MM_GL_MODELVIEW;
}

void TeRendererMetal::setViewport(int x,int y,int width,int height) {
	_metal->setViewport(Common::Rect(x,(int)_metal->height()-y-height,x+width,(int)_metal->height()-y));
}

void TeRendererMetal::shadowMode(ShadowMode mode) {
	_shadowMode = mode;
	_state.cull = mode == ShadowModeCreating;
	if (mode != ShadowModeNone) _lighting = false;
}

void TeRendererMetal::applyMaterial(const TeMaterial &material) {
	_material = material;
	if (_shadowMode == ShadowModeNone) {
		_lighting = material._enableLights;
		if (material._texture) {
			enableTexture(); material._texture->bind();
		}
		setCurrentColor(material._diffuseColor);
	}
	if (material._isShadowTexture) { _lighting = false; enableTexture(); }
	colorFloats(_uniforms.ambient,material._ambientColor);
	colorFloats(_uniforms.diffuse,material._diffuseColor);
	colorFloats(_uniforms.specular,material._specularColor);
	colorFloats(_uniforms.emission,material._emissionColor);
	_uniforms.materialParameters[0] = material._shininess;
	_uniforms.materialParameters[1] = 0.5f;
	_uniforms.materialParameters[2] = _shadowMode != ShadowModeNone;
	_uniforms.materialParameters[3] = _shadowMode == ShadowModeNone && !material._isShadowTexture && material._mode == TeMaterial::MaterialMode2;
}

void TeRendererMetal::updateGlobalLight() { colorFloats(_uniforms.globalAmbient,TeLight::globalAmbient()); }
void TeRendererMetal::updateScreen() { g_system->updateScreen(); }

TeMetalLight &TeRendererMetal::light(uint index) {
	if (index >= 8) error("Tetraedge: Metal light index %u exceeds the classic eight-light limit",index);
	return _uniforms.lights[index];
}

void TeRendererMetal::bindTexture(Metal::Texture *texture,const TeMatrix4x4 &matrix) {
	_boundTexture = texture;
	_loadedMatrices[MM_GL_TEXTURE] = matrix;
	_matriciesStacks[MM_GL_TEXTURE].loadMatrix(matrix);
}

void TeRendererMetal::releaseTexture(Metal::Texture *texture) {
	if (_boundTexture == texture) _boundTexture = nullptr;
	if (_shadowTexture == texture) _shadowTexture = nullptr;
}

void TeRendererMetal::setShadowTexture(Metal::Texture *texture,const TeMatrix4x4 &projection) {
	_shadowTexture = texture;
	TeMatrix4x4 inverse = _loadedMatrices[MM_GL_MODELVIEW]; inverse.inverse();
	TeMatrix4x4 eyeProjection = projection * inverse;
	memcpy(_uniforms.projectedTexture,eyeProjection.getData(),sizeof(_uniforms.projectedTexture));
}

void TeRendererMetal::draw(const TeMetalVertex *vertices,uint count,const uint32 *indices,uint indexCount,
		TeMesh::Mode mode,uint textureMode) {
	if (!count || !indexCount || mode == TeMesh::MeshMode_None) return;
	memcpy(_uniforms.modelView,_loadedMatrices[MM_GL_MODELVIEW].getData(),sizeof(_uniforms.modelView));
	memcpy(_uniforms.projection,_loadedMatrices[MM_GL_PROJECTION].getData(),sizeof(_uniforms.projection));
	memcpy(_uniforms.texture,_loadedMatrices[MM_GL_TEXTURE].getData(),sizeof(_uniforms.texture));
	TeMatrix4x4 normal = _loadedMatrices[MM_GL_MODELVIEW];
	if (!normal.inverse()) normal.setToIdentity();
	normal = normal.transpose();
	memcpy(_uniforms.normal,normal.getData(),sizeof(_uniforms.normal));
	_uniforms.lighting = _lighting;
	_uniforms.projected = _material._isShadowTexture && _shadowMode != ShadowModeCreating;
	Metal::TextureBinding binding;
	binding.texture = _uniforms.projected ? _shadowTexture : (_textureEnabled ? _boundTexture : nullptr);
	if (_shadowMode == ShadowModeCreating) binding.texture = nullptr;
	binding.wrapS = binding.wrapT = _uniforms.projected ? Metal::kBorder : Metal::kClamp;
	_uniforms.textured = binding.texture != nullptr;
	_uniforms.textureMode = _material._mode == TeMaterial::MaterialMode0 && !_uniforms.projected ? 2 : textureMode;
	Metal::DrawState state = _state;
	state.scissor = _scissorEnabled;
	if (_scissorEnabled) state.scissorRect = Common::Rect(_scissorX,(int)_metal->height()-_scissorY-_scissorHeight,
		_scissorX+_scissorWidth,(int)_metal->height()-_scissorY);
	Common::Array<uint32> converted;
	switch (mode) {
	case TeMesh::MeshMode_Points: state.primitive=Metal::kPoints; break;
	case TeMesh::MeshMode_Lines: state.primitive=Metal::kLines; break;
	case TeMesh::MeshMode_LineStrip: state.primitive=Metal::kLineStrip; break;
	case TeMesh::MeshMode_LineLoop:
		converted.resize(indexCount+1); memcpy(converted.data(),indices,indexCount*sizeof(uint32)); converted[indexCount]=indices[0];
		state.primitive=Metal::kLineStrip; break;
	case TeMesh::MeshMode_TriangleStrip: state.primitive=Metal::kTriangleStrip; break;
	case TeMesh::MeshMode_TriangleFan:
		if (indexCount < 3) return;
		for (uint i=1;i+1<indexCount;++i) {converted.push_back(indices[0]);converted.push_back(indices[i]);converted.push_back(indices[i+1]);}
		state.primitive=Metal::kTriangles; break;
	default: state.primitive=Metal::kTriangles; break;
	}
	if (!converted.empty()) {indices=converted.data();indexCount=converted.size();}
	if (_uniforms.materialParameters[2] != 0) {
		// Classic flat shading uses the last vertex; Metal uses the first.
		// Expand strips and rotate each primitive while preserving its winding.
		Common::Array<uint32> flat;
		if (state.primitive == Metal::kTriangleStrip) {
			for (uint i = 0; i + 2 < indexCount; ++i) {
				flat.push_back(indices[i + 2]);
				flat.push_back(indices[i + (i & 1)]);
				flat.push_back(indices[i + 1 - (i & 1)]);
			}
			state.primitive = Metal::kTriangles;
		} else if (state.primitive == Metal::kTriangles) {
			for (uint i = 0; i + 2 < indexCount; i += 3) {
				flat.push_back(indices[i + 2]); flat.push_back(indices[i]); flat.push_back(indices[i + 1]);
			}
		} else if (state.primitive == Metal::kLines || state.primitive == Metal::kLineStrip) {
			uint step = state.primitive == Metal::kLines ? 2 : 1;
			for (uint i = 0; i + 1 < indexCount; i += step) {
				flat.push_back(indices[i + 1]); flat.push_back(indices[i]);
			}
			state.primitive = Metal::kLines;
		}
		if (!flat.empty()) {
			converted = flat;
			indices = converted.data(); indexCount = converted.size();
		}
	}
	_metal->drawProgram(*_program,vertices,count,sizeof(TeMetalVertex),indices,indexCount,&_uniforms,sizeof(_uniforms),&binding,1,state);
}

void TeRendererMetal::renderTransparentMeshes() {
	if (!_numTransparentMeshes) return;
	optimiseTransparentMeshProperties();
	Common::Array<TeMetalVertex> vertices;
	vertices.resize(_transparentMeshVertexes.size());
	for (uint i=0;i<vertices.size();++i)
		vertices[i] = makeVertex(_transparentMeshVertexes[i],_transparentMeshNormals[i],_transparentMeshCoords[i],_transparentMeshColors[i]);
	bool depthWrite = _state.depthWrite; _state.depthWrite = false;
	bool scissor = _scissorEnabled;
	int sx=_scissorX,sy=_scissorY,sw=_scissorWidth,sh=_scissorHeight;
	uint offset = 0;
	for (const TransparentMeshProperties &property : _transparentMeshProps) {
		if (!property._shouldDraw) continue;
		property._camera->applyProjection();
		setMatrixMode(MM_GL_MODELVIEW); pushMatrix(); loadMatrix(property._matrix); loadCurrentMatrixToGL();
		applyMaterial(property._material);
		Common::Array<TeMetalVertex> shadowVertices;
		if (property._material._isShadowTexture) {
			shadowVertices = vertices;
			for (TeMetalVertex &vertex : shadowVertices) colorFloats(vertex.color, currentColor());
		}
		_scissorEnabled=property._scissorEnabled;
		setScissor(property._scissorX,property._scissorY,property._scissorWidth,property._scissorHeight);
		Common::Array<uint32> indices; indices.resize(property._vertexCount);
		for (uint i=0;i<indices.size();++i) indices[i]=_transparentMeshVertexNums[offset+i];
		draw(shadowVertices.empty() ? vertices.data() : shadowVertices.data(),vertices.size(),indices.data(),indices.size(),TeMesh::MeshMode_Triangles,property._glTexEnvMode);
		offset+=property._vertexCount;
		disableTexture(); popMatrix(); TeCamera::restore();
	}
	_state.depthWrite = depthWrite; _scissorEnabled=scissor;setScissor(sx,sy,sw,sh);
	_numTransparentMeshes=_pendingTransparentMeshProperties=0;_transparentMeshProps.clear();
}

void TeMeshMetal::draw() {
	if (!worldVisible()) return;
	TeRendererMetal &renderer=nativeRenderer();
	renderer.setMatrixMode(TeRenderer::MM_GL_MODELVIEW);renderer.pushMatrix();
	renderer.multiplyMatrix(_matrixForced?_forcedMatrix:worldTransformationMatrix());
	if (renderer.shadowMode()!=TeRenderer::ShadowModeCreating) {
		if (_faceCounts.empty()) {
			if (hasAlpha(0)&&_shouldDraw) {renderer.addTransparentMesh(*this,0,0,0);renderer.popMatrix();return;}
		} else {
			uint total=0;
			for (uint i=0;i<_faceCounts.size();++i) {
				if (_faceCounts[i]&&hasAlpha(i)) renderer.addTransparentMesh(*this,total,_faceCounts[i],i);
				total+=_faceCounts[i];
			}
		}
	}
	renderer.loadCurrentMatrixToGL();
	const Common::Array<TeVector3f32> &positions=_updatedVerticies.empty()?_verticies:_updatedVerticies;
	const Common::Array<TeVector3f32> &normals=_updatedVerticies.empty()?_normals:_updatedNormals;
	Common::Array<TeMetalVertex> vertices;vertices.resize(positions.size());
	for (uint i=0;i<vertices.size();++i)
		vertices[i]=makeVertex(positions[i],i<normals.size()?normals[i]:TeVector3f32(0,0,1),
			i<_uvs.size()?_uvs[i]:TeVector2f32(),i<_colors.size()?_colors[i]:renderer.currentColor());
	auto drawRange = [&](uint first,uint count,uint material) {
		if (material<_materials.size()) renderer.applyMaterial(_materials[material]);
		if (_colors.empty()) for (TeMetalVertex &v:vertices) colorFloats(v.color,renderer.currentColor());
		Common::Array<uint32> indices;indices.resize(count);
		for (uint i=0;i<count;++i) indices[i]=_indexes[first+i];
		renderer.draw(vertices.data(),vertices.size(),indices.data(),indices.size(),_mode,_textureMode);
		renderer.disableTexture();
	};
	if (_faceCounts.empty()) drawRange(0,_indexes.size(),0);
	else {
		uint total=0;
		for (uint i=0;i<_faceCounts.size();++i) {
			if (_faceCounts[i]&&(!hasAlpha(i)||renderer.shadowMode()==TeRenderer::ShadowModeCreating||!_shouldDraw))
				drawRange(total*3,_faceCounts[i]*3,i);
			total+=_faceCounts[i];
		}
	}
	if (_drawWires&&!normals.empty()) {
		renderer.disableAllLights();renderer.setCurrentColor(TeColor(255,255,255,255));
		for (uint i=0;i<positions.size()&&i<normals.size();++i) renderer.drawLine(positions[i],positions[i]+normals[i]);
	}
	renderer.popMatrix();
}

void TeLightMetal::disable(uint lightno) {nativeRenderer().light(lightno).parameters[3]=0;}
void TeLightMetal::enable(uint lightno) {nativeRenderer().light(lightno).parameters[3]=_colDiffuse.r()||_colDiffuse.g()||_colDiffuse.b();}

void TeLightMetal::update(uint lightno) {
	TeRendererMetal &renderer=nativeRenderer();
	TeMetalLight &light=renderer.light(lightno);
	colorFloats(light.ambient,_colAmbient);colorFloats(light.diffuse,_colDiffuse);colorFloats(light.specular,_colSpecular);
	if (light.diffuse[0]<0.02f&&light.diffuse[1]<0.02f&&light.diffuse[2]<0.02f) light.parameters[3]=0;
	TeMatrix4x4 matrix=renderer.modelViewForLights();
	TeVector3f32 source=_type==LightTypeDirectional?directionVector():_position3d;
	TeVector3f32 position=_type==LightTypeDirectional?matrix.mult3x3(source):matrix.mult4x3(source);
	vectorFloats(light.position,position);light.position[3]=_type==LightTypeDirectional?0:1;
	vectorFloats(light.direction,matrix.mult3x3(directionVector()));
	light.attenuation[0]=_constAtten;light.attenuation[1]=_linearAtten;light.attenuation[2]=_quadraticAtten;
	light.parameters[0]=_type==LightTypeSpot?cosf(_cutoff):-1;
	light.parameters[1]=g_engine->gameType()==TetraedgeEngine::kSyberia?_exponent:0;
}

void TeLightMetal::draw(TeCamera &camera) {
	TeRendererMetal &renderer=nativeRenderer();
	renderer.setCurrentColor(_colDiffuse);renderer.drawLine(_position3d,_position3d+directionVector()*_displaySize);
}

Te3DTextureMetal::Te3DTextureMetal() {create();}
Te3DTextureMetal::~Te3DTextureMetal() {destroy();}
void Te3DTextureMetal::create() {
	_flipY=false;_leftBorder=_btmBorder=_rightBorder=_topBorder=_width=_height=_texWidth=_texHeight=0;
	_hasAlpha=_loaded=false;_createdTexture=true;
}
void Te3DTextureMetal::destroy() {
	if (_texture && g_engine) {
		TeRendererMetal *renderer = dynamic_cast<TeRendererMetal *>(g_engine->getRenderer());
		if (renderer) renderer->releaseTexture(_texture);
	}
	delete _texture; _texture=nullptr; _pixels.free(); _createdTexture=_loaded=false;
}
bool Te3DTextureMetal::unload() {destroy();return true;}
void Te3DTextureMetal::forceTexData(uint texture,uint width,uint height) {
	if (texture) error("Tetraedge: a legacy GL texture cannot be adopted by Metal");
	destroy();_width=_texWidth=width;_height=_texHeight=height;
}
void Te3DTextureMetal::bind() const {nativeRenderer().bindTexture(_texture,_matrix);}

void Te3DTextureMetal::updateMatrix(bool flipY) {
	_matrix.setToIdentity();
	_matrix.scale(TeVector3f32((float)_width/_texWidth,(float)_height/_texHeight,1));
	_matrix.translate(TeVector3f32((float)_leftBorder/_width,(float)_btmBorder/_height,0));
	_matrix.scale(TeVector3f32(1.0f-(float)(_rightBorder+_leftBorder)/_width,1.0f-(float)(_topBorder+_btmBorder)/_height,1));
	if (flipY) {_matrix.translate(TeVector3f32(0,1,0));_matrix.scale(TeVector3f32(1,-1,1));}
}

bool Te3DTextureMetal::load(const TeImage &image) {
	if (image.w<=0||image.h<=0) return false;
	destroy();create();
	setAccessName(image.getAccessName().append(".3dtex"));
	_width=image.w;_height=image.h;_hasAlpha=image.format.aBits()>0;_flipY=true;
	TeVector2s32 size=optimisedSize(image.bufSize());_texWidth=size._x;_texHeight=size._y;
	_pixels.create(_texWidth,_texHeight,Graphics::PixelFormat::createFormatRGBA32());
	_pixels.fillRect(Common::Rect(_texWidth,_texHeight),0);
	Graphics::Surface *rgba=image.rawSurface().convertTo(_pixels.format);
	if (_alphaOnly) {
		for (int y=0;y<rgba->h;++y) for (int x=0;x<rgba->w;++x) {
			byte a,r,g,b;uint32 &pixel=*(uint32 *)rgba->getBasePtr(x,y);rgba->format.colorToARGB(pixel,a,r,g,b);
			pixel=rgba->format.ARGBToColor(r,r,r,r);
		}
	}
	_pixels.copyRectToSurface(*rgba,0,0,Common::Rect(rgba->w,rgba->h));rgba->free();delete rgba;
	_texture=nativeRenderer().metal().createTexture(_pixels);
	if (!_texture) error("Tetraedge: could not allocate Metal texture");
	updateMatrix(true);_loaded=true;return true;
}

void Te3DTextureMetal::update(const TeImage &image,uint xoff,uint yoff) {
	if (!image.w||!image.h||!_texture) return;
	Graphics::Surface *rgba=image.rawSurface().convertTo(_pixels.format);
	if (xoff+(uint)rgba->w>_texWidth||yoff+(uint)rgba->h>_texHeight) error("Tetraedge: Metal texture update outside allocation");
	_pixels.copyRectToSurface(*rgba,xoff,yoff,Common::Rect(rgba->w,rgba->h));rgba->free();delete rgba;
	nativeRenderer().metal().updateTexture(*_texture,_pixels,Common::Rect(xoff,yoff,xoff+image.w,yoff+image.h));
}

void Te3DTextureMetal::copyCurrentRender(uint xoffset,uint yoffset,uint x,uint y) {
	if (!_texture) return;
	Metal::Renderer &renderer=nativeRenderer().metal();
	Metal::Texture *screen=renderer.screenTexture();
	Common::Rect source(x,(int)screen->height()-y-_height,x+_width,(int)screen->height()-y);
	if (!x&&!y&&!xoffset&&!yoffset) source=Common::Rect(screen->width(),screen->height());
	Common::Rect destination(xoffset,yoffset,xoffset+_width,yoffset+_height);
	renderer.copyTexture(*_texture,destination,*screen,source,true);
	updateMatrix(false);
}

void Te3DTextureMetal::writeTo(Graphics::Surface &surface) {
	if (!_texture) return;
	if (!nativeRenderer().metal().readTexture(*_texture,surface,Common::Rect(_width,_height)))
		error("Tetraedge: could not read Metal texture");
}

} // End of namespace Tetraedge

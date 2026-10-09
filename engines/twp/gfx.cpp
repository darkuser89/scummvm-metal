/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/system.h"
#include "twp/twp.h"
#include "twp/gfx.h"
#include "twp/shaders.h"

namespace Twp {

#ifdef USE_METAL
bool usingMetal();
#endif

int Color::toInt() const {
	int r = (rgba.r * 255.f);
	int g = (rgba.g * 255.f);
	int b = (rgba.b * 255.f);
	int a = (rgba.a * 255.f);
	return (r << 16) | (g << 8) | b | (a << 24);
}

Vertex::Vertex() {}

Vertex::Vertex(const Math::Vector2d &p, const Color &c, const Math::Vector2d &t)
	: pos(p), color(c), texCoords(t) {
}

Math::Matrix4 ortho(float left, float right, float bottom, float top, float zNear, float zFar) {
	Math::Matrix4 result;
	float *m = result.getData();
	m[0] = 2.f / (right - left);
	m[5] = 2.f / (top - bottom);
	m[10] = -2.f / (zFar - zNear);
	m[12] = -(right + left) / (right - left);
	m[13] = -(top + bottom) / (top - bottom);
	m[14] = -(zFar + zNear) / (zFar - zNear);
	m[15] = 1;
	return result;
}

#ifdef USE_OPENGL_SHADERS
static GLint getFormat(int channels) {
	switch (channels) {
	case 3:
		return GL_RGB;
	case 4:
		return GL_RGBA;
	default:
		error("Can't get format for %d channels", channels);
	}
}

#endif

void Texture::load(const Graphics::Surface &surface) {
#ifdef USE_METAL
	if (usingMetal()) {
		width = surface.w; height = surface.h;
		if (_metalTarget && g_system->getMetalRenderer()->renderTarget() == _metalTarget.get())
			g_system->getMetalRenderer()->setRenderTarget(nullptr);
		_metalTarget.reset();
		_metalTexture.reset(g_system->getMetalRenderer()->createTexture(surface));
		if (!_metalTexture) error("TWP: could not upload native texture");
		return;
	}
#endif
#ifdef USE_OPENGL_SHADERS
	width = surface.w;
	height = surface.h;
	const void *data = surface.getPixels();
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	// set the texture wrapping/filtering options (on the currently bound texture object)
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glPixelStorei(GL_UNPACK_ALIGNMENT, surface.format.bytesPerPixel);
	GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, getFormat(surface.format.bytesPerPixel), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data));

#else
	error("TWP: no shader renderer available");
#endif
}

void Texture::bind(const Texture *texture) {
#ifdef USE_METAL
	if (usingMetal()) return;
#endif
#ifdef USE_OPENGL_SHADERS
	if (texture && texture->id) {
		GL_CALL(glBindTexture(GL_TEXTURE_2D, texture->id));
	} else {
		GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
	}

#else
	error("TWP: no shader renderer available");
#endif
}

void Texture::capture(Common::Array<byte> &data) {
#ifdef USE_METAL
	if (usingMetal()) {
		Graphics::Surface surface;
		if (!metalTexture() || !g_system->getMetalRenderer()->readTexture(*metalTexture(), surface, Common::Rect(width, height)))
			error("TWP: native texture readback failed");
		// Preserve the engine's bottom-origin capture contract.
		if (metalTarget()) surface.flipVertical(Common::Rect(width, height));
		data.resize(width * height * 4);
		for (int y = 0; y < height; ++y) memcpy(data.data() + y * width * 4, surface.getBasePtr(0, y), width * 4);
		surface.free();
		return;
	}
#endif
#ifdef USE_OPENGL_SHADERS
	data.resize(width * height * 4);
	GLint boundFrameBuffer;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFrameBuffer);
	if (boundFrameBuffer != (int)fbo) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	}
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
	if (boundFrameBuffer != (int)fbo) {
		glBindFramebuffer(GL_FRAMEBUFFER, boundFrameBuffer);
	}

#else
	error("TWP: no shader renderer available");
#endif
}

RenderTexture::RenderTexture(const Math::Vector2d &size) {
#ifdef USE_METAL
	if (usingMetal()) {
		width = size.getX(); height = size.getY();
		_metalTarget.reset(g_system->getMetalRenderer()->createRenderTarget(width, height));
		if (!_metalTarget) error("TWP: could not allocate native render target");
		return;
	}
#endif
#ifdef USE_OPENGL_SHADERS
	width = size.getX();
	height = size.getY();

	// first create the framebuffer
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);

	// then create an empty texture
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);

	// then attach it to framebuffer object
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, id, 0);
	assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

#else
	error("TWP: no shader renderer available");
#endif
}

RenderTexture::~RenderTexture() {
#ifdef USE_METAL
	if (_metalTarget) {
		if (g_system->getMetalRenderer())
			g_system->getMetalRenderer()->setRenderTarget(nullptr);
		_metalTarget.reset();
		return;
	}
#endif
#ifdef USE_OPENGL_SHADERS
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteTextures(1, &id);
	glDeleteFramebuffers(1, &fbo);

#else
	error("TWP: no shader renderer available");
#endif
}

Shader::Shader() {
}

Shader::~Shader() {
}

void Shader::init(const char *name, const char *vertex, const char *fragment) {
#ifdef USE_METAL
	if (usingMetal()) { initMetal(name, fragment); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	const char *attributes[] = {"a_position", "a_color", "a_texCoords", nullptr};
	_shader.loadFromStrings(name, vertex, fragment, attributes, 110);

	uint32 vbo = g_twp->getGfx()._vbo;
	_shader.enableVertexAttribute("a_position", vbo, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (uint32)0);
	_shader.enableVertexAttribute("a_color", vbo, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (uint32)(2 * sizeof(float)));
	_shader.enableVertexAttribute("a_texCoords", vbo, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (uint32)(6 * sizeof(float)));

#else
	error("TWP: no shader renderer available");
#endif
}

int Shader::getUniformLocation(const char *name) const {
#ifdef USE_METAL
	if (_metalShader) return !strcmp(name, "u_texture2") ? 1 : 0;
#endif
#ifdef USE_OPENGL_SHADERS
	return _shader.getUniformLocation(name);

#else
	error("TWP: no shader renderer available");
#endif
}

void Shader::setUniform(const char *name, int value) {
#ifdef USE_METAL
	if (_metalShader) { float number = value; setMetalUniform(name, &number, 1, 1); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	_shader.setUniform(name, value);

#else
	error("TWP: no shader renderer available");
#endif
}

void Shader::setUniform(const char *name, float value) {
#ifdef USE_METAL
	if (_metalShader) { setMetalUniform(name, &value, 1, 1); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	_shader.setUniform1f(name, value);

#else
	error("TWP: no shader renderer available");
#endif
}

void Shader::setUniform(const char *name, float *value, size_t count) {
#ifdef USE_METAL
	if (_metalShader) { setMetalUniform(name, value, count, 1); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	GLint loc = _shader.getUniformLocation(name);
	GL_CALL(glUniform1fv(loc, count, value));

#else
	error("TWP: no shader renderer available");
#endif
}

void Shader::setUniform2(const char *name, float *value, size_t count) {
#ifdef USE_METAL
	if (_metalShader) { setMetalUniform(name, value, count, 2); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	GLint loc = _shader.getUniformLocation(name);
	GL_CALL(glUniform2fv(loc, count, value));

#else
	error("TWP: no shader renderer available");
#endif
}

void Shader::setUniform3(const char *name, float *value, size_t count) {
#ifdef USE_METAL
	if (_metalShader) { setMetalUniform(name, value, count, 3); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	GLint loc = _shader.getUniformLocation(name);
	GL_CALL(glUniform3fv(loc, count, value));

#else
	error("TWP: no shader renderer available");
#endif
}

void Shader::setUniform(const char *name, Math::Vector2d value) {
#ifdef USE_METAL
	if (_metalShader) { float numbers[] = {value.getX(), value.getY()}; setMetalUniform(name, numbers, 1, 2); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	_shader.setUniform(name, value);

#else
	error("TWP: no shader renderer available");
#endif
}

void Shader::setUniform3(const char *name, Color value) {
#ifdef USE_METAL
	if (_metalShader) { setMetalUniform(name, value.v, 1, 3); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	_shader.setUniform(name, Math::Vector3d(value.v));

#else
	error("TWP: no shader renderer available");
#endif
}

void Shader::setUniform4(const char *name, Color value) {
#ifdef USE_METAL
	if (_metalShader) { setMetalUniform(name, value.v, 1, 4); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	_shader.setUniform(name, Math::Vector4d(value.v));

#else
	error("TWP: no shader renderer available");
#endif
}

void Gfx::init() {
	byte pixels[] = {0xFF, 0xFF, 0xFF, 0xFF};
	Graphics::Surface empty;
	empty.w = 1;
	empty.h = 1;
	empty.format = Graphics::PixelFormat::createFormatRGBA32();
	empty.setPixels(pixels);
	_emptyTexture.load(empty);

#ifdef USE_OPENGL_SHADERS
#ifdef USE_METAL
	if (!usingMetal()) {
#endif
	GL_CALL(glGenBuffers(1, &_vbo));
	GL_CALL(glGenBuffers(1, &_ebo));
	GL_CALL(glBindBuffer(GL_ARRAY_BUFFER, _vbo));
	GL_CALL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _ebo));

#ifdef USE_METAL
	}
#endif
#endif

	const char *fragmentSrc = R"(
		varying vec4 v_color;
		varying vec2 v_texCoords;
		uniform sampler2D u_texture;
		void main() {
			vec4 tex_color = texture2D(u_texture, v_texCoords);
			gl_FragColor = v_color * tex_color;
		})";
	_defaultShader.init("default", vsrc, fragmentSrc);
	_shader = &_defaultShader;
	_mvp = ortho(-1.f, 1.f, -1.f, 1.f, -1.f, 1.f);

#ifdef USE_OPENGL_SHADERS
#ifdef USE_METAL
	if (!usingMetal()) {
#endif
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &_oldFbo);
#ifdef USE_METAL
	}
#endif
#endif
}

void Gfx::clear(const Color &color) {
#ifdef USE_METAL
	if (usingMetal()) { g_system->getMetalRenderer()->clear(color.rgba.r, color.rgba.g, color.rgba.b, color.rgba.a, true, false, false); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	glClearColor(color.rgba.r, color.rgba.g, color.rgba.b, color.rgba.a);
	glClear(GL_COLOR_BUFFER_BIT);

#else
	error("TWP: no shader renderer available");
#endif
}

Math::Matrix4 Gfx::getFinalTransform(const Math::Matrix4 &trsf) {
	Math::Matrix4 t(trsf);
	t.transpose();
	return t * _mvp;
}

void Gfx::noTexture() {
#ifdef USE_METAL
	if (usingMetal()) { _texture = &_emptyTexture; return; }
#endif
#ifdef USE_OPENGL_SHADERS
	_texture = &_emptyTexture;
	GL_CALL(glBindTexture(GL_TEXTURE_2D, _emptyTexture.id));

#else
	error("TWP: no shader renderer available");
#endif
}

void Gfx::drawLines(Vertex *vertices, int count, const Math::Matrix4 &trsf) {
	noTexture();
	drawPrimitives(kPrimitiveLineStrip, vertices, count, trsf);
}

void Gfx::drawLinesLoop(Vertex *vertices, int count, const Math::Matrix4 &trsf) {
	noTexture();
	drawPrimitives(kPrimitiveLineLoop, vertices, count, trsf);
}

void Gfx::drawPrimitives(uint32 primitivesType, Vertex *vertices, int v_size, const Math::Matrix4 &trsf, Texture *texture) {
#ifdef USE_METAL
	if (usingMetal()) { drawMetal(primitivesType, vertices, v_size, nullptr, 0, trsf, texture); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	if (v_size > 0) {
		_texture = texture ? texture : &_emptyTexture;
		GL_CALL(glBindTexture(GL_TEXTURE_2D, _texture->id));

		// set blending
		GL_CALL(glEnable(GL_BLEND));
		GL_CALL(glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD));
		GL_CALL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

		GL_CALL(glBindBuffer(GL_ARRAY_BUFFER, _vbo));

		GL_CALL(glBindBuffer(GL_ARRAY_BUFFER, _vbo));
		GL_CALL(glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * v_size, vertices, GL_STREAM_DRAW));

		GL_CALL(glActiveTexture(GL_TEXTURE0));
		GL_CALL(glBindTexture(GL_TEXTURE_2D, _texture->id));

		Math::Matrix4 m = getFinalTransform(trsf);
		_shader->_shader.setUniform("u_transform", m);
		GL_CALL(glDrawArrays((GLenum)primitivesType, 0, v_size));
		_shader->_shader.unbind();

		glBindBuffer(GL_ARRAY_BUFFER, 0);

		GL_CALL(glDisableVertexAttribArray(0));
		GL_CALL(glDisableVertexAttribArray(1));
		GL_CALL(glDisableVertexAttribArray(2));

		glDisable(GL_BLEND);
	}

#else
	error("TWP: no shader renderer available");
#endif
}

void Gfx::drawPrimitives(uint32 primitivesType, Vertex *vertices, int v_size, uint32 *indices, int i_size, const Math::Matrix4 &trsf, Texture *texture) {
#ifdef USE_METAL
	if (usingMetal()) { drawMetal(primitivesType, vertices, v_size, indices, i_size, trsf, texture); return; }
#endif
#ifdef USE_OPENGL_SHADERS
	if (i_size > 0) {
		int num = _shader->getNumTextures();
		if (num == 0) {
			_texture = texture ? texture : &_emptyTexture;
			GL_CALL(glBindTexture(GL_TEXTURE_2D, _texture->id));
		} else {
			for (int i = 0; i < num; i++) {
				GL_CALL(glBindTexture(GL_TEXTURE_2D, _shader->getTexture(i)));
			}
		}

		// set blending
		GL_CALL(glEnable(GL_BLEND));
		GL_CALL(glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD));
		GL_CALL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA));

		_shader->_shader.use();

		GL_CALL(glBindBuffer(GL_ARRAY_BUFFER, _vbo));
		GL_CALL(glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * v_size, vertices, GL_STREAM_DRAW));
		GL_CALL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _ebo));
		GL_CALL(glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(uint32) * i_size, indices, GL_STREAM_DRAW));

		if (num == 0) {
			GL_CALL(glActiveTexture(GL_TEXTURE0));
			GL_CALL(glBindTexture(GL_TEXTURE_2D, _texture->id));
			GL_CALL(glUniform1i(_shader->getUniformLocation("u_texture"), 0));
		} else {
			for (int i = 0; i < num; i++) {
				GL_CALL(glActiveTexture(GL_TEXTURE0 + i));
				GL_CALL(glBindTexture(GL_TEXTURE_2D, _shader->getTexture(i)));
				GL_CALL(glUniform1i(_shader->getTextureLoc(i), i));
			}
		}

		_shader->_shader.setUniform("u_transform", getFinalTransform(trsf));
		_shader->applyUniforms();
		GL_CALL(glDrawElements(primitivesType, i_size, GL_UNSIGNED_INT, NULL));
		_shader->_shader.unbind();

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glDisable(GL_BLEND);
	}

#else
	error("TWP: no shader renderer available");
#endif
}

void Gfx::draw(Vertex *vertices, int v_size, uint32 *indices, int i_size, const Math::Matrix4 &trsf, Texture *texture) {
	drawPrimitives(kPrimitiveTriangles, vertices, v_size, indices, i_size, trsf, texture);
}

void Gfx::drawQuad(const Math::Vector2d &size, const Color &color, const Math::Matrix4 &trsf) {
	float w = size.getX();
	float h = size.getY();
	float x = 0;
	float y = 0;
	Vertex vertices[] = {
		Vertex{{x + w, y + h}, color, {1, 0}},
		Vertex{{x + w, y}, color, {1, 1}},
		Vertex{{x, y}, color, {0, 1}},
		Vertex{{x, y + h}, color, {0, 0}}};
	noTexture();
	uint32 quadIndices[] = {
		0, 1, 3,
		1, 2, 3};
	draw(vertices, 4, quadIndices, 6, trsf);
}

void Gfx::drawSprite(const Common::Rect &textRect, Texture &texture, const Color &color, const Math::Matrix4 &trsf, bool flipX, bool flipY) {
	float l = textRect.left / (float)texture.width;
	float r = textRect.right / (float)texture.width;
	float t = textRect.top / (float)texture.height;
	float b = textRect.bottom / (float)texture.height;
	if (flipX)
		SWAP(l, r);
	if (flipY)
		SWAP(t, b);

	Math::Vector2d pos;
	Vertex vertices[] = {
		{{pos.getX() + textRect.width(), pos.getY() + textRect.height()}, color, {r, t}},
		{{pos.getX() + textRect.width(), pos.getY()}, color, {r, b}},
		{{pos.getX(), pos.getY()}, color, {l, b}},
		{{pos.getX(), pos.getY() + textRect.height()}, color, {l, t}}};
	uint32 quadIndices[] = {
		0, 1, 3,
		1, 2, 3};
	draw(vertices, 4, quadIndices, 6, trsf, &texture);
}

void Gfx::drawSprite(Texture &texture, const Color &color, const Math::Matrix4 &trsf, bool flipX, bool flipY) {
	drawSprite(Common::Rect(texture.width, texture.height), texture, color, trsf, flipX, flipY);
}

void Gfx::camera(const Math::Vector2d &size) {
	_cameraSize = size;
	_mvp = ortho(0.f, size.getX(), 0.f, size.getY(), -1.f, 1.f);
}

Math::Vector2d Gfx::camera() const {
	return _cameraSize;
}

void Gfx::use(Shader *shader) {
	_shader = shader ? shader : &_defaultShader;
}

void Gfx::setRenderTarget(RenderTexture *target) {
#ifdef USE_METAL
	if (usingMetal()) {
		Graphics::Metal::Renderer &metal = *g_system->getMetalRenderer();
		metal.setRenderTarget(target ? target->metalTarget() : nullptr);
		metal.setViewport(Common::Rect(metal.width(), metal.height()));
		return;
	}
#endif
#ifdef USE_OPENGL_SHADERS
	if (!target) {
		glBindFramebuffer(GL_FRAMEBUFFER, _oldFbo);
		int w = g_twp->_system->getWidth();
		int h = g_twp->_system->getHeight();
		glViewport(0, 0, w, h);
	} else {
		glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
		glViewport(0, 0, target->width, target->height);
	}

#else
	error("TWP: no shader renderer available");
#endif
}

} // namespace Twp

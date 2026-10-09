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

#ifndef TWP_GFX_H
#define TWP_GFX_H

#include "common/array.h"
#include "common/hashmap.h"
#include "common/hash-str.h"
#include "common/rect.h"
#include "graphics/surface.h"
#include "common/ptr.h"
#ifdef USE_OPENGL_SHADERS
#include "graphics/opengl/shader.h"
#endif
#ifdef USE_METAL
#include "graphics/metal/renderer.h"
#endif
#include "math/vector2d.h"
#include "math/matrix4.h"

namespace Twp {

enum PrimitiveType {
	kPrimitiveLines = 0x0001,
	kPrimitiveLineLoop = 0x0002,
	kPrimitiveLineStrip = 0x0003,
	kPrimitiveTriangles = 0x0004,
	kPrimitiveTriangleStrip = 0x0005,
	kPrimitiveTriangleFan = 0x0006
};

#ifdef USE_METAL
struct MetalShaderState;
#endif

struct Color {
	union {
		float v[4];
		struct {
			float r;
			float g;
			float b;
			float a;
		} rgba;
	};

	Color(float red = 1.0f, float green = 1.0f, float blue = 1.0f, float alpha = 1.0f) {
		rgba.r = red;
		rgba.g = green;
		rgba.b = blue;
		rgba.a = alpha;
	}

	static Color withAlpha(const Color &c, float alpha = 1.0f) {
		Color result = c;
		result.rgba.a = alpha;
		return result;
	}

	static Color rgb(int c) {
		return create((uint8)((c >> 16) & 0xFF), (uint8)((c >> 8) & 0xFF), (uint8)(c & 0xFF), 0xFF);
	}

	static Color fromRgba(int c) {
		return create((uint8)((c >> 16) & 0xFF), (uint8)((c >> 8) & 0xFF), (uint8)(c & 0xFF), (uint8)((c >> 24) & 0xFF));
	}

	static Color create(uint8 red, uint8 green, uint8 blue, uint8 alpha = 0xFF) {
		return Color(red / 255.f, green / 255.f, blue / 255.f, alpha / 255.f);
	}

	Common::String toStr() {
		return Common::String::format("rgba(%f,%f,%f,%f)", rgba.r, rgba.g, rgba.b, rgba.a);
	}

	Color operator-(const Color &c) {
		return Color(rgba.r - c.rgba.r, rgba.g - c.rgba.g, rgba.b - c.rgba.b, rgba.a - c.rgba.a);
	}

	Color operator+(const Color &c) {
		return Color(rgba.r + c.rgba.r, rgba.g + c.rgba.g, rgba.b + c.rgba.b, rgba.a + c.rgba.a);
	}

	Color operator*(float f) {
		return Color(rgba.r * f, rgba.g * f, rgba.b * f, rgba.a * f);
	}

	int toInt() const;
};

// This is a point in 2D with a color and texture coordinates
struct Vertex {
public:
	Math::Vector2d pos;
	Color color;
	Math::Vector2d texCoords;

	Vertex();
	Vertex(const Math::Vector2d &p, const Color &c = Color(), const Math::Vector2d &t = Math::Vector2d());
};

class Texture {
public:
	Texture() {}
	virtual ~Texture() {}

	void load(const Graphics::Surface &surface);
	static void bind(const Texture *texture);
	void capture(Common::Array<byte> &data);
#ifdef USE_METAL
	Graphics::Metal::Texture *metalTexture() const { return _metalTarget ? &_metalTarget->texture() : _metalTexture.get(); }
	Graphics::Metal::RenderTarget *metalTarget() const { return _metalTarget.get(); }
#endif

public:
	uint id = 0;
	int width = 0, height = 0;
	uint fbo = 0;
#ifdef USE_METAL
	Common::SharedPtr<Graphics::Metal::Texture> _metalTexture;
	Common::SharedPtr<Graphics::Metal::RenderTarget> _metalTarget;
#endif
};

class RenderTexture : public Texture {
public:
	explicit RenderTexture(const Math::Vector2d &size);
	~RenderTexture() override;
};

struct TextureSlot {
	int id;
	Texture texture;
};

class Shader {
public:
	friend class Gfx;

public:
	Shader();
	virtual ~Shader();

	void init(const char *name, const char *vertex, const char *fragment);

	int getUniformLocation(const char *name) const;

	void setUniform(const char *name, int value);
	void setUniform(const char *name, float value);
	void setUniform(const char *name, float *value, size_t count);
	void setUniform2(const char *name, float *value, size_t count);
	void setUniform3(const char *name, float *value, size_t count);

	void setUniform(const char *name, Math::Vector2d value);
	void setUniform3(const char *name, Color value);
	void setUniform4(const char *name, Color value);

	virtual void applyUniforms() {}
	virtual int getNumTextures() { return 0; };
	virtual int getTexture(int index) { return 0; };
	virtual int getTextureLoc(int index) { return 0; };
	virtual Texture *getTextureObject(int index) { return nullptr; }

private:
	Common::HashMap<int, TextureSlot> _textures;
#ifdef USE_OPENGL_SHADERS
	OpenGL::Shader _shader;
#endif
#ifdef USE_METAL
	Common::SharedPtr<MetalShaderState> _metalShader;
	void initMetal(const char *name, const char *fragment);
	void setMetalUniform(const char *name, const float *values, size_t count, uint components);
#endif
};

typedef Common::HashMap<int, TextureSlot> Textures;

class Gfx {
public:
	friend class Shader;

public:
	void init();

	void camera(const Math::Vector2d &size);
	Math::Vector2d camera() const;
	Math::Vector2d cameraPos() const { return _cameraPos; }
	void cameraPos(const Math::Vector2d &pos) { _cameraPos = pos; }

	Shader *getShader() { return _shader; }
	void use(Shader *shader);
	void setRenderTarget(RenderTexture *target);

	void clear(const Color &color);
	void drawPrimitives(uint32 primitivesType, Vertex *vertices, int v_size, const Math::Matrix4 &transf = Math::Matrix4(), Texture *texture = NULL);
	void drawPrimitives(uint32 primitivesType, Vertex *vertices, int v_size, uint32 *indices, int i_size, const Math::Matrix4 &transf = Math::Matrix4(), Texture *texture = NULL);
	void drawLines(Vertex *vertices, int count, const Math::Matrix4 &trsf = Math::Matrix4());
	void drawLinesLoop(Vertex *vertices, int count, const Math::Matrix4 &trsf = Math::Matrix4());
	void draw(Vertex *vertices, int v_size, uint32 *indices, int i_size, const Math::Matrix4 &trsf = Math::Matrix4(), Texture *texture = NULL);
	void drawQuad(const Math::Vector2d &size, const Color &color = Color(), const Math::Matrix4 &trsf = Math::Matrix4());
	void drawSprite(const Common::Rect &textRect, Texture &texture, const Color &color = Color(), const Math::Matrix4 &trsf = Math::Matrix4(), bool flipX = false, bool flipY = false);
	void drawSprite(Texture &texture, const Color &color = Color(), const Math::Matrix4 &trsf = Math::Matrix4(), bool flipX = false, bool flipY = false);

private:
	Math::Matrix4 getFinalTransform(const Math::Matrix4 &trsf);
	void noTexture();
#ifdef USE_METAL
	void drawMetal(uint32 primitive, Vertex *vertices, int count, uint32 *indices, int indexCount,
	               const Math::Matrix4 &transform, Texture *texture);
#endif

private:
	Texture _emptyTexture;
	uint _vbo = 0, _ebo = 0;
	Shader _defaultShader;
	Shader *_shader = nullptr;
	Math::Matrix4 _mvp;
	Math::Vector2d _cameraPos;
	Math::Vector2d _cameraSize;
	Textures _textures;
	Texture *_texture = nullptr;
	int _oldFbo = 0;
};
} // namespace Twp

#endif

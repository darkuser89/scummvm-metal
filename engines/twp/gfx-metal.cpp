/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "twp/gfx.h"
#include "twp/metal-shaders.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "graphics/metal/shader-source.h"

namespace Twp {

namespace Metal = Graphics::Metal;

bool usingMetal() {
	return g_system->getMetalRenderer() != nullptr;
}

struct MetalLight {
	float position[4], color[4], cone[4], falloff[4];
};

struct MetalDrawUniforms {
	float transform[16];
	int32 settings[4];
	float effects[4], ghost[4], randomValue[8];
	float shadows[4], midtones[4], highlights[4], ambient[4], sprite[4], sheet[4];
	float hudShadow[4], hudNormal[4], hudHighlight[4], hudRanges[4];
};

struct MetalUniforms {
	MetalDrawUniforms draw;
	MetalLight lights[50];
};

struct MetalShaderState {
	Metal::Program *program = nullptr;
	MetalUniforms uniforms = {};
	Metal::Buffer *lights = nullptr;
	bool lightsDirty = true;
	~MetalShaderState() { delete program; delete lights; }
};

static_assert(sizeof(MetalDrawUniforms) == 304, "TWP draw uniform layout");
static_assert(sizeof(MetalUniforms) == 3504, "TWP Metal uniform layout");

void Shader::initMetal(const char *name, const char *fragment) {
	Common::SharedPtr<MetalShaderState> shader(new MetalShaderState());
	int type = 0;
	if (!strcmp(name, "black&white")) type = 1;
	else if (!strcmp(name, "ghost")) type = 2;
	else if (!strcmp(name, "sepia")) type = 3;
	else if (!strcmp(name, "fadeShader")) type = 4;
	else if (!strcmp(name, "lighting")) type = strstr(fragment, "finalCol.rgb + diffuse") ? 6 : 5;
	else if (!strcmp(name, "hud")) type = 7;
	else if (strcmp(name, "default")) error("TWP: no native Metal shader for %s", name);
	shader->uniforms.draw.settings[0] = type;
	shader->uniforms.draw.effects[3] = 1;
	shader->uniforms.draw.ghost[1] = shader->uniforms.draw.ghost[2] = 1;
	Common::String replacement = Metal::shaderOverride("twp", name);
	shader->program = g_system->getMetalRenderer()->createProgram(replacement.empty() ? kTwpMetalShaderSource : replacement.c_str(), "twpVertex", "twpFragment");
	if (!shader->program) error("TWP: could not create Metal shader %s", name);
	_metalShader = shader;
}

void Shader::setMetalUniform(const char *name, const float *values, size_t count, uint components) {
	if (!_metalShader || !count || !values) return;
	MetalDrawUniforms &u = _metalShader->uniforms.draw;
	float *destination = nullptr;
	uint capacity = 1;
	if (!strcmp(name, "u_numberLights")) { u.settings[1] = (int32)values[0]; return; }
	if (!strcmp(name, "u_fadeToSep")) { u.settings[2] = (int32)values[0]; return; }
	if (!strcmp(name, "u_texture") || !strcmp(name, "u_texture2")) return;
	if (!strcmp(name, "u_timer")) destination = &u.effects[0];
	else if (!strcmp(name, "u_fade")) destination = &u.effects[1];
	else if (!strcmp(name, "u_movement")) destination = &u.effects[2];
	else if (!strcmp(name, "sepiaFlicker")) destination = &u.effects[3];
	else if (!strcmp(name, "iGlobalTime")) destination = &u.ghost[0];
	else if (!strcmp(name, "iFade")) destination = &u.ghost[1];
	else if (!strcmp(name, "wobbleIntensity")) destination = &u.ghost[2];
	else if (!strcmp(name, "TimeLapse")) destination = &u.ghost[3];
	else if (!strcmp(name, "RandomValue")) { destination = u.randomValue; capacity = 5; }
	else if (!strcmp(name, "shadows")) { destination = u.shadows; capacity = 3; }
	else if (!strcmp(name, "midtones")) { destination = u.midtones; capacity = 3; }
	else if (!strcmp(name, "highlights")) { destination = u.highlights; capacity = 3; }
	else if (!strcmp(name, "u_ambientColor")) { destination = u.ambient; capacity = 3; }
	else if (!strcmp(name, "u_contentSize")) { destination = &u.sprite[0]; capacity = 2; }
	else if (!strcmp(name, "u_spriteOffset")) { destination = &u.sprite[2]; capacity = 2; }
	else if (!strcmp(name, "u_spritePosInSheet")) { destination = &u.sheet[0]; capacity = 2; }
	else if (!strcmp(name, "u_spriteSizeRelToSheet")) { destination = &u.sheet[2]; capacity = 2; }
	else if (!strcmp(name, "u_ranges")) { destination = u.hudRanges; capacity = 2; }
	else if (!strcmp(name, "u_shadowColor")) { destination = u.hudShadow; capacity = 4; }
	else if (!strcmp(name, "u_normalColor")) { destination = u.hudNormal; capacity = 4; }
	else if (!strcmp(name, "u_highlightColor")) { destination = u.hudHighlight; capacity = 4; }
	if (destination) {
		if (count * components > capacity) error("TWP: Metal uniform %s exceeds its allocation", name);
		memcpy(destination, values, count * components * sizeof(float));
		return;
	}
	if (count > 50) error("TWP: Metal light array exceeds 50 lights");
	for (uint i = 0; i < count; ++i) {
		MetalLight &light = _metalShader->uniforms.lights[i];
		float *member = nullptr;
		if (!strcmp(name, "u_lightPos") && components == 3) member = light.position;
		else if (!strcmp(name, "u_lightColor") && components == 3) member = light.color;
		else if (!strcmp(name, "u_coneDirection") && components == 2) member = light.cone;
		else if (!strcmp(name, "u_coneCosineHalfConeAngle") && components == 1) member = &light.cone[2];
		else if (!strcmp(name, "u_coneFalloff") && components == 1) member = &light.cone[3];
		else if (!strcmp(name, "u_brightness") && components == 1) member = &light.falloff[0];
		else if (!strcmp(name, "u_cutoffRadius") && components == 1) member = &light.falloff[1];
		else if (!strcmp(name, "u_halfRadius") && components == 1) member = &light.falloff[2];
		if (!member) error("TWP: unknown native Metal uniform %s", name);
		_metalShader->lightsDirty |= memcmp(member, values + i * components, components * sizeof(float)) != 0;
		memcpy(member, values + i * components, components * sizeof(float));
	}
}

void Gfx::drawMetal(uint32 primitive, Vertex *vertices, int count, uint32 *indices, int indexCount,
		const Math::Matrix4 &transform, Texture *texture) {
	if (!vertices || count <= 0 || (indices && indexCount <= 0)) return;
	if (!_shader || !_shader->_metalShader) error("TWP: native shader was not initialized");
	_shader->applyUniforms();
	MetalShaderState &shader = *_shader->_metalShader;
	MetalDrawUniforms uniforms = shader.uniforms.draw;
	Metal::Renderer &renderer = *g_system->getMetalRenderer();
	if (shader.lightsDirty || !shader.lights) {
		if (shader.lights) renderer.updateBuffer(*shader.lights, shader.uniforms.lights, sizeof(shader.uniforms.lights));
		else shader.lights = renderer.createBuffer(shader.uniforms.lights, sizeof(shader.uniforms.lights));
		if (!shader.lights) error("TWP: could not allocate persistent lights");
		shader.lightsDirty = false;
	}
	Metal::UniformBindings constants; constants.extraFragment = shader.lights;
	Math::Matrix4 matrix = getFinalTransform(transform);
	memcpy(uniforms.transform, matrix.getData(), sizeof(uniforms.transform));
	struct PackedVertex { float position[2], color[4], uv[2]; };
	static_assert(sizeof(PackedVertex) == 32, "TWP native vertex layout");
	Common::Array<PackedVertex> packed;
	packed.resize(count);
	for (int i = 0; i < count; ++i) {
		packed[i].position[0] = vertices[i].pos.getX(); packed[i].position[1] = vertices[i].pos.getY();
		memcpy(packed[i].color, vertices[i].color.v, sizeof(packed[i].color));
		packed[i].uv[0] = vertices[i].texCoords.getX(); packed[i].uv[1] = vertices[i].texCoords.getY();
	}
	int elements = indices ? indexCount : count;
	if (!indices) indexCount = 0;
	Common::Array<uint32> converted;
	Metal::DrawState state;
	state.blend = true; state.linear = false;
	switch (primitive) {
	case kPrimitiveLines: state.primitive = Metal::kLines; break;
	case kPrimitiveLineStrip: state.primitive = Metal::kLineStrip; break;
	case kPrimitiveLineLoop:
		for (int i = 0; i < elements; ++i) converted.push_back(indices ? indices[i] : i);
		converted.push_back(indices ? indices[0] : 0); state.primitive = Metal::kLineStrip; break;
	case kPrimitiveTriangleStrip: state.primitive = Metal::kTriangleStrip; break;
	case kPrimitiveTriangleFan:
		if (elements < 3) return;
		for (int i = 1; i + 1 < elements; ++i) {
			converted.push_back(indices ? indices[0] : 0); converted.push_back(indices ? indices[i] : i); converted.push_back(indices ? indices[i + 1] : i + 1);
		}
		break;
	case kPrimitiveTriangles: break;
	default: error("TWP: unsupported native Metal primitive %u", primitive);
	}
	if (!converted.empty()) { indices = converted.data(); indexCount = converted.size(); }
	Metal::TextureBinding bindings[2];
	uniforms.settings[3] = 0;
	int numTextures = _shader->getNumTextures();
	if (numTextures > 2) error("TWP: native shader has too many textures");
	for (int i = 0; i < 2; ++i) {
		Texture *source = numTextures ? _shader->getTextureObject(i) : texture;
		if (!source) source = &_emptyTexture;
		bindings[i].texture = source->metalTexture();
		bindings[i].linear = false;
		if (source->metalTarget()) uniforms.settings[3] |= 1 << i;
	}
	g_system->getMetalRenderer()->drawProgram(*shader.program, packed.data(), count, sizeof(PackedVertex),
		indices, indexCount, uniforms.transform, sizeof(uniforms.transform), bindings, 2, state,
		uniforms.settings, sizeof(uniforms) - sizeof(uniforms.transform), nullptr, &constants);
}

} // End of namespace Twp

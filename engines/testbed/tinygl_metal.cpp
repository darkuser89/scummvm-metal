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

#include "common/file.h"
#include "engines/util.h"
#include "image/png.h"
#include "graphics/managed_surface.h"
#include "graphics/metal/renderer.h"
#include "common/ptr.h"
#include "graphics/tinygl/tinygl.h"

#include "testbed/tinygl.h"

namespace Testbed {

namespace TinyGLTests {
	struct TextureEnvironmentArg {
		uint32 _sourceRGB, _operandRGB;
		uint32 _sourceAlpha, _operandAlpha;

		TextureEnvironmentArg(
			uint32 sourceRGB = TGL_TEXTURE,
			uint32 operandRGB = TGL_SRC_COLOR,
			uint32 sourceAlpha = TGL_TEXTURE,
			uint32 operandAlpha = TGL_SRC_ALPHA)
			: _sourceRGB(sourceRGB)
			, _operandRGB(operandRGB)
			, _sourceAlpha(sourceAlpha)
			, _operandAlpha(operandAlpha) { }
	};

	struct TextureEnvironment {
		uint32 _mode, _combineRGB, _combineAlpha;
		TextureEnvironmentArg _arg0, _arg1;
		byte _constantR = 255, _constantG = 255, _constantB = 255, _constantA = 255;

		TextureEnvironment(
			uint32 mode = TGL_REPLACE,
			uint32 combineRGB = TGL_REPLACE,
			uint32 combineAlpha = TGL_REPLACE)
			: _mode(mode)
			, _combineRGB(combineRGB)
			, _combineAlpha(combineAlpha) { }

		template<typename TexEnvFunci, typename TexEnvFuncfv>
		void apply(TexEnvFunci funci, TexEnvFuncfv funcfv) const {
			funci(TGL_TEXTURE_ENV, TGL_TEXTURE_ENV_MODE, _mode);
			funci(TGL_TEXTURE_ENV, TGL_COMBINE_RGB, _combineRGB);
			funci(TGL_TEXTURE_ENV, TGL_COMBINE_ALPHA, _combineAlpha);

			funci(TGL_TEXTURE_ENV, TGL_SOURCE0_RGB, _arg0._sourceRGB);
			funci(TGL_TEXTURE_ENV, TGL_SOURCE0_ALPHA, _arg0._sourceAlpha);
			funci(TGL_TEXTURE_ENV, TGL_SOURCE1_RGB, _arg1._sourceRGB);
			funci(TGL_TEXTURE_ENV, TGL_SOURCE1_ALPHA, _arg1._sourceAlpha);

			funci(TGL_TEXTURE_ENV, TGL_OPERAND0_RGB, _arg0._operandRGB);
			funci(TGL_TEXTURE_ENV, TGL_OPERAND0_ALPHA, _arg0._operandAlpha);
			funci(TGL_TEXTURE_ENV, TGL_OPERAND1_RGB, _arg1._operandRGB);
			funci(TGL_TEXTURE_ENV, TGL_OPERAND1_ALPHA, _arg1._operandAlpha);

			const float values[] = {
				_constantR / 255.0f,
				_constantG / 255.0f,
				_constantB / 255.0f,
				_constantA / 255.0f
			};
			funcfv(TGL_TEXTURE_ENV, TGL_TEXTURE_ENV_COLOR, values);
		}
	};

	TestExitStatus runTexEnvTest(
		const char *testName,
		const TextureEnvironment &env,
		byte colorR, byte colorG, byte colorB, byte colorA);
	TestExitStatus testTexEnvReplace();
	TestExitStatus testTexEnvModulate();
	TestExitStatus testTexEnvDecal();
	TestExitStatus testTexEnvAdd();
	TestExitStatus testTexEnvBlend();
	TestExitStatus testTexEnvCombineOpNormal();
	TestExitStatus testTexEnvCombineOpInverse();
	TestExitStatus testTexEnvCombineOpAlphaToColor();
	TestExitStatus testTexEnvCombineMixedArgs();
	TestExitStatus testTexEnvCombineReplace();
	TestExitStatus testTexEnvCombineModulate();
	TestExitStatus testTexEnvCombineAdd();
}

TinyGLTestSuite::TinyGLTestSuite() {
	addTest("Replace", &TinyGLTests::testTexEnvReplace);
	addTest("Modulate", &TinyGLTests::testTexEnvModulate);
	addTest("Decal", &TinyGLTests::testTexEnvDecal);
	addTest("Add", &TinyGLTests::testTexEnvAdd);
	addTest("Blend", &TinyGLTests::testTexEnvBlend);
	addTest("CombineOpNormal", &TinyGLTests::testTexEnvCombineOpNormal);
	addTest("CombineOpInverse", &TinyGLTests::testTexEnvCombineOpInverse);
	addTest("CombineOpAlphaToColor", &TinyGLTests::testTexEnvCombineOpAlphaToColor);
	addTest("CombineMixedArgs", &TinyGLTests::testTexEnvCombineMixedArgs);
	addTest("CombineReplace", &TinyGLTests::testTexEnvCombineReplace);
	addTest("CombineModulate", &TinyGLTests::testTexEnvCombineModulate);
	addTest("CombineAdd", TinyGLTests::testTexEnvCombineAdd);
}

TestExitStatus TinyGLTests::testTexEnvModulate() {
	TextureEnvironment env(TGL_MODULATE);
	return runTexEnvTest("Modulate", env, 50, 111, 222, 255);
}

TestExitStatus TinyGLTests::testTexEnvReplace() {
	TextureEnvironment env(TGL_REPLACE);
	return runTexEnvTest("Replace", env, 255, 0, 255, 255);
}

TestExitStatus TinyGLTests::testTexEnvDecal() {
	TextureEnvironment env(TGL_DECAL);
	return runTexEnvTest("Decal", env, 50, 111, 222, 255);
}

TestExitStatus TinyGLTests::testTexEnvAdd() {
	TextureEnvironment env(TGL_ADD);
	return runTexEnvTest("Add", env, 50, 111, 222, 255);
}

TestExitStatus TinyGLTests::testTexEnvBlend() {
	TextureEnvironment env(TGL_BLEND);
	env._constantR = 250;
	env._constantG = 150;
	env._constantB = 100;
	env._constantA = 50;
	return runTexEnvTest("Blend", env, 50, 111, 222, 255);
}

TestExitStatus TinyGLTests::testTexEnvCombineOpNormal() {
	TextureEnvironment env(TGL_COMBINE, TGL_REPLACE, TGL_REPLACE);
	env._arg0 = { TGL_TEXTURE, TGL_SRC_COLOR, TGL_TEXTURE, TGL_SRC_ALPHA };
	return runTexEnvTest("CombineOpNormal", env, 50, 111, 222, 255);
}

TestExitStatus TinyGLTests::testTexEnvCombineOpInverse() {
	TextureEnvironment env(TGL_COMBINE, TGL_REPLACE, TGL_REPLACE);
	env._arg0 = { TGL_TEXTURE, TGL_ONE_MINUS_SRC_COLOR, TGL_TEXTURE, TGL_ONE_MINUS_SRC_ALPHA };
	return runTexEnvTest("CombineOpInverse", env, 50, 111, 222, 255);
}

TestExitStatus TinyGLTests::testTexEnvCombineOpAlphaToColor() {
	TextureEnvironment env(TGL_COMBINE, TGL_REPLACE, TGL_REPLACE);
	env._arg0 = { TGL_TEXTURE, TGL_SRC_ALPHA, TGL_TEXTURE, TGL_ONE_MINUS_SRC_ALPHA };
	return runTexEnvTest("CombineOpAlphaToColor", env, 50, 111, 222, 255);
}

TestExitStatus TinyGLTests::testTexEnvCombineMixedArgs() {
	TextureEnvironment env(TGL_COMBINE, TGL_REPLACE, TGL_REPLACE);
	env._arg0 = { TGL_PRIMARY_COLOR, TGL_SRC_ALPHA, TGL_TEXTURE, TGL_SRC_ALPHA };
	return runTexEnvTest("CombineMixedArgs", env, 255, 0, 255, 50);
}

TestExitStatus TinyGLTests::testTexEnvCombineReplace() {
	TextureEnvironment env(TGL_COMBINE, TGL_REPLACE, TGL_REPLACE);
	env._arg0 = { TGL_TEXTURE, TGL_SRC_COLOR, TGL_PRIMARY_COLOR, TGL_SRC_ALPHA };
	return runTexEnvTest("CombineReplace", env, 50, 11, 222, 127);
}

TestExitStatus TinyGLTests::testTexEnvCombineModulate() {
	TextureEnvironment env(TGL_COMBINE, TGL_MODULATE, TGL_MODULATE);
	env._arg0 = { TGL_TEXTURE, TGL_SRC_COLOR, TGL_TEXTURE, TGL_SRC_ALPHA };
	env._arg1 = { TGL_PRIMARY_COLOR, TGL_SRC_COLOR, TGL_PRIMARY_COLOR, TGL_SRC_ALPHA };
	return runTexEnvTest("CombineModulate", env, 50, 11, 222, 127);
}

TestExitStatus TinyGLTests::testTexEnvCombineAdd() {
	TextureEnvironment env(TGL_COMBINE, TGL_ADD, TGL_ADD);
	env._arg0 = { TGL_TEXTURE, TGL_SRC_COLOR, TGL_TEXTURE, TGL_SRC_ALPHA };
	env._arg1 = { TGL_PRIMARY_COLOR, TGL_SRC_COLOR, TGL_PRIMARY_COLOR, TGL_SRC_ALPHA };
	return runTexEnvTest("CombineAdd", env, 50, 11, 222, 127);
}

struct TinyGLContextDeleter {
	inline void operator()(TinyGL::ContextHandle *handle) {
		TinyGL::destroyContext(handle);
	}
};


static constexpr int kScreenWidth = 800, kScreenHeight = 600, kTextureSize = 256;

struct EnvironmentUniforms {
	float primary[4], constantColor[4];
	uint32 mode, rgbMode, alphaMode, padding;
	uint32 sourceRGB[2], operandRGB[2], sourceAlpha[2], operandAlpha[2];
};

static const char *const kEnvironmentShader = R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex { packed_float3 position; packed_float2 uv; packed_float4 color; };
struct Uniforms {
 float4 primary, constantColor;
 uint mode, rgbMode, alphaMode, padding;
 uint sourceRGB[2], operandRGB[2], sourceAlpha[2], operandAlpha[2];
};
struct Varying { float4 position [[position]]; float2 uv; };
vertex Varying environmentVertex(uint id [[vertex_id]], const device Vertex *v [[buffer(0)]]) {
 Varying o; o.position=float4(float3(v[id].position),1); o.uv=float2(v[id].uv); return o;
}
float4 source(uint id, float4 texel, constant Uniforms &u) {
 return id == 0 ? texel : id == 1 ? u.constantColor : u.primary;
}
float3 operand(float4 value, uint id) {
 return id == 0 ? value.rgb : id == 1 ? 1-value.rgb : id == 2 ? float3(value.a) : float3(1-value.a);
}
fragment float4 environmentFragment(Varying in [[stage_in]], constant Uniforms &u [[buffer(1)]],
 texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]]) {
 float4 texel=image.sample(filtering,in.uv), color=u.primary;
 if (u.mode == 0) return texel;
 if (u.mode == 1) return texel*color;
 if (u.mode == 2) return float4(mix(color.rgb,texel.rgb,texel.a),color.a);
 if (u.mode == 3) return clamp(float4(texel.rgb+color.rgb,texel.a*color.a),0.0,1.0);
 if (u.mode == 4) return float4(mix(color.rgb,u.constantColor.rgb,texel.rgb),texel.a*color.a);
 float3 a=operand(source(u.sourceRGB[0],texel,u),u.operandRGB[0]);
 float3 b=operand(source(u.sourceRGB[1],texel,u),u.operandRGB[1]);
 float aa=source(u.sourceAlpha[0],texel,u).a, ab=source(u.sourceAlpha[1],texel,u).a;
 if (u.operandAlpha[0]) aa=1-aa;
 if (u.operandAlpha[1]) ab=1-ab;
 return clamp(float4(u.rgbMode == 0 ? a : u.rgbMode == 1 ? a*b : a+b,
                     u.alphaMode == 0 ? aa : u.alphaMode == 1 ? aa*ab : aa+ab),0.0,1.0);
}
)metal";

static uint32 nativeMode(uint32 mode) {
	switch (mode) {
	case TGL_REPLACE: return 0;
	case TGL_MODULATE: return 1;
	case TGL_DECAL: return 2;
	case TGL_ADD: return 3;
	case TGL_BLEND: return 4;
	default: return 5;
	}
}

static uint32 nativeSource(uint32 source) {
	if (source == TGL_TEXTURE) return 0;
	if (source == TGL_CONSTANT) return 1;
	return 2; // primary and previous are identical for this single texture unit
}

static uint32 nativeOperand(uint32 operand) {
	if (operand == TGL_SRC_COLOR) return 0;
	if (operand == TGL_ONE_MINUS_SRC_COLOR) return 1;
	if (operand == TGL_SRC_ALPHA) return 2;
	return 3;
}

static uint32 nativeCombine(uint32 combine) {
	return combine == TGL_REPLACE ? 0 : combine == TGL_MODULATE ? 1 : 2;
}

static void copyAlphaIntoColorChannels(Graphics::ManagedSurface &surface) {
	for (int y=0; y<surface.h; ++y) {
		uint32 *pixels=(uint32 *)surface.getBasePtr(0,y);
		for (int x=0; x<surface.w; ++x) {
			byte a,r,g,b;
			surface.format.colorToARGB(pixels[x],a,r,g,b);
			pixels[x]=surface.format.ARGBToColor(255,a,a,a);
		}
	}
}

static void drawGridQuad(Graphics::Metal::Renderer &renderer, Graphics::Metal::Texture *texture, int row, int column) {
	using namespace Graphics::Metal;
	float x=kScreenWidth/4*(row*2+1)-kTextureSize/2;
	float y=kScreenHeight/4*(column*2+1)-kTextureSize/2;
	float l=2*x/kScreenWidth-1, r=2*(x+kTextureSize)/kScreenWidth-1;
	float t=1-2*y/kScreenHeight, b=1-2*(y+kTextureSize)/kScreenHeight;
	Vertex v[]={{{l,t,0},{0,0},{1,1,1,1}},{{r,t,0},{1,0},{1,1,1,1}},
	            {{l,b,0},{0,1},{1,1,1,1}},{{r,b,0},{1,1},{1,1,1,1}}};
	DrawState state; state.primitive=kTriangleStrip; state.linear=false;
	renderer.draw(v,4,texture,state,Uniforms());
}

TestExitStatus TinyGLTests::runTexEnvTest(const char *testName, const TextureEnvironment &env,
		byte colorR, byte colorG, byte colorB, byte colorA) {
	using namespace Graphics::Metal;
	int oldW=g_system->getWidth(), oldH=g_system->getHeight(), oldMode=g_system->getGraphicsMode();
	uint oldScaler=g_system->getScaler(), oldScaleFactor=g_system->getScaleFactor();
	Graphics::PixelFormat oldFormat=g_system->getScreenFormat();
	Graphics::ManagedSurface testImage;
	{
		Image::PNGDecoder png;
		Common::File file;
		if (!file.open("image/pm5544-32bpp-grayalpha.png") || !png.loadStream(file) ||
		    png.getSurface()->w<240 || png.getSurface()->h<240) {
			Testsuite::logDetailedPrintf("Error! Could not load texture environment image\n");
			return kTestFailed;
		}
		const Graphics::Surface &image=*png.getSurface();
		int size=MIN(image.w,image.h);
		Graphics::ManagedSurface converted;
		converted.convertFrom(image,Graphics::PixelFormat::createFormatRGBA32());
		testImage.create(kTextureSize,kTextureSize,converted.format);
		Graphics::scaleBlitBilinear((byte *)testImage.getPixels(),
			(const byte *)converted.getBasePtr((image.w-size)/2,(image.h-size)/2),
			testImage.pitch,converted.pitch,kTextureSize,kTextureSize,size,size,converted.format);
	}

	initGraphics3d(kScreenWidth,kScreenHeight,true);
	Renderer *renderer=g_system->getMetalRenderer();
	if (!renderer) error("Testbed: native Metal renderer is unavailable");
	Common::ScopedPtr<RenderTarget> target(renderer->createRenderTarget(kTextureSize,kTextureSize));
	Common::ScopedPtr<Texture> image(renderer->createTexture(testImage));
	Common::ScopedPtr<Program> program(renderer->createProgram(kEnvironmentShader,"environmentVertex","environmentFragment"));
	if (!target || !image || !program) error("Testbed: could not create native texture environment resources");
	EnvironmentUniforms u={};
	u.primary[0]=colorR/255.f; u.primary[1]=colorG/255.f; u.primary[2]=colorB/255.f; u.primary[3]=colorA/255.f;
	u.constantColor[0]=env._constantR/255.f; u.constantColor[1]=env._constantG/255.f;
	u.constantColor[2]=env._constantB/255.f; u.constantColor[3]=env._constantA/255.f;
	u.mode=nativeMode(env._mode); u.rgbMode=nativeCombine(env._combineRGB); u.alphaMode=nativeCombine(env._combineAlpha);
	const TextureEnvironmentArg *args[]={&env._arg0,&env._arg1};
	for (int i=0; i<2; ++i) {
		u.sourceRGB[i]=nativeSource(args[i]->_sourceRGB); u.operandRGB[i]=nativeOperand(args[i]->_operandRGB);
		u.sourceAlpha[i]=nativeSource(args[i]->_sourceAlpha); u.operandAlpha[i]=args[i]->_operandAlpha==TGL_ONE_MINUS_SRC_ALPHA;
	}
	Vertex vertices[]={{{-1,1,0},{0,0},{1,1,1,1}},{{1,1,0},{1,0},{1,1,1,1}},
	                   {{-1,-1,0},{0,1},{1,1,1,1}},{{1,-1,0},{1,1},{1,1,1,1}}};
	DrawState state; state.primitive=kTriangleStrip; state.linear=false;
	TextureBinding binding; binding.texture=image.get(); binding.linear=false;
	renderer->setRenderTarget(target.get()); renderer->clear(0,0,0,0);
	renderer->drawProgram(*program,vertices,4,sizeof(Vertex),nullptr,0,&u,sizeof(u),&binding,1,state);
	Graphics::Surface nativeSurface;
	if (!renderer->readTexture(target->texture(),nativeSurface,Common::Rect(kTextureSize,kTextureSize)))
		error("Testbed: native texture environment readback failed");
	renderer->setRenderTarget(nullptr);
	Graphics::ManagedSurface readBack; readBack.copyFrom(nativeSurface); nativeSurface.free();
	Common::ScopedPtr<Texture> nativeColor(renderer->createTexture(readBack));
	copyAlphaIntoColorChannels(readBack);
	Common::ScopedPtr<Texture> nativeAlpha(renderer->createTexture(readBack));

	Common::ScopedPtr<TinyGL::ContextHandle,TinyGLContextDeleter> context(
		TinyGL::createContext(kTextureSize,kTextureSize,testImage.format,kTextureSize,false,false));
	TinyGL::setContext(context.get());
	tglMatrixMode(TGL_MODELVIEW); tglLoadIdentity();
	tglMatrixMode(TGL_PROJECTION); tglLoadIdentity();
	tglOrtho(0,kTextureSize,0,kTextureSize,-1,1); tglViewport(0,0,kTextureSize,kTextureSize);
	tglDisable(TGL_BLEND); tglDisable(TGL_DEPTH_TEST); tglDisable(TGL_CULL_FACE);
	tglEnable(TGL_TEXTURE_2D); tglClearColor(0,0,0,0);
	TGLuint tinyTexture; tglGenTextures(1,&tinyTexture); tglBindTexture(TGL_TEXTURE_2D,tinyTexture);
	tglTexParameteri(TGL_TEXTURE_2D,TGL_TEXTURE_MIN_FILTER,TGL_NEAREST);
	tglTexParameteri(TGL_TEXTURE_2D,TGL_TEXTURE_MAG_FILTER,TGL_NEAREST);
	tglTexImage2D(TGL_TEXTURE_2D,0,TGL_RGBA,kTextureSize,kTextureSize,0,TGL_RGBA,TGL_UNSIGNED_BYTE,testImage.getPixels());
	env.apply(tglTexEnvi,tglTexEnvfv); tglClear(TGL_COLOR_BUFFER_BIT); tglColor4ub(colorR,colorG,colorB,colorA);
	tglBegin(TGL_TRIANGLE_STRIP);
	tglTexCoord2f(0,1); tglVertex2f(0,0);
	tglTexCoord2f(1,1); tglVertex2f(kTextureSize,0);
	tglTexCoord2f(0,0); tglVertex2f(0,kTextureSize);
	tglTexCoord2f(1,0); tglVertex2f(kTextureSize,kTextureSize);
	tglEnd(); TinyGL::presentBuffer();
	Graphics::Surface tinySurface; TinyGL::getSurfaceRef(tinySurface); readBack.copyFrom(tinySurface);
	Common::ScopedPtr<Texture> tinyColor(renderer->createTexture(readBack));
	copyAlphaIntoColorChannels(readBack);
	Common::ScopedPtr<Texture> tinyAlpha(renderer->createTexture(readBack));
	tglDeleteTextures(1,&tinyTexture);

	renderer->setViewport(Common::Rect(renderer->width(),renderer->height())); renderer->clear(0,0,0,0);
	drawGridQuad(*renderer,nativeColor.get(),0,0); drawGridQuad(*renderer,nativeAlpha.get(),1,0);
	drawGridQuad(*renderer,tinyColor.get(),0,1); drawGridQuad(*renderer,tinyAlpha.get(),1,1);
	g_system->updateScreen(); g_system->delayMillis(1000);
	Common::String question=Common::String::format("Does the top row of images look like the bottom row?\n(Testing Metal 4 / TinyGL: %s)",testName);
	TestExitStatus status=kTestPassed;
	if (Testsuite::handleInteractiveInput(question,"Yes","No",kOptionRight)) {
		Testsuite::logDetailedPrintf("Error! TinyGL and Metal have different texture environment behaviors for %s\n",testName);
		status=kTestFailed;
	}
	// Release native resources before restoring the previous backend mode.
	nativeColor.reset(); nativeAlpha.reset(); tinyColor.reset(); tinyAlpha.reset();
	target.reset(); image.reset(); program.reset();
	g_system->beginGFXTransaction(); g_system->setGraphicsMode(oldMode); g_system->initSize(oldW,oldH,&oldFormat);
	g_system->setScaler(oldScaler,oldScaleFactor);
	g_system->endGFXTransaction();
	return status;
}

} // namespace Testbed

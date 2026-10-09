/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "colony/renderer.h"
#include "common/config-manager.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "graphics/metal/renderer.h"
#include "math/matrix4.h"

namespace Colony {

namespace Metal = Graphics::Metal;

class MetalRenderer : public Renderer {
public:
	MetalRenderer(OSystem *system, int width, int height) : _system(system), _width(width), _height(height) {
		_renderer = system->getMetalRenderer();
		if (!_renderer)
			error("Colony: Metal renderer unavailable");
		memset(_palette, 255, sizeof(_palette));
		_uniforms2D.transform[0] = 2.0f / width;
		_uniforms2D.transform[5] = -2.0f / height;
		_uniforms2D.transform[10] = -1;
		_uniforms2D.transform[12] = -1;
		_uniforms2D.transform[13] = 1;
		computeScreenViewport();
	}
	~MetalRenderer() override { _renderer->flush(true); }
	Graphics::PixelFormat getPixelFormat() override { return Graphics::PixelFormat::createFormatRGBA32(); }
	void clear(uint32 color) override {
		float c[4]; resolveColor(color, c);
		_renderer->clear(c[0], c[1], c[2], c[3]);
	}
	void setPalette(const byte *palette, uint start, uint count) override {
		if (start >= 256) return;
		memcpy(_palette + start * 3, palette, MIN(count, 256 - start) * 3);
	}
	void drawLine(int x1, int y1, int x2, int y2, uint32 color) override {
		const float p[] = {(float)x1, (float)y1, 0, (float)x2, (float)y2, 0};
		draw(p, 2, color, false, false);
	}
	void drawRect(const Common::Rect &r, uint32 color) override {
		const float p[] = {(float)r.left,(float)r.top,0, (float)r.right,(float)r.top,0,
		                   (float)r.right,(float)r.bottom,0, (float)r.left,(float)r.bottom,0};
		draw(p, 4, color, false, true);
	}
	void fillRect(const Common::Rect &r, uint32 color) override {
		const float p[] = {(float)r.left,(float)r.top,0, (float)r.right,(float)r.top,0,
		                   (float)r.right,(float)r.bottom,0, (float)r.left,(float)r.bottom,0};
		draw(p, 4, color, true, false);
	}
	void setPixel(int x, int y, uint32 color) override { fillRect(Common::Rect(x,y,x+1,y+1), color); }
	void drawQuad(int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4, uint32 color) override {
		const int x[] = {x1,x2,x3,x4}, y[] = {y1,y2,y3,y4};
		drawPolygon(x, y, 4, color);
	}
	void drawPolygon(const int *x, const int *y, int count, uint32 color) override {
		if (count < 3) return;
		Common::Array<float> p; p.resize(count * 3);
		for (int i = 0; i < count; ++i) { p[3*i] = x[i]; p[3*i+1] = y[i]; p[3*i+2] = 0; }
		draw(p.data(), count, color, true, false);
		draw(p.data(), count, 0xffffffff, false, true);
	}
	void drawEllipse(int x, int y, int rx, int ry, uint32 color) override { ellipse(x,y,rx,ry,color,false); }
	void fillEllipse(int x, int y, int rx, int ry, uint32 color) override { ellipse(x,y,rx,ry,color,true); }
	void fillDitherRect(const Common::Rect &r, uint32 c1, uint32 c2) override {
		if (r.isEmpty()) return;
		Graphics::Surface image;
		image.create(r.width(), r.height(), getPixelFormat());
		float a[4], b[4]; resolveColor(c1,a); resolveColor(c2,b);
		uint32 colors[] = {image.format.RGBToColor(a[0]*255,a[1]*255,a[2]*255),
		                   image.format.RGBToColor(b[0]*255,b[1]*255,b[2]*255)};
		for (int y = 0; y < image.h; ++y)
			for (int x = 0; x < image.w; ++x)
				*(uint32 *)image.getBasePtr(x,y) = colors[(x+y+1)&1];
		drawSurface(&image, r.left, r.top);
		image.free();
	}
	void drawString(const Graphics::Font *font, const Common::String &text, int x, int y,
	                uint32 color, Graphics::TextAlign align) override {
		if (!font) return;
		int w = font->getStringWidth(text), h = font->getFontHeight();
		if (w <= 0 || h <= 0) return;
		if (align == Graphics::kTextAlignCenter) x -= w / 2;
		else if (align == Graphics::kTextAlignRight) x -= w;
		Graphics::Surface mask, image;
		mask.create(w,h,Graphics::PixelFormat::createFormatCLUT8());
		mask.fillRect(Common::Rect(w,h),0);
		font->drawString(&mask,text,0,0,w,1,Graphics::kTextAlignLeft);
		image.create(w,h,getPixelFormat());
		float c[4]; resolveColor(color,c);
		uint32 ink = image.format.ARGBToColor(255,c[0]*255,c[1]*255,c[2]*255);
		for (int py = 0; py < h; ++py)
			for (int px = 0; px < w; ++px)
				*(uint32 *)image.getBasePtr(px,py) = *(const byte *)mask.getBasePtr(px,py) ? ink : 0;
		drawSurface(&image,x,y);
		mask.free(); image.free();
	}
	void drawSurface(const Graphics::Surface *surface, int x, int y) override {
		if (!surface || surface->w <= 0 || surface->h <= 0) return;
		Metal::Texture *texture = _renderer->createTexture(*surface);
		if (!texture) error("Colony: could not create Metal bitmap");
		Metal::Vertex v[] = {{{(float)x,(float)y,0},{0,0},{1,1,1,1}},
		                     {{(float)(x+surface->w),(float)y,0},{1,0},{1,1,1,1}},
		                     {{(float)x,(float)(y+surface->h),0},{0,1},{1,1,1,1}},
		                     {{(float)(x+surface->w),(float)(y+surface->h),0},{1,1},{1,1,1,1}}};
		Metal::DrawState state;
		state.primitive = Metal::kTriangleStrip; state.linear = false; state.blend = true;
		state.srcAlpha = Metal::kSrcAlpha;
		_renderer->draw(v,4,texture,state,_uniforms2D);
		delete texture;
	}
	void begin3D(int camX, int camY, int camZ, int angle, int angleY, const Common::Rect &viewport) override {
		_renderer->clear(0,0,0,1,false,true,false);
		_active3DViewport = viewport;
		Common::Rect native = mapRect(viewport);
		_renderer->setViewport(native);
		_state3D.depthTest = _state3D.depthWrite = true;
		_state3D.depthCompare = Metal::kLessEqual;
		_state3D.scissor = true; _state3D.scissorRect = native;
		_in3D = true;
		Math::Matrix4 projection;
		for (int r=0; r<4; ++r) for (int c=0; c<4; ++c) projection(r,c)=0;
		const float nearClip=1, farClip=10000;
		projection(0,0)=2*kProjectionFocalLength/viewport.width();
		projection(1,1)=2*kProjectionFocalLength/viewport.height();
		projection(2,2)=-(farClip+nearClip)/(farClip-nearClip);
		projection(2,3)=-2*farClip*nearClip/(farClip-nearClip);
		projection(3,2)=-1;
		Math::Matrix4 pitch, minus90, yaw, translation;
		pitch.buildAroundX(angleY*360.0f/256);
		minus90.buildAroundX(-90);
		yaw.buildAroundZ(-angle*360.0f/256+90);
		translation.setPosition(Math::Vector3d(-camX,-camY,-camZ));
		Math::Matrix4 mvp = projection * pitch * minus90 * yaw * translation;
		mvp.transpose();
		memcpy(_uniforms3D.transform,mvp.getData(),sizeof(_uniforms3D.transform));
	}
	void draw3DWall(int x1, int y1, int x2, int y2, uint32 color) override {
		const float p[] = {x1*256.0f,y1*256.0f,-160, x2*256.0f,y2*256.0f,-160,
		                   x2*256.0f,y2*256.0f,160, x1*256.0f,y1*256.0f,160};
		polygon3D(p,4,color);
	}
	void draw3DQuad(float x1,float y1,float z1,float x2,float y2,float z2,
	                float x3,float y3,float z3,float x4,float y4,float z4,uint32 color) override {
		const float p[] = {x1,y1,z1,x2,y2,z2,x3,y3,z3,x4,y4,z4}; polygon3D(p,4,color);
	}
	void draw3DPolygon(const float *x,const float *y,const float *z,int count,uint32 color) override {
		if (count <= 0) return;
		Common::Array<float> p; p.resize(count*3);
		for (int i=0;i<count;++i) {p[3*i]=x[i];p[3*i+1]=y[i];p[3*i+2]=z[i];}
		polygon3D(p.data(),count,color);
	}
	void draw3DLine(float x1,float y1,float z1,float x2,float y2,float z2,uint32 color) override {
		const float p[] = {x1,y1,z1,x2,y2,z2}; draw(p,2,color,false,false);
	}
	void end3D() override {
		_in3D = false; _state3D = Metal::DrawState();
		computeScreenViewport();
	}
	void setWireframe(bool enabled,int64_t fillColor) override {_wireframe=enabled;_fillColor=fillColor;}
	void setXorMode(bool enabled) override {_xor=enabled;}
	void setMacColors(uint32 fg,uint32 bg) override {_fg=fg;_bg=bg;}
	void setStippleData(const byte *data) override {
		_stipple=bool(data);
		if (data) memcpy(_uniforms3D.stipple,data,128);
	}
	void setDepthState(bool test,bool write) override {_state3D.depthTest=test;_state3D.depthWrite=write;}
	void setDepthRange(float nearVal,float farVal) override {_state3D.depthNear=nearVal;_state3D.depthFar=farVal;}
	void setFeatureClipX(int left,int right) override {
		Common::Rect r=_active3DViewport;
		r.left=MAX<int>(r.left,left);r.right=MAX<int>(r.left,MIN<int>(r.right,right));
		_state3D.scissorRect=mapRect(r);
	}
	void clearFeatureClipX() override {_state3D.scissorRect=mapRect(_active3DViewport);}
	void setSquarePixelViewport(bool enabled) override {_square=enabled;computeScreenViewport();}
	void computeScreenViewport() override {
		int w=_system->getWidth(), h=_system->getHeight();
		int numerator=0,denominator=1;
		if (_square) {numerator=_width;denominator=_height;}
		else if (ConfMan.getBool("widescreen_mod")) {numerator=16;denominator=9;}
		else if (_system->getFeatureState(OSystem::kFeatureAspectRatioCorrection)) {numerator=4;denominator=3;}
		int vw=numerator?MIN(w,(h*numerator+denominator/2)/denominator):w;
		int vh=numerator?MIN(h,(w*denominator+numerator/2)/numerator):h;
		_screenViewport=Common::Rect(vw,vh);_screenViewport.translate((w-vw)/2,(h-vh)/2);
		_renderer->setViewport(_screenViewport);
	}
	void copyToScreen() override {_renderer->flush();_system->updateScreen();}
	Graphics::Surface *getScreenshot() override {
		Graphics::Surface *surface=new Graphics::Surface();
		if (!_renderer->readPixels(*surface,_screenViewport)) {delete surface;return nullptr;}
		return surface;
	}

private:
	void resolveColor(uint32 color,float rgba[4]) const {
		if (color&0xff000000) {rgba[0]=((color>>16)&255)/255.0f;rgba[1]=((color>>8)&255)/255.0f;rgba[2]=(color&255)/255.0f;}
		else {uint index=(color&255)*3;for (uint i=0;i<3;++i) rgba[i]=_palette[index+i]/255.0f;}
		rgba[3]=1;
	}
	Common::Rect mapRect(const Common::Rect &r) const {
		float sx=(float)_screenViewport.width()/_width,sy=(float)_screenViewport.height()/_height;
		int x=_screenViewport.left+(int)(r.left*sx), y=_screenViewport.top+(int)(r.top*sy);
		return Common::Rect(x,y,x+(int)(r.width()*sx),y+(int)(r.height()*sy));
	}
	void draw(const float *positions,int count,uint32 color,bool fill,bool closed,bool stipple=false) {
		if (count<=0) return;
		Metal::DrawState state=_in3D?_state3D:Metal::DrawState();
		Metal::Uniforms uniforms=_in3D?_uniforms3D:_uniforms2D;
		uniforms.stippled=stipple;
		if (!_in3D && _xor) {
			state.logicXor = true;
			state.blend = false;
		}
		Common::Array<Metal::Vertex> vertices;vertices.resize(count);
		for (int i=0;i<count;++i) {
			memcpy(vertices[i].position,positions+3*i,3*sizeof(float));
			vertices[i].texcoord[0]=vertices[i].texcoord[1]=0;
			resolveColor(color,vertices[i].color);
		}
		if (fill && count>=3) {
			Common::Array<uint32> indices;
			for (int i=1;i+1<count;++i) {indices.push_back(0);indices.push_back(i);indices.push_back(i+1);}
			_renderer->drawIndexed(vertices.data(),count,indices.data(),indices.size(),nullptr,state,uniforms);
		} else {
			float width=MAX(1.0f,(float)_system->getWidth()/_width);
			for (int i=1;i<count;++i) _renderer->drawWideLine(vertices[i-1],vertices[i],width,state,uniforms);
			if (closed && count>1) _renderer->drawWideLine(vertices[count-1],vertices[0],width,state,uniforms);
		}
	}
	void polygon3D(const float *p,int count,uint32 color) {
		if (count<3) {draw(p,count,color,false,false);return;}
		if (!_wireframe || _stipple || _fillColor!=-1) {
			_state3D.depthBias=4;_state3D.slopeBias=1.1f;
			if (_stipple) {
				draw(p,count,_bg,true,false);
				draw(p,count,_fg,true,false,true);
			} else draw(p,count,_wireframe?(uint32)_fillColor:color,true,false);
			_state3D.depthBias=_state3D.slopeBias=0;
		}
		if (_wireframe) draw(p,count,color,false,true);
	}
	void ellipse(int x,int y,int rx,int ry,uint32 color,bool fill) {
		float p[36*3];
		for (uint i=0;i<36;++i) {float rad=i*2*(float)M_PI/36;p[3*i]=x+cosf(rad)*rx;p[3*i+1]=y+sinf(rad)*ry;p[3*i+2]=0;}
		draw(p,36,color,fill,!fill);
	}
	OSystem *_system;
	Metal::Renderer *_renderer;
	int _width,_height;
	byte _palette[768];
	Metal::Uniforms _uniforms2D,_uniforms3D;
	Metal::DrawState _state3D;
	Common::Rect _active3DViewport;
	bool _in3D=false,_wireframe=true,_stipple=false,_xor=false,_square=false;
	int64_t _fillColor=0;
	uint32 _fg=0,_bg=255;
};

Renderer *createMetalRenderer(OSystem *system,int width,int height) {return new MetalRenderer(system,width,height);}

} // End of namespace Colony

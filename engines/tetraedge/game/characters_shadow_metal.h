/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TETRAEDGE_GAME_CHARACTERS_SHADOW_METAL_H
#define TETRAEDGE_GAME_CHARACTERS_SHADOW_METAL_H

#include "tetraedge/game/characters_shadow.h"
#include "graphics/metal/renderer.h"

namespace Tetraedge {

class CharactersShadowMetal : public CharactersShadow {
public:
	~CharactersShadowMetal() override;
	void draw(InGameScene *scene) override;

protected:
	void createInternal() override;
	void createTextureInternal(InGameScene *scene) override;
	void deleteTexture() override;

private:
	Graphics::Metal::RenderTarget *_target = nullptr;
};

} // End of namespace Tetraedge

#endif

/* ScummVM - Graphic Adventure Engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tetraedge/game/characters_shadow_metal.h"
#include "tetraedge/game/character.h"
#include "tetraedge/te/te_renderer_metal.h"
#include "tetraedge/tetraedge.h"
#include "common/textconsole.h"

namespace Tetraedge {

static TeRendererMetal &shadowRenderer() {
	TeRendererMetal *renderer = dynamic_cast<TeRendererMetal *>(g_engine->getRenderer());
	if (!renderer)
		error("Tetraedge: Metal shadow renderer unavailable");
	return *renderer;
}

CharactersShadowMetal::~CharactersShadowMetal() {
	deleteTexture();
}

void CharactersShadowMetal::createInternal() {
	deleteTexture();
	_target = shadowRenderer().metal().createRenderTarget(_texSize, _texSize);
	if (!_target)
		error("Tetraedge: could not allocate Metal shadow target");
}

void CharactersShadowMetal::createTextureInternal(InGameScene *scene) {
	TeRendererMetal &renderer = shadowRenderer();
	Graphics::Metal::Renderer &metal = renderer.metal();
	metal.setRenderTarget(_target);
	// The base class applied the camera against the screen dimensions. Apply
	// again after selecting the square target so its viewport has the right origin.
	_camera->apply();
	metal.clear(0, 0, 0, 0);
	for (Character *character : scene->_characters)
		character->_model->draw();
	if (scene->_character)
		scene->_character->_model->draw();
	metal.setRenderTarget(nullptr);
}

void CharactersShadowMetal::deleteTexture() {
	if (!_target)
		return;
	TeRendererMetal *renderer = g_engine ? dynamic_cast<TeRendererMetal *>(g_engine->getRenderer()) : nullptr;
	if (renderer) {
		renderer->disableTexture();
		renderer->releaseTexture(&_target->texture());
		if (renderer->metal().renderTarget() == _target) renderer->metal().setRenderTarget(nullptr);
	}
	delete _target;
	_target = nullptr;
}

void CharactersShadowMetal::draw(InGameScene *scene) {
	if (!_target)
		return;
	TeRendererMetal &renderer = shadowRenderer();
	renderer.disableZBuffer();
	renderer.enableTexture();
	renderer.setCurrentColor(scene->shadowColor());

	TeMatrix4x4 projection;
	projection.translate(TeVector3f32(0.5f, 0.5f, 0.5f));
	projection.scale(TeVector3f32(0.5f, 0.5f, 0.5f));
	TeMatrix4x4 camera = _camera->worldTransformationMatrix();
	camera.inverse();
	projection = projection * _camera->projectionMatrix() * camera;
	renderer.setShadowTexture(&_target->texture(), projection);

	Common::Array<TeIntrusivePtr<TeModel>> &models =
		g_engine->gameType() == TetraedgeEngine::kSyberia ? scene->zoneModels() : scene->shadowReceivingObjects();
	for (TeIntrusivePtr<TeModel> model : models) {
		if (!model->meshes().empty() && model->meshes()[0]->materials().empty()) {
			model->meshes()[0]->defaultMaterial(TeIntrusivePtr<Te3DTexture>());
			model->meshes()[0]->materials()[0]._isShadowTexture = true;
			model->meshes()[0]->materials()[0]._diffuseColor = scene->shadowColor();
		}
		model->draw();
	}
	renderer.disableTexture();
	renderer.enableZBuffer();
}

} // End of namespace Tetraedge

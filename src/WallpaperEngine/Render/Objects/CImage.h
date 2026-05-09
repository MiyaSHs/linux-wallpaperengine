#pragma once

#include "CRenderable.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Puppet/MdlParser.h"
#include "WallpaperEngine/Render/Puppet/Puppet.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include "WallpaperEngine/Render/Shaders/Shader.h"

#include "../TextureProvider.h"

#include <glm/vec3.hpp>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render::Objects::Effects {
class CMaterial;
class CPass;
} // namespace WallpaperEngine::Render::Objects::Effects

namespace WallpaperEngine::Render::Objects {
class CEffect;

class CImage final : public CRenderable {
    friend CObject;

public:
    CImage (Wallpapers::CScene& scene, const Image& image);
    ~CImage () override;

    void setup () override;
    void render () override;

    [[nodiscard]] const Image& getImage () const;
    [[nodiscard]] const std::vector<CEffect*>& getEffects () const;
    [[nodiscard]] const Effects::CMaterial* getMaterial () const;
    [[nodiscard]] glm::vec2 getSize () const;

    [[nodiscard]] GLuint getSceneSpacePosition () const;
    [[nodiscard]] GLuint getCopySpacePosition () const;
    [[nodiscard]] GLuint getPassSpacePosition () const;
    [[nodiscard]] GLuint getTexCoordCopy () const;
    [[nodiscard]] GLuint getTexCoordPass () const;

    [[nodiscard]] const std::vector<Render::Puppet::Mdl::Attachment>& getPuppetAttachments () const {
	return m_puppetMdl.attachments;
    }
    [[nodiscard]] const std::vector<Render::Puppet::Puppet::Bone>& getPuppetBones () const {
	static const std::vector<Render::Puppet::Puppet::Bone> empty;
	return m_puppetMdl.puppet ? m_puppetMdl.puppet->bones : empty;
    }

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

    /**
     * Performs a ping-pong on the available framebuffers to be able to continue rendering things to them
     *
     * @param drawTo The framebuffer to use
     * @param asInput The last texture used as output (if needed)
     */
    void pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput);

protected:
    void setupPasses ();

    void updateScreenSpacePosition ();

private:
    GLuint m_sceneSpacePosition;
    GLuint m_copySpacePosition;
    GLuint m_passSpacePosition;
    GLuint m_texcoordCopy;
    GLuint m_texcoordPass;

    glm::mat4 m_modelViewProjectionScreen = {};
    glm::mat4 m_modelViewProjectionPass = {};
    glm::mat4 m_modelViewProjectionCopy = {};
    glm::mat4 m_modelViewProjectionScreenInverse = {};
    glm::mat4 m_modelViewProjectionPassInverse = {};
    glm::mat4 m_modelViewProjectionCopyInverse = {};

    glm::mat4 m_modelMatrix = {};
    glm::mat4 m_viewProjectionMatrix = {};

    std::shared_ptr<const CFBO> m_mainFBO = nullptr;
    std::shared_ptr<const CFBO> m_subFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentMainFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentSubFBO = nullptr;

    const Image& m_image;

    std::vector<CEffect*> m_effects = {};
    Effects::CMaterial* m_material = nullptr;
    Effects::CMaterial* m_colorBlendMaterial = nullptr;
    std::vector<Effects::CPass*> m_passes = {};
    std::vector<MaterialPassUniquePtr> m_virtualPassess = {};

    glm::vec4 m_pos = {};
    glm::vec3 m_sceneCenter = {};

    bool m_initialized = false;

    struct {
	struct {
	    MaterialUniquePtr material;
	    ImageEffectPassOverrideUniquePtr override;
	} colorBlending;
    } m_materials;

    // ----- Puppet warp ---------------------------------------------------------------------
    // Set when m_image.model->puppet has a value. The .mdl is parsed in the constructor and
    // its mesh + bone hierarchy + animations are stored here. Per-frame, we compute bone
    // matrices and upload them as the g_Bones uniform via a geometry callback on the main
    // pass (which we also tell to render glDrawElements over our skinned mesh, not the quad).
    bool m_isPuppet = false;
    Render::Puppet::Mdl m_puppetMdl;
    Render::Puppet::PuppetLayer m_puppetLayer;
    std::vector<Render::Puppet::PuppetLayer::AnimationLayer> m_puppetAnimLayers;

    GLuint m_puppetVAO = 0;
    GLuint m_puppetVBO = 0;
    GLuint m_puppetEBO = 0;
    GLsizei m_puppetIndexCount = 0;
    GLint m_puppetSavedVAO = 0;

    /** Repacked bone matrices for upload as g_Bones (mat4x3 column-major: 12 floats/bone). */
    std::vector<float> m_boneMatrixUpload;
    /** Last simulation time used to advance per-layer animation timers. */
    double m_puppetLastTime = 0.0;
    /** Pass override carrying SKINNING=1 / BONECOUNT=N combos. Owned here so its lifetime
     *  matches the pass that references it. */
    std::unique_ptr<ImageEffectPassOverride> m_puppetPassOverride;

    /** Puppet vertices are in image-local centered model space (Y-down — smaller Y is
     *  visually-up per UV convention). We render the puppet in one of two ways:
     *
     *   - Multi-pass (image has effects): the first pass renders into the image's own FBO
     *     using image-local ortho. This places the puppet vertices at the correct location
     *     within the FBO, and the existing scene-composite final pass handles scene
     *     placement. We use m_puppetLocalM/MVP for this.
     *
     *   - Single-pass (no effects, the material's pass IS the scene-composite): the pass
     *     writes direct to scene FBO. We use m_puppetSceneM/MVP for this case.
     *
     *  The Y-flip in m_puppetLocalM converts the puppet's Y-down convention into the FBO's
     *  Y-up OpenGL convention so subsequent flat-quad passes sample the result correctly.
     */
    glm::mat4 m_puppetLocalM = glm::mat4 (1.0f);
    glm::mat4 m_puppetLocalMVP = glm::mat4 (1.0f);
    glm::mat4 m_puppetLocalMVPInverse = glm::mat4 (1.0f);
    glm::mat4 m_puppetSceneM = glm::mat4 (1.0f);
    glm::mat4 m_puppetSceneVP = glm::mat4 (1.0f);
    glm::mat4 m_puppetSceneMVP = glm::mat4 (1.0f);
    glm::mat4 m_puppetSceneMVPInverse = glm::mat4 (1.0f);

    /** Parse the puppet .mdl, build VBO/EBO/VAO, snapshot animationLayers config.
     *  Returns false on failure — caller falls back to non-puppet quad rendering. */
    bool initPuppet ();
    /** Compute bone matrices for the current frame and pack into m_boneMatrixUpload. */
    void updatePuppetBones ();
};
} // namespace WallpaperEngine::Render::Objects

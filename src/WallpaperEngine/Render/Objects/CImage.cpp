#include "CImage.h"

#include "CRenderable.h"

#include <functional>
#include <sstream>

// Render-time clock — shared with CScene/CParticle. Declared in main.cpp.
extern float g_Time;
extern float g_TimeLast;

#include <glm/gtc/matrix_transform.hpp>

#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;

CImage::CImage (Wallpapers::CScene& scene, const Image& image) :
    CRenderable (scene, image, *image.model->material), m_sceneSpacePosition (GL_NONE), m_copySpacePosition (GL_NONE),
    m_passSpacePosition (GL_NONE), m_texcoordCopy (GL_NONE), m_texcoordPass (GL_NONE), m_modelViewProjectionScreen (),
    m_modelViewProjectionPass (glm::mat4 (1.0)), m_modelViewProjectionCopy (), m_modelViewProjectionScreenInverse (),
    m_modelViewProjectionPassInverse (glm::inverse (m_modelViewProjectionPass)), m_modelViewProjectionCopyInverse (),
    m_modelMatrix (), m_viewProjectionMatrix (), m_image (image), m_material (nullptr), m_colorBlendMaterial (nullptr),
    m_pos (), m_initialized (false) {
    // get scene width and height to calculate positions
    auto scene_width = static_cast<float> (scene.getWidth ());
    auto scene_height = static_cast<float> (scene.getHeight ());

    // TODO: MAKE USE OF THE USER PROPERTIES POINTER HERE TOO! SO EVERYTHING IS UPDATED ACCORDINGLY
    glm::vec3 origin = this->getImage ().origin->value->getVec3 ();
    glm::vec2 size = this->getSize ();
    glm::vec3 scale = this->getImage ().scale->value->getVec3 ();

    // Walk the parent chain accumulating each ancestor's stored origin. WPE wallpapers
    // can nest parents several levels deep — e.g. 3219908811 has head→torso→hairstyle
    // where only the topmost ancestor stores a non-zero origin and the intermediate
    // links exist purely for grouping. Reading just the immediate parent's origin
    // would leave the head at (0,0,0). Bound the walk to guard against pathological
    // cycles in malformed scene.json.
    auto walkParentChain = [&] (std::optional<int> from) -> glm::vec3 {
	glm::vec3 acc (0.0f);
	auto cur = from;
	int hops = 0;
	while (cur.has_value () && hops++ < 32) {
	    const auto& ancestor = this->getScene ().getObject (cur.value ())->getObject ();
	    acc += ancestor.origin->value->getVec3 ();
	    cur = ancestor.parent;
	}
	return acc;
    };
    origin += walkParentChain (this->m_image.parent);

    // NOTE: WPE puppets expose named MDAT attachment points (e.g. "头" on the body
    // puppet of 3363252053) with a `bone_index` and a 4x4 transform offset from
    // that bone. A child image with `attachment: <name>` should follow that
    // bone+offset position as the parent animates. Both `boneIndex` and the
    // attachment matrix are now parsed and stored in m_puppetMdl, but the exact
    // formula composing them with the child image's `origin` field is not yet
    // verified — naive interpretations (bone-world * attachment-local + parent
    // origin) produce visually wrong placements. Leaving this disabled until I
    // can correlate against a concrete WPE-rendered reference.

    // Note: model->cropoffset is editor-only metadata. The WPE editor's
    // "Align with background" context-menu button calls `centerRenderable(id,
    // cropoffset)` which updates the image's origin to put visible content at
    // canvas center; the cropoffset value itself is not applied at runtime.
    // We deliberately do NOT add it to origin here.

    this->detectTexture ();

    // detect texture (if any)
    if (this->m_texture == nullptr) {
	if (this->m_image.model->solidlayer && size.x == 0.0f && size.y == 0.0f) {
	    size.x = scene_width;
	    size.y = scene_height;
	}
	// if (this->m_image->isSolid ()) // layer receives cursor events:
	// https://docs.wallpaperengine.io/en/scene/scenescript/reference/event/cursor.html same applies to effects
	// TODO: create a dummy texture of correct size, fbo constructors should be enough, but this should be properly
	// handled
	this->m_texture = std::make_shared<CFBO> (
	    "", TextureFormat_ARGB8888, TextureFlags_NoFlags, 1, size.x, size.y, size.x, size.y
	);
    }

    // If the wallpaper doesn't specify a size, fall back to the texture or model dimensions
    if ((size.x == 0.0f || size.y == 0.0f) && this->m_texture != nullptr) {
	size.x = static_cast<float> (this->m_texture->getRealWidth ());
	size.y = static_cast<float> (this->m_texture->getRealHeight ());
    } else if ((size.x == 0.0f || size.y == 0.0f) && this->getImage ().model->width.has_value ()
	       && this->getImage ().model->height.has_value ()) {
	size.x = static_cast<float> (this->getImage ().model->width.value ());
	size.y = static_cast<float> (this->getImage ().model->height.value ());
    }

    // fullscreen layers should use the whole projection's size
    // TODO: WHAT SHOULD AUTOSIZE DO?
    if (this->getImage ().model->fullscreen) {
	size = { scene_width, scene_height };
	origin = { scene_width / 2, scene_height / 2, 0 };

	// TODO: CHANGE ALIGNMENT TOO?
    }

    glm::vec2 scaledSize = size * glm::vec2 (scale);

    // calculate the center and shift from there
    this->m_pos.x = origin.x - (scaledSize.x / 2);
    this->m_pos.w = origin.y + (scaledSize.y / 2);
    this->m_pos.z = origin.x + (scaledSize.x / 2);
    this->m_pos.y = origin.y - (scaledSize.y / 2);

    if (this->getImage ().alignment.find ("top") != std::string::npos) {
	this->m_pos.y -= scaledSize.y / 2;
	this->m_pos.w -= scaledSize.y / 2;
    } else if (this->getImage ().alignment.find ("bottom") != std::string::npos) {
	this->m_pos.y += scaledSize.y / 2;
	this->m_pos.w += scaledSize.y / 2;
    }

    if (this->getImage ().alignment.find ("left") != std::string::npos) {
	this->m_pos.x += scaledSize.x / 2;
	this->m_pos.z += scaledSize.x / 2;
    } else if (this->getImage ().alignment.find ("right") != std::string::npos) {
	this->m_pos.x -= scaledSize.x / 2;
	this->m_pos.z -= scaledSize.x / 2;
    }

    // wallpaper engine
    this->m_pos.x -= scene_width / 2;
    this->m_pos.y = scene_height / 2 - this->m_pos.y;
    this->m_pos.z -= scene_width / 2;
    this->m_pos.w = scene_height / 2 - this->m_pos.w;

    // register both FBOs into the scene
    std::ostringstream nameA, nameB;

    // TODO: determine when _rt_imageLayerComposite and _rt_imageLayerAlbedo is used
    nameA << "_rt_imageLayerComposite_" << this->getImage ().id << "_a";
    nameB << "_rt_imageLayerComposite_" << this->getImage ().id << "_b";

    this->m_currentMainFBO = this->m_mainFBO = scene.create (
	nameA.str (), TextureFormat_ARGB8888, this->m_texture->getFlags (), 1, { size.x, size.y }, { size.x, size.y }
    );
    this->m_currentSubFBO = this->m_subFBO = scene.create (
	nameB.str (), TextureFormat_ARGB8888, this->m_texture->getFlags (), 1, { size.x, size.y }, { size.x, size.y }
    );

    // build a list of vertices, these might need some change later (or maybe invert the camera)
    GLfloat sceneSpacePosition[] = { this->m_pos.x, this->m_pos.y, 0.0f, this->m_pos.x, this->m_pos.w, 0.0f,
				     this->m_pos.z, this->m_pos.y, 0.0f, this->m_pos.z, this->m_pos.y, 0.0f,
				     this->m_pos.x, this->m_pos.w, 0.0f, this->m_pos.z, this->m_pos.w, 0.0f };

    float width = 1.0f;
    float height = 1.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animated images use different coordinates as they're essentially a texture atlas
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }
    // calculate the correct texCoord limits for the texture based on the texture screen size and real size
    else if (this->getTexture () != nullptr
	     && (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
		 || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())) {
	// Account for padding in non-power-of-two textures: clamp UVs to the real content
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    // TODO: RECALCULATE THESE POSITIONS FOR PASSTHROUGH SO THEY TAKE THE RIGHT PART OF THE TEXTURE
    float x = 0.0f;
    float y = 0.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animations should be copied completely
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
    }

    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0;
    GLfloat realY = 0.0;

    if (this->getImage ().model->passthrough) {
	x = -((this->m_pos.x + (scene_width / 2)) / size.x);
	y = -((this->m_pos.w + (scene_height / 2)) / size.y);
	height = (this->m_pos.y + (scene_height / 2)) / size.y;
	width = (this->m_pos.z + (scene_width / 2)) / size.x;

	if (this->getImage ().model->fullscreen) {
	    realX = -1.0;
	    realY = -1.0;
	    realWidth = 1.0;
	    realHeight = 1.0;
	}
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };

    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };

    GLfloat texcoordPass[] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    GLfloat passSpacePosition[]
	= { -1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, 1.0, 0.0f, 1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, -1.0, 0.0f };

    // bind vertex list to the openGL buffers
    glGenBuffers (1, &this->m_sceneSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_copySpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_STATIC_DRAW);

    // bind pass' vertex list to the openGL buffers
    glGenBuffers (1, &this->m_passSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_passSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (passSpacePosition), passSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordCopy);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordPass);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordPass);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordPass), texcoordPass, GL_STATIC_DRAW);

    // compute the center of the image in scene space for rotation
    this->m_sceneCenter = glm::vec3 (
	(this->m_pos.x + this->m_pos.z) / 2.0f,
	(this->m_pos.y + this->m_pos.w) / 2.0f,
	0.0f
    );

    this->m_modelViewProjectionScreen
	= this->getScene ().getCamera ().getProjection () * this->getScene ().getCamera ().getLookAt ();

    this->m_modelViewProjectionCopy = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, size.x, 0.0, size.y);
    this->m_viewProjectionMatrix = glm::mat4 (1.0);

    // ensure the input texture is marked as used
    // this makes video playback start if it's not already
    this->m_texture->incrementUsageCount ();

    // If this image declares a puppet, parse the .mdl and prepare GPU buffers for skinned
    // rendering. On any failure we just don't set m_isPuppet — the image falls back to the
    // regular flat-quad path, which at least shows the un-deformed source texture.
    if (this->m_image.model->puppet.has_value ()) {
	if (this->initPuppet ()) {
	    this->m_isPuppet = true;
	}
    }
}

CImage::~CImage () {
    this->m_texture->decrementUsageCount ();

    // delete passes first as they depend on the image's data
    for (auto* pass : this->m_passes) {
	delete pass;
    }

    this->m_passes.clear ();

    // free any gl resources
    glDeleteBuffers (1, &this->m_sceneSpacePosition);
    glDeleteBuffers (1, &this->m_copySpacePosition);
    glDeleteBuffers (1, &this->m_passSpacePosition);
    glDeleteBuffers (1, &this->m_texcoordCopy);
    glDeleteBuffers (1, &this->m_texcoordPass);

    if (this->m_puppetVBO) glDeleteBuffers (1, &this->m_puppetVBO);
    if (this->m_puppetEBO) glDeleteBuffers (1, &this->m_puppetEBO);
    if (this->m_puppetVAO) glDeleteVertexArrays (1, &this->m_puppetVAO);
}

bool CImage::initPuppet () {
    namespace P = WallpaperEngine::Render::Puppet;

    if (!P::MdlParser::parse (
	    this->getAssetLocator (), *this->m_image.model->puppet, this->m_puppetMdl
	)) {
	sLog.error ("Puppet parse failed for image ", this->m_image.id);
	return false;
    }
    if (!this->m_puppetMdl.puppet || this->m_puppetMdl.puppet->bones.empty ()) {
	sLog.error ("Puppet has no bones for image ", this->m_image.id);
	return false;
    }

    // Snapshot the per-image animation layers from the parsed scene description. The struct
    // field name is `id` even though it stores the JSON `animation` field (= the animation's
    // own id) — that's the WPE-format quirk reflected throughout the data model.
    this->m_puppetAnimLayers.clear ();
    this->m_puppetAnimLayers.reserve (this->m_image.animationLayers.size ());
    for (const auto& al : this->m_image.animationLayers) {
	P::PuppetLayer::AnimationLayer layer {};
	layer.id = al->animation;
	layer.rate = al->rate;
	layer.blend = al->blend;
	layer.visible = al->visible->value->getBool ();
	layer.curTime = 0.0;
	this->m_puppetAnimLayers.push_back (layer);
    }

    this->m_puppetLayer = P::PuppetLayer (this->m_puppetMdl.puppet);
    this->m_puppetLayer.prepared (this->m_puppetAnimLayers);
    this->m_boneMatrixUpload.assign (this->m_puppetMdl.puppet->bones.size () * 12, 0.0f);

    // Build interleaved VBO matching WPE's stock vertex layout for skinned meshes:
    //   [pos.xyz][blend_indices.xyzw uint32][weights.xyzw][uv.xy] = 13 × 4 bytes = 52 bytes.
    constexpr GLsizei stride = sizeof (float) * (3 + 4 + 4 + 2);
    const auto& verts = this->m_puppetMdl.vertexs;
    std::vector<uint8_t> vboBuf (verts.size () * stride);
    for (size_t i = 0; i < verts.size (); ++i) {
	uint8_t* dst = vboBuf.data () + i * stride;
	std::memcpy (dst + 0, verts[i].position.data (), sizeof (float) * 3);
	std::memcpy (dst + 12, verts[i].blendIndices.data (), sizeof (uint32_t) * 4);
	std::memcpy (dst + 28, verts[i].weight.data (), sizeof (float) * 4);
	std::memcpy (dst + 44, verts[i].texcoord.data (), sizeof (float) * 2);
    }

    // Indices: file is u16 — keep as u16 on GPU too.
    std::vector<uint16_t> idxBuf;
    idxBuf.reserve (this->m_puppetMdl.indices.size () * 3);
    for (const auto& tri : this->m_puppetMdl.indices) {
	idxBuf.insert (idxBuf.end (), tri.begin (), tri.end ());
    }
    this->m_puppetIndexCount = static_cast<GLsizei> (idxBuf.size ());

    GLint prevVAO = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &prevVAO);

    glGenVertexArrays (1, &this->m_puppetVAO);
    glBindVertexArray (this->m_puppetVAO);

    glGenBuffers (1, &this->m_puppetVBO);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetVBO);
    glBufferData (GL_ARRAY_BUFFER, vboBuf.size (), vboBuf.data (), GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_puppetEBO);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetEBO);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, idxBuf.size () * sizeof (uint16_t), idxBuf.data (), GL_STATIC_DRAW
    );

    // Attribute layout follows what stock WPE shaders expect:
    //   location 0: a_Position (vec3)
    //   location 1: a_BlendIndices (uvec4)
    //   location 2: a_BlendWeights (vec4)
    //   location 3: a_TexCoord (vec2)
    // CPass currently looks attribute names up via glGetAttribLocation each frame, so the
    // location numbers below don't have to match — the bindings just need to match positions
    // in the VBO. We use the same offsets in the geometry-setup callback (where we resolve
    // the live attribute locations from the linked program).
    glBindVertexArray (prevVAO);

    // Build the pass override. SKINNING/BONECOUNT enable the stock WPE shader's bone-weighted
    // vertex transform that the puppet meshes are authored against.
    this->m_puppetPassOverride = std::make_unique<ImageEffectPassOverride> ();
    this->m_puppetPassOverride->combos["SKINNING"] = 1;
    this->m_puppetPassOverride->combos["BONECOUNT"] = static_cast<int> (
	this->m_puppetMdl.puppet->bones.size ()
    );

    sLog.out (
	"Puppet ready for image ", this->m_image.id, ": bones=",
	this->m_puppetMdl.puppet->bones.size (), " anims=",
	this->m_puppetMdl.puppet->anims.size (), " verts=", verts.size (),
	" tris=", this->m_puppetIndexCount / 3
    );
    return true;
}

void CImage::updatePuppetBones () {
    const double dt = static_cast<double> (g_Time - g_TimeLast);

    auto matrices = this->m_puppetLayer.genFrame (dt);

    // Repack glm::mat4 (16 floats column-major) into mat4x3 layout (12 floats column-major).
    // That's: for each of 4 columns, take floats [0,1,2] (skip [3]).
    for (size_t i = 0; i < matrices.size (); ++i) {
	const float* src = &matrices[i][0][0];
	float* dst = this->m_boneMatrixUpload.data () + i * 12;
	for (int col = 0; col < 4; ++col) {
	    dst[col * 3 + 0] = src[col * 4 + 0];
	    dst[col * 3 + 1] = src[col * 4 + 1];
	    dst[col * 3 + 2] = src[col * 4 + 2];
	}
    }
}

void CImage::setup () {
    // do not double-init stuff, that's bad!
    if (this->m_initialized) {
	return;
    }

    // TODO: CHECK ORDER OF THINGS, 2419444134'S ID 27 DEPENDS ON 104'S COMPOSITE_A WHEN OUR LAST RENDER IS ON
    // COMPOSITE_B
    // TODO: SUPPORT PASSTHROUGH (IT'S A SHADER)
    if (this->m_image.model->passthrough) {
		// passthrough images without effects are bad, do not draw them
		if(this->m_image.effects.empty ()) {
			return;
		}

		// Some have attempted to declare effects with visible set to false.
		bool allEffectsInvisible = true;
		for (const auto& cur : this->m_image.effects) {
			if (cur->visible->value->getBool()) {
				allEffectsInvisible = false;
				break;
			}
		}

		if (allEffectsInvisible) {
			return;
		}
    }

    // copy pass to the composite layer. Puppet images get their SKINNING/BONECOUNT combo
    // override and a geometry callback that binds the skinned VAO + uploads bone matrices.
    for (const auto& cur : this->getImage ().model->material->passes) {
	std::optional<std::reference_wrapper<const ImageEffectPassOverride>> passOverride
	    = std::nullopt;
	if (this->m_isPuppet && this->m_puppetPassOverride) {
	    passOverride = std::cref (*this->m_puppetPassOverride);
	}
	auto* pass = new CPass (
	    *this, std::make_shared<FBOProvider> (this), *cur, passOverride, std::nullopt, std::nullopt
	);
	this->m_passes.push_back (pass);

	// Replace the default flat-quad geometry with our skinned mesh.
	if (this->m_isPuppet) {
	    // Build BOTH transform candidates up-front; pick which to pin after setupPasses().
	    //
	    //  - Image-local: vertex (0,0) → FBO center. Range [-size/2, +size/2] → [0, size].
	    //    Y-flip converts puppet's Y-down convention into the FBO's Y-up OpenGL
	    //    convention so the final scene-composite samples it correctly.
	    //
	    //  - Scene-space: T(image_origin_scene) × S(scale, -scale, scale). Used only when
	    //    the puppet pass is also the scene-composite pass (single-pass material with
	    //    no effects). Y-scale is negated for the same Y-down → Y-up reason.
	    const glm::vec2 size = this->getSize ();
	    glm::mat4 mLocal (1.0f);
	    mLocal = glm::translate (mLocal, glm::vec3 (size.x / 2.0f, size.y / 2.0f, 0.0f));
	    // Puppet uses Y-down convention (head at smallest Y, feet at largest Y); FBO uses
	    // OpenGL Y-up. The flip matches the existing copy-pass behaviour (which pairs FBO
	    // upper-left with V=1) so the deformed image lands in the FBO oriented the same way
	    // as the un-deformed source — subsequent flat-quad sampling renders right-side-up.
	    mLocal = glm::scale (mLocal, glm::vec3 (1.0f, -1.0f, 1.0f));
	    this->m_puppetLocalM = mLocal;
	    this->m_puppetLocalMVP = this->m_modelViewProjectionCopy * mLocal;
	    this->m_puppetLocalMVPInverse = glm::inverse (this->m_puppetLocalMVP);

	    const glm::vec3 rawOrigin = this->m_image.origin->value->getVec3 ();
	    const float canvasW = static_cast<float> (this->getScene ().getCamera ().getWidth ());
	    const float canvasH = static_cast<float> (this->getScene ().getCamera ().getHeight ());
	    const glm::vec3 sceneOrigin (
		rawOrigin.x - canvasW / 2.0f,
		canvasH / 2.0f - rawOrigin.y,
		rawOrigin.z
	    );
	    const glm::vec3 scale = this->m_image.scale->value->getVec3 ();
	    glm::mat4 mScene (1.0f);
	    mScene = glm::translate (mScene, sceneOrigin);
	    // Same Y-flip rationale as the image-local M above.
	    mScene = glm::scale (mScene, glm::vec3 (scale.x, -scale.y, scale.z));
	    this->m_puppetSceneM = mScene;
	    this->m_puppetSceneVP
		= this->getScene ().getCamera ().getProjection () * this->getScene ().getCamera ().getLookAt ();
	    this->m_puppetSceneMVP = this->m_puppetSceneVP * mScene;
	    this->m_puppetSceneMVPInverse = glm::inverse (this->m_puppetSceneMVP);

	    pass->setGeometryCallback (
		[this, pass] () {
		    // Force-disable culling for puppet — these are 2D meshes whose triangle
		    // winding can be either way, and our M matrix may flip it. Just don't cull.
		    glDisable (GL_CULL_FACE);
		    // Clear the destination FBO to transparent black so non-mesh pixels are
		    // alpha=0. The FBO is created with an opaque-black clear color, and the
		    // flat-quad copy pass we're replacing would have filled the entire FBO
		    // with the source texture; without that, un-covered pixels stay opaque
		    // black and composite over the scene as a visible rectangular box.
		    GLfloat prevClear[4];
		    glGetFloatv (GL_COLOR_CLEAR_VALUE, prevClear);
		    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
		    glClear (GL_COLOR_BUFFER_BIT);
		    glClearColor (prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
		    // Override the pass's blend func with proper alpha compositing for the
		    // puppet. The default for the first pass is BlendingMode_Normal (GL_ONE,
		    // GL_ZERO = direct overwrite), which destroys whatever was rendered
		    // earlier — so an arm triangle drawn after a torso triangle erases the
		    // torso pixels under the arm's transparent edges. With proper alpha
		    // blending, a transparent arm edge keeps the torso visible behind it.
		    //
		    // The alpha channel uses (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) so the FBO
		    // accumulates correct edge alpha without double-multiplying when later
		    // composited into the scene. (Standard SRC_ALPHA blend on alpha would
		    // shrink edge alpha to alpha² and make outer edges look too transparent
		    // after the second blend in the scene-composite pass.)
		    glEnable (GL_BLEND);
		    glBlendFuncSeparate (
			GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
			GL_ONE,       GL_ONE_MINUS_SRC_ALPHA
		    );
		    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &this->m_puppetSavedVAO);
		    glBindVertexArray (this->m_puppetVAO);
		    this->updatePuppetBones ();
		    const GLuint program = pass->getProgramID ();
		    if (program == 0) return;
		    const GLint loc = glGetUniformLocation (program, "g_Bones");
		    if (loc >= 0 && !this->m_boneMatrixUpload.empty ()) {
			glUniformMatrix4x3fv (
			    loc, static_cast<GLsizei> (this->m_boneMatrixUpload.size () / 12),
			    GL_FALSE, this->m_boneMatrixUpload.data ()
			);
		    }
		    // Re-bind the per-vertex attributes from our interleaved VBO. Stride 52,
		    // matching the layout we built in initPuppet().
		    constexpr GLsizei stride = sizeof (float) * (3 + 4 + 4 + 2);
		    const GLint posLoc = glGetAttribLocation (program, "a_Position");
		    const GLint biLoc = glGetAttribLocation (program, "a_BlendIndices");
		    const GLint bwLoc = glGetAttribLocation (program, "a_BlendWeights");
		    const GLint tcLoc = glGetAttribLocation (program, "a_TexCoord");
		    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetVBO);
		    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetEBO);
		    if (posLoc >= 0) {
			glEnableVertexAttribArray (posLoc);
			glVertexAttribPointer (
			    posLoc, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*> (0)
			);
		    }
		    if (biLoc >= 0) {
			glEnableVertexAttribArray (biLoc);
			glVertexAttribIPointer (
			    biLoc, 4, GL_UNSIGNED_INT, stride, reinterpret_cast<void*> (12)
			);
		    }
		    if (bwLoc >= 0) {
			glEnableVertexAttribArray (bwLoc);
			glVertexAttribPointer (
			    bwLoc, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*> (28)
			);
		    }
		    if (tcLoc >= 0) {
			glEnableVertexAttribArray (tcLoc);
			glVertexAttribPointer (
			    tcLoc, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*> (44)
			);
		    }
		},
		[this] () {
		    glDrawElements (
			GL_TRIANGLES, this->m_puppetIndexCount, GL_UNSIGNED_SHORT, nullptr
		    );
		},
		[this] () { glBindVertexArray (this->m_puppetSavedVAO); }
	    );
	}
    }

    // prepare the passes list
    if (!this->getImage ().effects.empty ()) {
	// generate the effects used by this material
	for (const auto& cur : this->m_image.effects) {
	    // do not add non-visible effects, this might need some adjustements tho as some effects might not be
	    // visible but affect the output of the image...
	    if (!cur->visible->value->getBool ()) {
		continue;
	    }

	    const auto fboProvider = std::make_shared<FBOProvider> (this);

	    // create all the fbos for this effect
	    for (const auto& fbo : cur->effect->fbos) {
		fboProvider->create (*fbo, this->m_texture->getFlags (), this->getSize ());
	    }

	    // TODO: MAKE USE OF ZIP OPERATOR IN BOOST? WAY OVERKILL JUST FOR THIS...

	    auto curEffect = cur->effect->passes.begin ();
	    auto endEffect = cur->effect->passes.end ();
	    auto curOverride = cur->passOverrides.begin ();
	    auto endOverride = cur->passOverrides.end ();

	    for (; curEffect != endEffect; ++curEffect) {
		if (!(*curEffect)->material.has_value ()) {
		    if (!(*curEffect)->command.has_value ()) {
			sLog.error ("Pass without material and command not supported");
			continue;
		    }

		    if (!(*curEffect)->source.has_value ()) {
			sLog.error ("Pass without material and source not supported");
			continue;
		    }

		    if (!(*curEffect)->target.has_value ()) {
			sLog.error ("Pass without material and target not supported");
			continue;
		    }

		    if ((*curEffect)->command != Command_Copy) {
			sLog.error ("Only copy command is supported for pass without material");
			continue;
		    }

		    auto virtualPass
			= std::make_unique<MaterialPass> (MaterialPass { .blending = BlendingMode_Normal,
									 .cullmode = CullingMode_Disable,
									 .depthtest = DepthtestMode_Disabled,
									 .depthwrite = DepthwriteMode_Disabled,
									 .shader = "commands/copy",
									 .textures = { { 0, *(*curEffect)->source } },
									 .combos = {},
									 .constants = {} });

		    const auto& config = *this->m_virtualPassess.emplace_back (std::move (virtualPass));

		    // build a pass for a copy shader
		    this->m_passes.push_back (new CPass (
			*this, fboProvider, config, std::nullopt, std::nullopt, (*curEffect)->target.value ()
		    ));
		} else {
		    for (auto& pass : (*curEffect)->material.value ()->passes) {
			const auto override = curOverride != endOverride
			    ? **curOverride
			    : std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (std::nullopt);
			const auto target = (*curEffect)->target.has_value ()
			    ? *(*curEffect)->target
			    : std::optional<std::reference_wrapper<std::string>> (std::nullopt);

			this->m_passes.push_back (
			    new CPass (*this, fboProvider, *pass, override, (*curEffect)->binds, target)
			);
		    }

		    if (curOverride != endOverride) {
			++curOverride;
		    }
		}
	    }
	}
    }

    // extra render pass if there's any blending to be done
    if (this->m_image.colorBlendMode > 0) {
	this->m_materials.colorBlending.material
	    = MaterialParser::load (this->getScene ().getScene ().project, "materials/util/effectpassthrough.json");
	this->m_materials.colorBlending.override = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
            .id = -1,
            .combos = {
                {"BLENDMODE", this->m_image.colorBlendMode},
            },
            .constants = {},
            .textures = {},
        });

	this->m_passes.push_back (new CPass (
	    *this, std::make_shared<FBOProvider> (this), **this->m_materials.colorBlending.material->passes.begin (),
	    *this->m_materials.colorBlending.override, std::nullopt, std::nullopt
	));
    }

    // if there's more than one pass the blendmode has to be moved from the beginning to the end
    if (this->m_passes.size () > 1) {
	const auto first = this->m_passes.begin ();
	const auto last = this->m_passes.rbegin ();

	(*last)->setBlendingMode ((*first)->getBlendingMode ());
	(*first)->setBlendingMode (BlendingMode_Normal);
    }

    CRenderable::setup ();

    this->setupPasses ();

    // setupPasses() pinned each pass's matrix pointers to CImage's flat-quad matrices.
    // Re-pin the FIRST puppet pass with the appropriate transform:
    //   - If it's the only pass (no effects), it draws direct to scene → use scene-space M.
    //   - If there are effects, it draws to the image FBO → use image-local M, and the
    //     existing pipeline handles scene-composite via the final pass.
    if (this->m_isPuppet && !this->m_passes.empty ()) {
	auto* p = this->m_passes.front ();
	const bool singlePass = (this->m_passes.size () == 1);
	if (singlePass) {
	    p->setModelMatrix (&this->m_puppetSceneM);
	    p->setViewProjectionMatrix (&this->m_puppetSceneVP);
	    p->setModelViewProjectionMatrix (&this->m_puppetSceneMVP);
	    p->setModelViewProjectionMatrixInverse (&this->m_puppetSceneMVPInverse);
	} else {
	    p->setModelMatrix (&this->m_puppetLocalM);
	    p->setViewProjectionMatrix (&this->m_modelViewProjectionCopy);
	    p->setModelViewProjectionMatrix (&this->m_puppetLocalMVP);
	    p->setModelViewProjectionMatrixInverse (&this->m_puppetLocalMVPInverse);
	}
    }

    this->m_initialized = true;
}

void CImage::setupPasses () {
    // do a pass on everything and setup proper inputs and values
    std::shared_ptr<const CFBO> drawTo = this->m_currentMainFBO;
    std::shared_ptr<const TextureProvider> asInput = this->getTexture ();
    GLuint texcoord = this->getTexCoordCopy ();

    auto cur = this->m_passes.begin ();
    auto end = this->m_passes.end ();
    bool first = true;

    for (; cur != end; ++cur) {
	// TODO: PROPERLY CHECK EFFECT'S VISIBILITY AND TAKE IT INTO ACCOUNT
	// TODO: THIS REQUIRES ON-THE-FLY EVALUATION OF EFFECTS VISIBILITY TO FIGURE OUT
	// TODO: WHICH ONE IS THE LAST + A FEW OTHER THINGS
	Effects::CPass* pass = *cur;
	std::shared_ptr<const CFBO> prevDrawTo = drawTo;
	GLuint spacePosition = (first) ? this->getCopySpacePosition () : this->getPassSpacePosition ();
	const glm::mat4* projection = (first) ? &this->m_modelViewProjectionCopy : &this->m_modelViewProjectionPass;
	const glm::mat4* inverseProjection
	    = (first) ? &this->m_modelViewProjectionCopyInverse : &this->m_modelViewProjectionPassInverse;
	first = false;

	pass->setModelMatrix (&this->m_modelMatrix);
	pass->setViewProjectionMatrix (&this->m_viewProjectionMatrix);

	// set viewport and target texture if needed
	if (pass->getTarget ().has_value ()) {
	    // setup target texture
	    std::string target = pass->getTarget ().value ();
	    drawTo = pass->getFBOProvider ()->find (target);
	    // spacePosition = this->getPassSpacePosition ();

	    // not a local fbo, try to find a scene fbo with the same name
	    if (drawTo == nullptr) {
		// this one throws if no fbo was found
		drawTo = this->getScene ().findFBO (target);
	    }
	}
	// determine if it's the last element in the list as this is a screen-copy-like process
	// TODO: PROPERLY CHECK IF THIS IS ALL THAT'S NEEDED
	else if (std::next (cur) == end && this->getImage ().visible->value->getBool ()) {
	    // TODO: PROPERLY CHECK EFFECT'S VISIBILITY AND TAKE IT INTO ACCOUNT
	    spacePosition = this->getSceneSpacePosition ();
	    drawTo = this->getScene ().getFBO ();
	    projection = &this->m_modelViewProjectionScreen;
	    inverseProjection = &this->m_modelViewProjectionScreenInverse;
	}

	pass->setDestination (drawTo);
	pass->setInput (asInput);
	pass->setPosition (spacePosition);
	pass->setTexCoord (texcoord);
	pass->setModelViewProjectionMatrix (projection);
	pass->setModelViewProjectionMatrixInverse (inverseProjection);

	texcoord = this->getTexCoordPass ();
	drawTo = prevDrawTo;

	if (!pass->getTarget ().has_value ()) {
	    this->pinpongFramebuffer (&drawTo, &asInput);
	}
    }
}

void CImage::pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput) {
    // temporarily store FBOs used
    std::shared_ptr<const CFBO> currentMainFBO = this->m_currentMainFBO;
    std::shared_ptr<const CFBO> currentSubFBO = this->m_currentSubFBO;

    if (drawTo != nullptr) {
	*drawTo = currentSubFBO;
    }
    if (asInput != nullptr) {
	*asInput = currentMainFBO;
    }

    // swap the FBOs
    this->m_currentMainFBO = currentSubFBO;
    this->m_currentSubFBO = currentMainFBO;
}

void CImage::render () {
    // do not try to render something that did not initialize successfully
    // non-visible materials do need to be rendered
    if (!this->m_initialized) {
	return;
    }

    // TODO: DO NOT DRAW IMAGES THAT ARE NOT VISIBLE AND NOTHING DEPENDS ON THEM

    glColorMask (true, true, true, true);

    // Always update screen transform (handles rotation + parallax dynamically)
    this->updateScreenSpacePosition ();

#if !NDEBUG
    std::string str = "Rendering ";

    if (this->getScene ().getScene ().camera.bloom.enabled->value->getBool () && this->getId () == -1) {
	str += "bloom";
    } else {
	str += this->getImage ().name + " (" + std::to_string (this->getId ()) + ", "
	    + this->getImage ().model->material->filename + ")";
    }

    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    auto cur = this->m_passes.begin ();

    for (const auto end = this->m_passes.end (); cur != end; ++cur) {
	if (std::next (cur) == end) {
	    glColorMask (true, true, true, false);
	}

	(*cur)->render ();
    }

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}

const float& CImage::getBrightness () const { return this->m_image.brightness; }

const float& CImage::getUserAlpha () const { return this->m_image.alpha->value->getFloat (); }

const float& CImage::getAlpha () const { return this->m_image.alpha->value->getFloat (); }

const glm::vec3& CImage::getColor () const { return this->m_image.color->value->getVec3 (); }

const glm::vec4& CImage::getColor4 () const { return this->m_image.color->value->getVec4 (); }

const glm::vec3& CImage::getCompositeColor () const { return this->m_image.color->value->getVec3 (); }

void CImage::updateScreenSpacePosition () {
    // Build rotation from angles (already in radians from scene.json — see CParticle.cpp:2119)
    // Negate X and Z rotations to account for Y-flipped coordinate system (CParticle.cpp:2120)
    glm::vec3 angles = this->getImage ().angles->value->getVec3 ();
    glm::mat4 rotModel = glm::mat4 (1.0f);
    if (angles.x != 0.0f || angles.y != 0.0f || angles.z != 0.0f) {
	rotModel = glm::translate (rotModel, this->m_sceneCenter);
	rotModel = glm::rotate (rotModel, -angles.z, glm::vec3 (0.0f, 0.0f, 1.0f));
	rotModel = glm::rotate (rotModel, angles.y, glm::vec3 (0.0f, 1.0f, 0.0f));
	rotModel = glm::rotate (rotModel, -angles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
	rotModel = glm::translate (rotModel, -this->m_sceneCenter);
    }

    glm::mat4 mvp = this->getScene ().getCamera ().getProjection ()
		   * this->getScene ().getCamera ().getLookAt ()
		   * rotModel;

    // Apply parallax displacement if enabled
    if (this->getScene ().getScene ().camera.parallax.enabled
	&& !this->getImage ().model->fullscreen
	&& !this->getScene ().getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	const double parallaxAmount = this->getScene ().getScene ().camera.parallax.amount->value->getFloat ();
	const glm::vec2 depth = this->getImage ().parallaxDepth->value->getVec2 ();
	const glm::vec2* displacement = this->getScene ().getParallaxDisplacement ();
	float x = (depth.x + parallaxAmount) * displacement->x * this->getSize ().x;
	float y = (depth.y + parallaxAmount) * displacement->y * this->getSize ().y;
	mvp = glm::translate (mvp, { x, y, 0.0f });
    }

    this->m_modelViewProjectionScreen = mvp;
    this->m_modelViewProjectionScreenInverse = glm::inverse (mvp);
}

const Image& CImage::getImage () const { return this->m_image; }

const std::vector<CEffect*>& CImage::getEffects () const { return this->m_effects; }

const Effects::CMaterial* CImage::getMaterial () const { return this->m_material; }

glm::vec2 CImage::getSize () const {
    if (this->m_texture == nullptr) {
	return this->getImage ().size;
    }

    return { this->m_texture->getRealWidth (), this->m_texture->getRealHeight () };
}

GLuint CImage::getSceneSpacePosition () const { return this->m_sceneSpacePosition; }

GLuint CImage::getCopySpacePosition () const { return this->m_copySpacePosition; }

GLuint CImage::getPassSpacePosition () const { return this->m_passSpacePosition; }

GLuint CImage::getTexCoordCopy () const { return this->m_texcoordCopy; }

GLuint CImage::getTexCoordPass () const { return this->m_texcoordPass; }

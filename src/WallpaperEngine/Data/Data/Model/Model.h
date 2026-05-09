#pragma once

#include <optional>
#include <string>

#include <glm/vec2.hpp>

#include "Types.h"

namespace WallpaperEngine::Data::Model {
// TODO: FIND A BETTER NAMING SO THIS DOESN'T COLLIDE WITH THE NAMESPACE ITSELF
struct ModelStruct {
    /** The filename of the model */
    std::string filename;
    /** The material used for this model */
    MaterialUniquePtr material;
    /** Whether this model is a solid layer */
    bool solidlayer;
    /** Whether this model is a fullscreen layer */
    bool fullscreen;
    /** Whether this model is a passthrough layer */
    bool passthrough;
    /** Whether this models's size should be determined automatically or not */
    bool autosize;
    /** Whether this models's padding should be disabled or not */
    bool nopadding;
    /** Not sure what's used for */
    std::optional<int> width;
    /** Not sure what's used for */
    std::optional<int> height;
    /** Model file for puppet */
    std::optional<std::string> puppet;
    /** Per-image origin offset in canvas pixels, used by autosize images authored with
     *  asymmetrically-cropped textures (e.g. Miku's head image whose face is offset
     *  from the texture center). The WPE editor exposes this via the
     *  "Align with background" tool. Applied as an additional translation to the
     *  image's rendered position. */
    glm::vec2 cropoffset = { 0.0f, 0.0f };
};
} // namespace WallpaperEngine::Data::Model
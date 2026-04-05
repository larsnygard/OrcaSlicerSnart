#ifndef slic3r_MeshTexturizer_hpp_
#define slic3r_MeshTexturizer_hpp_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "TriangleMesh.hpp"

namespace Slic3r {

/// Greyscale texture image for displacement mapping.
/// Pixel values are 8-bit: 0 = deepest inward, 128 = neutral, 255 = outward.
struct TextureImage {
    std::vector<uint8_t> data; // row-major greyscale pixels
    size_t               width  = 0;
    size_t               height = 0;

    bool valid() const { return !data.empty() && width > 0 && height > 0; }

    /// Bilinear-sample the texture at normalised UV coordinates [0..1].
    /// Returns a value in [0..1].
    float sample(float u, float v) const;
};

/// Parameters for mesh texturisation (UV mapping + displacement).
struct MeshTexturizerParams {
    /// UV projection mode, mirroring the stlTexturizer projection modes.
    enum class ProjectionMode : int {
        PlanarXY    = 0,
        PlanarXZ    = 1,
        PlanarYZ    = 2,
        Cylindrical = 3,
        Spherical   = 4,
        Triplanar   = 5,
        Cubic       = 6,
    };

    ProjectionMode mode = ProjectionMode::Triplanar;

    /// Independent UV scale (0.05 – 10).
    float scale_u = 1.0f;
    float scale_v = 1.0f;

    /// UV offset.
    float offset_u = 0.0f;
    float offset_v = 0.0f;

    /// Texture rotation in degrees (applied before projection).
    float rotation = 0.0f;

    /// Displacement amplitude in mm.
    /// 50 % grey = no displacement, white = +amplitude/2, black = -amplitude/2.
    float amplitude = 1.0f;

    /// Adaptive subdivision: maximum edge length in mm.
    /// Edges longer than this are split until all edges are within this budget.
    float max_edge_length = 0.5f;

    /// After displacement, decimate to this triangle count.
    /// 0 = keep all triangles (no decimation).
    uint32_t target_triangle_count = 0;

    /// Angle masking: suppress displacement on nearly-horizontal top faces
    /// (angle between face normal and +Z smaller than this threshold, deg).
    float top_angle_limit = 0.0f;

    /// Angle masking: suppress displacement on nearly-horizontal bottom faces.
    float bottom_angle_limit = 0.0f;

    /// Seam-blend strength for Cubic / Cylindrical / Spherical modes [0..1].
    float seam_blend = 0.5f;

    /// Cap-angle threshold for Cylindrical mode (degrees).
    float cap_angle = 20.0f;
};

/// Utility class (no instances) for applying surface texture displacement.
class MeshTexturizer {
public:
    MeshTexturizer() = delete;

    /// Load a greyscale displacement texture from a PNG or JPEG file.
    /// Returns an invalid TextureImage on failure.
    static TextureImage load_texture(const std::string &path);

    /// Apply texture displacement to @p mesh using the given @p texture and
    /// @p params.  Returns a new displaced TriangleMesh.
    ///
    /// @param throw_on_cancel  Called periodically; should throw to abort.
    /// @param statusfn         Receives progress values 0–100.
    static TriangleMesh apply(const TriangleMesh &             mesh,
                              const TextureImage &             texture,
                              const MeshTexturizerParams &     params,
                              std::function<void(void)>        throw_on_cancel = nullptr,
                              std::function<void(int)>         statusfn        = nullptr);
};

} // namespace Slic3r

#endif // slic3r_MeshTexturizer_hpp_

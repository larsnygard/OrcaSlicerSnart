/// MeshTexturizer.cpp
/// Implements surface displacement texturing, ported from the JavaScript
/// stlTexturizer / BumpMesh tool (https://github.com/CNCKitchen/stlTexturizer).
///
/// Pipeline:
///   1. Adaptive edge subdivision until every edge ≤ max_edge_length.
///   2. Per-vertex UV projection (7 modes).
///   3. Bilinear texture sampling → displacement value.
///   4. Move each vertex along its (smooth, area-weighted) normal.
///   5. Optional QEM decimation to a target triangle count.

#include "MeshTexturizer.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include "NormalUtils.hpp"
#include "Point.hpp"
#include "QuadricEdgeCollapse.hpp"
#include "TriangleMesh.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static constexpr float TEX_PI = static_cast<float>(M_PI);

// PNG decoding (already in libslic3r)
#include "PNGReadWrite.hpp"

// JPEG decoding via libjpeg (already linked in libslic3r)
#include <jpeglib.h>

namespace Slic3r {

// ──────────────────────────────────────────────────────────────────────────────
// TextureImage::sample  —  bilinear interpolation
// ──────────────────────────────────────────────────────────────────────────────
float TextureImage::sample(float u, float v) const
{
    if (!valid())
        return 0.5f;

    // Tile
    u = u - std::floor(u);
    v = v - std::floor(v);

    const float fx = u * static_cast<float>(width  - 1);
    const float fy = v * static_cast<float>(height - 1);

    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, static_cast<int>(width)  - 1);
    const int y1 = std::min(y0 + 1, static_cast<int>(height) - 1);

    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    auto px = [&](int x, int y) -> float {
        return data[y * width + x] / 255.0f;
    };

    const float top    = px(x0, y0) * (1.f - tx) + px(x1, y0) * tx;
    const float bottom = px(x0, y1) * (1.f - tx) + px(x1, y1) * tx;
    return top * (1.f - ty) + bottom * ty;
}

// ──────────────────────────────────────────────────────────────────────────────
// Texture loading
// ──────────────────────────────────────────────────────────────────────────────
namespace {

/// Convert a 3-channel RGB pixel buffer to greyscale (BT.601 luminance).
static std::vector<uint8_t> rgb_to_greyscale(const std::vector<uint8_t> &rgb, size_t n_pixels)
{
    std::vector<uint8_t> grey(n_pixels);
    for (size_t i = 0; i < n_pixels; ++i) {
        const unsigned r = rgb[i * 3 + 0];
        const unsigned g = rgb[i * 3 + 1];
        const unsigned b = rgb[i * 3 + 2];
        // BT.601 coefficients scaled by 256: R*77 + G*150 + B*29
        grey[i] = static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8u);
    }
    return grey;
}

/// Read entire file into a byte vector.
static std::vector<uint8_t> read_file(const std::string &path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    if (sz <= 0)
        return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char *>(buf.data()), sz);
    return buf;
}

static TextureImage load_png(const std::vector<uint8_t> &buf)
{
    TextureImage img;
    // Try greyscale first, then colour.
    {
        png::ImageGreyscale grey;
        png::ReadBuf         rb{buf.data(), buf.size()};
        if (png::decode_png(rb, grey)) {
            img.width  = grey.cols;
            img.height = grey.rows;
            img.data   = std::move(grey.buf);
            return img;
        }
    }
    {
        png::ImageColorscale colour;
        png::ReadBuf          rb{buf.data(), buf.size()};
        if (png::decode_colored_png(rb, colour)) {
            img.width  = colour.cols;
            img.height = colour.rows;
            const int bpp = colour.bytes_per_pixel;
            const size_t n = img.width * img.height;
            if (bpp == 1) {
                img.data = colour.buf;
            } else if (bpp >= 3) {
                img.data = rgb_to_greyscale(colour.buf, n);
            }
            return img;
        }
    }
    return img; // invalid
}

static TextureImage load_jpeg(const std::vector<uint8_t> &buf)
{
    TextureImage img;

    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    jpeg_mem_src(&cinfo,
                 const_cast<unsigned char *>(buf.data()),
                 static_cast<unsigned long>(buf.size()));

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return img;
    }

    // Request greyscale output regardless of input colour space.
    cinfo.out_color_space = JCS_GRAYSCALE;
    jpeg_start_decompress(&cinfo);

    img.width  = cinfo.output_width;
    img.height = cinfo.output_height;
    img.data.resize(img.width * img.height);

    std::vector<uint8_t> row(img.width);
    uint8_t *             row_ptr = row.data();
    size_t                row_idx = 0;

    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
        std::memcpy(img.data.data() + row_idx * img.width, row.data(), img.width);
        ++row_idx;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return img;
}

} // anonymous namespace

TextureImage MeshTexturizer::load_texture(const std::string &path)
{
    const auto buf = read_file(path);
    if (buf.empty()) {
        BOOST_LOG_TRIVIAL(error) << "MeshTexturizer: cannot read file " << path;
        return {};
    }

    // Detect format by magic bytes.
    const bool is_png  = buf.size() >= 8 &&
                         buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' && buf[3] == 'G';
    const bool is_jpeg = buf.size() >= 2 && buf[0] == 0xFF && buf[1] == 0xD8;

    TextureImage img;
    if (is_png)
        img = load_png(buf);
    else if (is_jpeg)
        img = load_jpeg(buf);
    else
        BOOST_LOG_TRIVIAL(warning) << "MeshTexturizer: unknown image format for " << path;

    if (!img.valid())
        BOOST_LOG_TRIVIAL(error) << "MeshTexturizer: failed to decode " << path;

    return img;
}

// ──────────────────────────────────────────────────────────────────────────────
// Adaptive edge subdivision
// ──────────────────────────────────────────────────────────────────────────────
namespace {

struct Bounds {
    Vec3f min_pt, max_pt, size_pt, center;
};

static Bounds compute_bounds(const indexed_triangle_set &its)
{
    Bounds b;
    if (its.vertices.empty()) {
        b.min_pt = b.max_pt = b.size_pt = b.center = Vec3f::Zero();
        return b;
    }
    b.min_pt = its.vertices[0];
    b.max_pt = its.vertices[0];
    for (const auto &v : its.vertices) {
        b.min_pt = b.min_pt.cwiseMin(v);
        b.max_pt = b.max_pt.cwiseMax(v);
    }
    b.size_pt = b.max_pt - b.min_pt;
    b.center  = 0.5f * (b.min_pt + b.max_pt);
    return b;
}

/// Midpoint of two vertices; result is added to @p positions if not already
/// present, and its index returned via @p cache.
static int get_or_add_midpoint(
    std::vector<Vec3f> &       positions,
    std::map<std::pair<int, int>, int> &cache,
    int                        a,
    int                        b)
{
    if (a > b) std::swap(a, b);
    auto key = std::make_pair(a, b);
    auto it  = cache.find(key);
    if (it != cache.end())
        return it->second;

    const int idx = static_cast<int>(positions.size());
    positions.push_back(0.5f * (positions[a] + positions[b]));
    cache[key] = idx;
    return idx;
}

/// One subdivision pass: split every edge whose squared length > maxSq.
/// Returns true if any edge was split.
static bool subdivide_pass(indexed_triangle_set &its, float max_edge_length)
{
    const float maxSq = max_edge_length * max_edge_length;

    // Step 1: globally mark edges that need splitting.
    auto edge_len_sq = [&](int a, int b) {
        return (its.vertices[a] - its.vertices[b]).squaredNorm();
    };

    // We need a consistent per-edge split decision to avoid T-junctions.
    // Key: (min_idx, max_idx) → split?
    std::map<std::pair<int, int>, bool> split_flag;
    for (const auto &tri : its.indices) {
        for (int e = 0; e < 3; ++e) {
            const int a = tri[e], b = tri[(e + 1) % 3];
            const int lo = std::min(a, b), hi = std::max(a, b);
            auto key = std::make_pair(lo, hi);
            if (split_flag.find(key) == split_flag.end())
                split_flag[key] = edge_len_sq(a, b) > maxSq;
        }
    }

    bool any = false;
    for (auto &[k, v] : split_flag)
        if (v) { any = true; break; }
    if (!any)
        return false;

    // Step 2: rebuild index list.
    std::map<std::pair<int, int>, int>  midcache;
    std::vector<Vec3i32>                 new_indices;
    new_indices.reserve(its.indices.size() * 2);

    for (const auto &tri : its.indices) {
        const int a = tri[0], b = tri[1], c = tri[2];
        const bool sAB = split_flag[{std::min(a,b), std::max(a,b)}];
        const bool sBC = split_flag[{std::min(b,c), std::max(b,c)}];
        const bool sCA = split_flag[{std::min(c,a), std::max(c,a)}];
        const int  n   = (sAB ? 1 : 0) + (sBC ? 1 : 0) + (sCA ? 1 : 0);

        if (n == 0) {
            new_indices.push_back(tri);
        } else if (n == 3) {
            // Classic 1→4 split
            const int mAB = get_or_add_midpoint(its.vertices, midcache, a, b);
            const int mBC = get_or_add_midpoint(its.vertices, midcache, b, c);
            const int mCA = get_or_add_midpoint(its.vertices, midcache, c, a);
            new_indices.push_back({a, mAB, mCA});
            new_indices.push_back({mAB, b, mBC});
            new_indices.push_back({mCA, mBC, c});
            new_indices.push_back({mAB, mBC, mCA});
        } else if (n == 1) {
            // Split single longest edge → 2 triangles
            if (sAB) {
                const int m = get_or_add_midpoint(its.vertices, midcache, a, b);
                new_indices.push_back({a, m, c});
                new_indices.push_back({m, b, c});
            } else if (sBC) {
                const int m = get_or_add_midpoint(its.vertices, midcache, b, c);
                new_indices.push_back({a, b, m});
                new_indices.push_back({a, m, c});
            } else {
                const int m = get_or_add_midpoint(its.vertices, midcache, c, a);
                new_indices.push_back({a, b, m});
                new_indices.push_back({m, b, c});
            }
        } else { // n == 2
            // Fan split from the vertex opposite the un-split edge
            if (!sAB) {
                const int mBC = get_or_add_midpoint(its.vertices, midcache, b, c);
                const int mCA = get_or_add_midpoint(its.vertices, midcache, c, a);
                new_indices.push_back({a, b, mBC});
                new_indices.push_back({a, mBC, mCA});
                new_indices.push_back({mCA, mBC, c});
            } else if (!sBC) {
                const int mAB = get_or_add_midpoint(its.vertices, midcache, a, b);
                const int mCA = get_or_add_midpoint(its.vertices, midcache, c, a);
                new_indices.push_back({a, mAB, mCA});
                new_indices.push_back({mAB, b, c});
                new_indices.push_back({mAB, c, mCA});
            } else {
                const int mAB = get_or_add_midpoint(its.vertices, midcache, a, b);
                const int mBC = get_or_add_midpoint(its.vertices, midcache, b, c);
                new_indices.push_back({a, mAB, c});
                new_indices.push_back({mAB, b, mBC});
                new_indices.push_back({mAB, mBC, c});
            }
        }
    }

    its.indices = std::move(new_indices);
    return true;
}

static constexpr size_t MAX_SUBDIVISION_TRIANGLES = 10'000'000;

static indexed_triangle_set subdivide(const indexed_triangle_set &input,
                                      float                        max_edge_length)
{
    indexed_triangle_set its = input;
    for (int iter = 0; iter < 14; ++iter) {
        if (its.indices.size() >= MAX_SUBDIVISION_TRIANGLES)
            break;
        if (!subdivide_pass(its, max_edge_length))
            break;
    }
    return its;
}

// ──────────────────────────────────────────────────────────────────────────────
// UV projection
// ──────────────────────────────────────────────────────────────────────────────

/// Apply rotation + scale + offset to raw UV.
static std::pair<float, float> apply_transform(
    float u, float v,
    float scale_u, float scale_v,
    float offset_u, float offset_v,
    float rot_rad)
{
    if (rot_rad != 0.f) {
        const float cu = std::cos(rot_rad);
        const float su = std::sin(rot_rad);
        const float ru = cu * u - su * v;
        const float rv = su * u + cu * v;
        u = ru; v = rv;
    }
    u = u * scale_u + offset_u;
    v = v * scale_v + offset_v;
    return {u, v};
}

struct UVSample { float u, v, w; };
using UVResult = std::vector<UVSample>; // blended multi-sample for triplanar modes

static UVResult compute_uv(
    const Vec3f &             pos,
    const Vec3f &             normal,
    const MeshTexturizerParams &params,
    const Bounds &            bnd)
{
    const float md = std::max({bnd.size_pt.x(), bnd.size_pt.y(), bnd.size_pt.z(), 1e-6f});
    const float rot_rad = params.rotation * TEX_PI / 180.f;

    auto T = [&](float u, float v) -> UVSample {
        auto [tu, tv] = apply_transform(u, v,
                                        params.scale_u, params.scale_v,
                                        params.offset_u, params.offset_v,
                                        rot_rad);
        return {tu, tv, 1.f};
    };

    using Mode = MeshTexturizerParams::ProjectionMode;
    switch (params.mode) {
    case Mode::PlanarXY:
        return {T((pos.x() - bnd.min_pt.x()) / md,
                  (pos.y() - bnd.min_pt.y()) / md)};

    case Mode::PlanarXZ:
        return {T((pos.x() - bnd.min_pt.x()) / md,
                  (pos.z() - bnd.min_pt.z()) / md)};

    case Mode::PlanarYZ:
        return {T((pos.y() - bnd.min_pt.y()) / md,
                  (pos.z() - bnd.min_pt.z()) / md)};

    case Mode::Cylindrical: {
        const float rx = pos.x() - bnd.center.x();
        const float ry = pos.y() - bnd.center.y();
        const float theta = std::atan2(ry, rx);
        const float u_raw = theta / (2.f * TEX_PI) + 0.5f;
        const float r_xy  = std::max(bnd.size_pt.x(), bnd.size_pt.y()) * 0.5f;
        const float circ  = 2.f * TEX_PI * std::max(r_xy, 1e-6f);
        const float v_side = (pos.z() - bnd.min_pt.z()) / circ;
        const float cap_thr = std::cos(params.cap_angle * TEX_PI / 180.f);
        const float abs_nz  = std::abs(normal.z());
        const float cap_w   = std::max(0.f, std::min(1.f,
            (abs_nz - (cap_thr - 0.1f)) / 0.2f));

        auto s_side = T(u_raw, v_side);
        if (cap_w <= 0.f)
            return {s_side};

        const float u_cap = rx / circ + 0.5f;
        const float v_cap = ry / circ + 0.5f;
        auto s_cap = T(u_cap, v_cap);

        if (cap_w >= 1.f)
            return {s_cap};

        s_side.w = 1.f - cap_w;
        s_cap.w  = cap_w;
        return {s_side, s_cap};
    }

    case Mode::Spherical: {
        const Vec3f r = pos - bnd.center;
        const float len = r.norm();
        const float phi   = std::acos(std::max(-1.f, std::min(1.f,
                                r.z() / std::max(len, 1e-6f))));
        const float theta = std::atan2(r.y(), r.x());
        return {T(theta / (2.f * TEX_PI) + 0.5f, phi / TEX_PI)};
    }

    case Mode::Triplanar: {
        // Blend three planar projections weighted by abs(normal) components.
        const float ax = std::abs(normal.x());
        const float ay = std::abs(normal.y());
        const float az = std::abs(normal.z());
        const float sum = ax + ay + az + 1e-6f;
        const float wx = ax / sum, wy = ay / sum, wz = az / sum;

        auto sYZ = T((pos.y() - bnd.min_pt.y()) / md,
                     (pos.z() - bnd.min_pt.z()) / md);
        auto sXZ = T((pos.x() - bnd.min_pt.x()) / md,
                     (pos.z() - bnd.min_pt.z()) / md);
        auto sXY = T((pos.x() - bnd.min_pt.x()) / md,
                     (pos.y() - bnd.min_pt.y()) / md);
        sYZ.w = wx; sXZ.w = wy; sXY.w = wz;
        return {sYZ, sXZ, sXY};
    }

    case Mode::Cubic:
    default: {
        // Dominant-axis projection with optional blend.
        const float ax = std::abs(normal.x());
        const float ay = std::abs(normal.y());
        const float az = std::abs(normal.z());
        const float blend = params.seam_blend;

        auto sYZ = T(normal.x() < 0 ?
                         -(pos.y() - bnd.min_pt.y()) / md :
                          (pos.y() - bnd.min_pt.y()) / md,
                     (pos.z() - bnd.min_pt.z()) / md);
        auto sXZ = T(normal.y() > 0 ?
                         -(pos.x() - bnd.min_pt.x()) / md :
                          (pos.x() - bnd.min_pt.x()) / md,
                     (pos.z() - bnd.min_pt.z()) / md);
        auto sXY = T(normal.z() < 0 ?
                         -(pos.x() - bnd.min_pt.x()) / md :
                          (pos.x() - bnd.min_pt.x()) / md,
                     (pos.y() - bnd.min_pt.y()) / md);

        if (blend <= 0.001f) {
            if (ax >= ay && ax >= az) return {sYZ};
            if (ay >= az)             return {sXZ};
            return {sXY};
        }

        // Smooth blend
        const float sum = ax + ay + az + 1e-6f;
        sYZ.w = ax / sum; sXZ.w = ay / sum; sXY.w = az / sum;
        return {sYZ, sXZ, sXY};
    }
    }
}

/// Sample a TextureImage using the blended multi-sample UV result.
static float sample_uv(const TextureImage &tex, const UVResult &uvs)
{
    float result = 0.f;
    for (const auto &s : uvs)
        result += tex.sample(s.u, s.v) * s.w;
    return result;
}

// ──────────────────────────────────────────────────────────────────────────────
// Angle masking helper
// ──────────────────────────────────────────────────────────────────────────────

/// Returns true if the face with given normal should be masked (no displacement).
static bool is_face_masked(const Vec3f &face_normal, const MeshTexturizerParams &params)
{
    if (params.top_angle_limit <= 0.f && params.bottom_angle_limit <= 0.f)
        return false;
    const float nz = face_normal.z();
    const float face_angle_deg = std::acos(std::abs(nz)) * 180.f / TEX_PI;
    if (nz >= 0 && params.top_angle_limit > 0.f && face_angle_deg <= params.top_angle_limit)
        return true;
    if (nz < 0 && params.bottom_angle_limit > 0.f && face_angle_deg <= params.bottom_angle_limit)
        return true;
    return false;
}

} // anonymous namespace

// ──────────────────────────────────────────────────────────────────────────────
// MeshTexturizer::apply
// ──────────────────────────────────────────────────────────────────────────────
TriangleMesh MeshTexturizer::apply(
    const TriangleMesh &         in_mesh,
    const TextureImage &         texture,
    const MeshTexturizerParams & params,
    std::function<void(void)>    throw_on_cancel,
    std::function<void(int)>     statusfn)
{
    auto report = [&](int pct) { if (statusfn) statusfn(pct); };
    auto check_cancel = [&]() { if (throw_on_cancel) throw_on_cancel(); };

    if (!texture.valid()) {
        BOOST_LOG_TRIVIAL(error) << "MeshTexturizer::apply: invalid texture";
        return in_mesh;
    }

    report(0);
    check_cancel();

    // ── 1. Adaptive subdivision ──────────────────────────────────────────────
    BOOST_LOG_TRIVIAL(info) << "MeshTexturizer: subdividing mesh ("
                            << in_mesh.its.indices.size() << " triangles) …";
    indexed_triangle_set its = subdivide(in_mesh.its, params.max_edge_length);

    BOOST_LOG_TRIVIAL(info) << "MeshTexturizer: after subdivision: "
                            << its.indices.size() << " triangles, "
                            << its.vertices.size() << " vertices";
    report(30);
    check_cancel();

    // ── 2. Compute bounds ────────────────────────────────────────────────────
    const Bounds bnd = compute_bounds(its);

    // ── 3. Compute per-vertex smooth normals (area-weighted average) ─────────
    const auto normals = NormalUtils::create_normals(its, NormalUtils::VertexNormalType::AverageNeighbor);

    report(40);
    check_cancel();

    // ── 4. Compute face normals for angle masking ────────────────────────────
    const auto face_normals = NormalUtils::create_triangle_normals(its);

    // ── 5. For each face, determine masking ──────────────────────────────────
    std::vector<bool> face_masked(its.indices.size(), false);
    if (params.top_angle_limit > 0.f || params.bottom_angle_limit > 0.f) {
        for (size_t f = 0; f < its.indices.size(); ++f)
            face_masked[f] = is_face_masked(face_normals[f], params);
    }

    // Build per-vertex masked-fraction:
    // If all adjacent faces are masked, vertex displacement = 0.
    // If none are masked, displacement = full.
    // Boundary vertices blend proportionally.
    struct VertexMask {
        float masked_area = 0.f;
        float total_area  = 0.f;
    };
    std::vector<VertexMask> vmask(its.vertices.size());

    for (size_t f = 0; f < its.indices.size(); ++f) {
        const auto &tri = its.indices[f];
        // Triangle area
        const Vec3f e1 = its.vertices[tri[1]] - its.vertices[tri[0]];
        const Vec3f e2 = its.vertices[tri[2]] - its.vertices[tri[0]];
        const float area = 0.5f * e1.cross(e2).norm();
        for (int v = 0; v < 3; ++v) {
            const int vi = tri[v];
            vmask[vi].total_area += area;
            if (face_masked[f])
                vmask[vi].masked_area += area;
        }
    }

    report(50);
    check_cancel();

    // ── 6. Displace vertices ─────────────────────────────────────────────────
    const float half_amp = params.amplitude * 0.5f;

    for (size_t vi = 0; vi < its.vertices.size(); ++vi) {
        const Vec3f pos = its.vertices[vi]; // copy before displacement to avoid aliased read after write
        const Vec3f &nrm = normals[vi];

        // Compute blended UV and sample texture
        UVResult uvs = compute_uv(pos, nrm, params, bnd);
        float grey = sample_uv(texture, uvs); // [0..1]

        // Convert to signed displacement: 0.5 = zero, 1 = +half_amp, 0 = -half_amp
        float displacement = (grey - 0.5f) * 2.f * half_amp;

        // Apply angle masking blend
        if (vmask[vi].total_area > 1e-12f) {
            const float masked_frac = vmask[vi].masked_area / vmask[vi].total_area;
            displacement *= (1.f - masked_frac);
        }

        its.vertices[vi] = pos + nrm * displacement;
    }

    report(75);
    check_cancel();

    // ── 7. Optional decimation ───────────────────────────────────────────────
    if (params.target_triangle_count > 0 &&
        params.target_triangle_count < static_cast<uint32_t>(its.indices.size())) {
        BOOST_LOG_TRIVIAL(info) << "MeshTexturizer: decimating from "
                                << its.indices.size() << " → "
                                << params.target_triangle_count << " triangles …";
        its_quadric_edge_collapse(its, params.target_triangle_count, nullptr,
                                  throw_on_cancel,
                                  [&](int p) { report(75 + p / 4); });
        BOOST_LOG_TRIVIAL(info) << "MeshTexturizer: after decimation: "
                                << its.indices.size() << " triangles";
    }

    report(100);
    return TriangleMesh(std::move(its));
}

} // namespace Slic3r

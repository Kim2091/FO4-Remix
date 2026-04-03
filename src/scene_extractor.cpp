#include "scene_extractor.h"
#include "config.h"
#include "light_extractor.h"

#include "f4se_common/f4se_version.h"
#include "f4se_common/Relocation.h"
#include "f4se/PluginAPI.h"
#include "f4se/NiNodes.h"
#include "f4se/NiObjects.h"
#include "f4se/NiTypes.h"
#include "f4se/BSGeometry.h"
#include "f4se/GameTypes.h"
#include "f4se/NiProperties.h"
#include "f4se/NiMaterials.h"
#include "f4se/NiTextures.h"

#include <d3d11.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <wrl/client.h>

// ---------------------------------------------------------------------------
// Debug: dump an RGBA8 texture to a TGA file for visual inspection
// ---------------------------------------------------------------------------
static void DebugDumpTGA(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return;

    uint8_t header[18] = {};
    header[2]  = 2; // uncompressed true-color
    header[12] = (uint8_t)(w & 0xFF);
    header[13] = (uint8_t)(w >> 8);
    header[14] = (uint8_t)(h & 0xFF);
    header[15] = (uint8_t)(h >> 8);
    header[16] = 32; // bits per pixel
    header[17] = 0x20; // top-left origin
    f.write(reinterpret_cast<const char*>(header), 18);

    // TGA is BGRA, our data is RGBA
    for (uint32_t i = 0; i < w * h; i++) {
        uint8_t pixel[4] = { rgba[i*4+2], rgba[i*4+1], rgba[i*4+0], rgba[i*4+3] };
        f.write(reinterpret_cast<const char*>(pixel), 4);
    }
}

// ---------------------------------------------------------------------------
// g_player RelocPtr — same address as GameReferences.cpp but avoids pulling
// in the full GameReferences / GameForms dependency chain.
// ---------------------------------------------------------------------------
static RelocPtr<uintptr_t> s_g_player(0x032D2260);

// Known offsets (verified by STATIC_ASSERTs in F4SE SDK headers):
//   TESObjectREFR::parentCell  = 0xB8  (GameReferences.h)
//   TESObjectREFR::unkF0       = 0xF0  (LoadedData*)
//   LoadedData::rootNode       = 0x08  (NiNode*)
//   TESObjectCELL::objectList  = 0x70  (tArray<TESObjectREFR*>)

static constexpr uintptr_t OFF_FORM_ID          = 0x14;
static constexpr uintptr_t OFF_REFR_PARENT_CELL = 0xB8;
static constexpr uintptr_t OFF_REFR_LOADED_DATA = 0xF0;
static constexpr uintptr_t OFF_LOADED_ROOT_NODE = 0x08;
static constexpr uintptr_t OFF_CELL_OBJECT_LIST = 0x70;



// ---------------------------------------------------------------------------
// Half-float → float conversion
// ---------------------------------------------------------------------------
static float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;

    uint32_t result;
    if (exp == 0) {
        if (mant == 0) {
            result = sign;
        } else {
            // Denormalized → renormalize
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            result = sign | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        result = sign | 0x7F800000 | (mant << 13); // Inf / NaN
    } else {
        result = sign | ((exp + 112) << 23) | (mant << 13);
    }

    float f;
    memcpy(&f, &result, 4);
    return f;
}

// Packed unsigned byte → [-1, 1]
static float UnpackByte(uint8_t b) {
    return (b / 255.0f) * 2.0f - 1.0f;
}

// ---------------------------------------------------------------------------
// Texture cache — keyed by hash derived from ID3D11Resource pointer
// ---------------------------------------------------------------------------
static std::unordered_map<uint64_t, ExtractedTexture> g_textureCache;

// ---------------------------------------------------------------------------
// Compute mip-0 byte size for a given DXGI_FORMAT, width, height.
// Returns 0 for unsupported formats (caller should skip texture).
// ---------------------------------------------------------------------------
static uint32_t ComputeMip0Size(uint32_t width, uint32_t height, DXGI_FORMAT fmt)
{
    uint32_t bw, bh; // block dimensions (in blocks for BC, in pixels for uncompressed)
    switch (fmt) {
        // BC1 / DXT1
        case DXGI_FORMAT_BC1_TYPELESS:      // 70 — not in task list but close
        case DXGI_FORMAT_BC1_UNORM:         // 71
        case DXGI_FORMAT_BC1_UNORM_SRGB:    // 72
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 8;

        // BC3 / DXT5
        case DXGI_FORMAT_BC3_TYPELESS:      // 76
        case DXGI_FORMAT_BC3_UNORM:         // 77
        case DXGI_FORMAT_BC3_UNORM_SRGB:    // 78
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

        // BC5
        case DXGI_FORMAT_BC5_TYPELESS:      // 82
        case DXGI_FORMAT_BC5_UNORM:         // 83
        case DXGI_FORMAT_BC5_SNORM:         // 84
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

        // BC7
        case DXGI_FORMAT_BC7_TYPELESS:      // 97
        case DXGI_FORMAT_BC7_UNORM:         // 98
        case DXGI_FORMAT_BC7_UNORM_SRGB:    // 99
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

        // R8G8B8A8
        case DXGI_FORMAT_R8G8B8A8_UNORM:        // 28
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   // 29
            return width * height * 4;

        // B8G8R8A8
        case DXGI_FORMAT_B8G8R8A8_UNORM:         // 87
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:    // 91
            return width * height * 4;

        default:
            return 0; // unsupported
    }
}

// ---------------------------------------------------------------------------
// Determine whether a DXGI_FORMAT is block-compressed and its block byte size.
// Returns false for uncompressed formats.
// ---------------------------------------------------------------------------
static bool IsBlockCompressed(DXGI_FORMAT fmt, uint32_t& blockSize)
{
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            blockSize = 8;
            return true;

        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            blockSize = 16;
            return true;

        default:
            blockSize = 0;
            return false;
    }
}

// ---------------------------------------------------------------------------
// GPU readback: copy mip 0 of a texture into CPU memory
// ---------------------------------------------------------------------------
static bool ReadbackTexture(ID3D11Device* device, ID3D11Texture2D* tex2D,
                            uint64_t hash, ExtractedTexture& out)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx) return false;

    D3D11_TEXTURE2D_DESC desc;
    tex2D->GetDesc(&desc);

    uint32_t dataSize = ComputeMip0Size(desc.Width, desc.Height, desc.Format);
    if (dataSize == 0) {
        _MESSAGE("FO4RemixPlugin: ReadbackTexture - unsupported DXGI format %u, skipping", (unsigned)desc.Format);
        return false;
    }

    // Create staging texture matching mip 0
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width              = desc.Width;
    stagingDesc.Height             = desc.Height;
    stagingDesc.MipLevels          = 1;
    stagingDesc.ArraySize          = 1;
    stagingDesc.Format             = desc.Format;
    stagingDesc.SampleDesc.Count   = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage              = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags          = 0;
    stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        _MESSAGE("FO4RemixPlugin: ReadbackTexture - CreateTexture2D staging failed hr=0x%08X", (unsigned)hr);
        return false;
    }

    // Copy mip 0 from the source texture
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex2D, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        _MESSAGE("FO4RemixPlugin: ReadbackTexture - Map failed hr=0x%08X", (unsigned)hr);
        return false;
    }

    // Determine row layout
    uint32_t blockSize = 0;
    bool bc = IsBlockCompressed(desc.Format, blockSize);

    uint32_t numRows;       // number of scanline-rows (or block-rows for BC)
    uint32_t expectedPitch; // tight row pitch

    if (bc) {
        uint32_t bw = (desc.Width  + 3) / 4; if (bw < 1) bw = 1;
        uint32_t bh = (desc.Height + 3) / 4; if (bh < 1) bh = 1;
        numRows       = bh;
        expectedPitch = bw * blockSize;
    } else {
        numRows       = desc.Height;
        expectedPitch = desc.Width * 4; // all uncompressed formats we support are 4 bpp
    }

    out.pixels.resize(dataSize);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = out.pixels.data();

    for (uint32_t row = 0; row < numRows; row++) {
        memcpy(dst, src, expectedPitch);
        src += mapped.RowPitch;
        dst += expectedPitch;
    }

    ctx->Unmap(staging.Get(), 0);

    // Fill metadata
    out.hash       = hash;
    out.width      = desc.Width;
    out.height     = desc.Height;
    out.dxgiFormat = desc.Format;

    return true;
}

// ---------------------------------------------------------------------------
// BC1/BC3 decode helpers — decompress a 4x4 block to RGBA8
// ---------------------------------------------------------------------------
static void DecodeBC1ColorBlock(const uint8_t* block, uint8_t out[4][4][4])
{
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);

    uint8_t colors[4][3];
    colors[0][0] = ((c0 >> 11) & 0x1F) * 255 / 31;
    colors[0][1] = ((c0 >>  5) & 0x3F) * 255 / 63;
    colors[0][2] = ( c0        & 0x1F) * 255 / 31;
    colors[1][0] = ((c1 >> 11) & 0x1F) * 255 / 31;
    colors[1][1] = ((c1 >>  5) & 0x3F) * 255 / 63;
    colors[1][2] = ( c1        & 0x1F) * 255 / 31;

    if (c0 > c1) {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = (2 * colors[0][i] + colors[1][i] + 1) / 3;
            colors[3][i] = (colors[0][i] + 2 * colors[1][i] + 1) / 3;
        }
    } else {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = (colors[0][i] + colors[1][i]) / 2;
            colors[3][i] = 0;
        }
    }

    uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = indices & 3;
            indices >>= 2;
            out[y][x][0] = colors[idx][0];
            out[y][x][1] = colors[idx][1];
            out[y][x][2] = colors[idx][2];
            out[y][x][3] = 255;
        }
    }
}

static void DecodeBC3AlphaBlock(const uint8_t* block, uint8_t alphas[4][4])
{
    uint8_t a0 = block[0], a1 = block[1];
    uint8_t palette[8];
    palette[0] = a0;
    palette[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; i++)
            palette[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (int i = 1; i <= 4; i++)
            palette[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }

    uint64_t bits = 0;
    for (int i = 2; i < 8; i++)
        bits |= (uint64_t)block[i] << ((i - 2) * 8);

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            alphas[y][x] = palette[bits & 7];
            bits >>= 3;
        }
}

// Decompress a BC-compressed texture to R8G8B8A8 and invert RGB (smoothness → roughness)
static bool DecompressAndInvert(ExtractedTexture& tex)
{
    bool isBC1 = (tex.dxgiFormat == DXGI_FORMAT_BC1_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC1_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC1_TYPELESS);
    bool isBC3 = (tex.dxgiFormat == DXGI_FORMAT_BC3_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC3_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC3_TYPELESS);
    bool isBC5 = (tex.dxgiFormat == DXGI_FORMAT_BC5_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC5_SNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC5_TYPELESS);

    if (!isBC1 && !isBC3 && !isBC5) return false;

    uint32_t bw = (tex.width + 3) / 4;
    uint32_t bh = (tex.height + 3) / 4;
    uint32_t blockSize = isBC1 ? 8 : 16;

    std::vector<uint8_t> rgba(tex.width * tex.height * 4);
    const uint8_t* src = tex.pixels.data();

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t block[4][4][4]; // [y][x][rgba]

            if (isBC5) {
                // BC5: R=specular, G=smoothness. Use G channel as-is, write grayscale.
                uint8_t rChan[4][4], gChan[4][4];
                DecodeBC3AlphaBlock(src, rChan);
                DecodeBC3AlphaBlock(src + 8, gChan);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        uint8_t roughness = gChan[y][x];
                        block[y][x][0] = roughness;
                        block[y][x][1] = roughness;
                        block[y][x][2] = roughness;
                        block[y][x][3] = 255;
                    }
                src += blockSize;

                // Write pixels (already inverted)
                for (int y = 0; y < 4; y++) {
                    uint32_t py = by * 4 + y;
                    if (py >= tex.height) continue;
                    for (int x = 0; x < 4; x++) {
                        uint32_t px = bx * 4 + x;
                        if (px >= tex.width) continue;
                        uint32_t offset = (py * tex.width + px) * 4;
                        memcpy(&rgba[offset], block[y][x], 4);
                    }
                }
                continue;
            } else if (isBC3) {
                uint8_t alphas[4][4];
                DecodeBC3AlphaBlock(src, alphas);
                DecodeBC1ColorBlock(src + 8, block);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        block[y][x][3] = alphas[y][x];
            } else {
                DecodeBC1ColorBlock(src, block);
            }
            src += blockSize;

            // Write decoded + inverted pixels (BC1/BC3 path)
            for (int y = 0; y < 4; y++) {
                uint32_t py = by * 4 + y;
                if (py >= tex.height) continue;
                for (int x = 0; x < 4; x++) {
                    uint32_t px = bx * 4 + x;
                    if (px >= tex.width) continue;
                    uint32_t offset = (py * tex.width + px) * 4;
                    rgba[offset + 0] = 255 - block[y][x][0]; // invert R
                    rgba[offset + 1] = 255 - block[y][x][1]; // invert G
                    rgba[offset + 2] = 255 - block[y][x][2]; // invert B
                    rgba[offset + 3] = block[y][x][3];       // keep A
                }
            }
        }
    }

    tex.pixels = std::move(rgba);
    tex.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    return true;
}

// Invert an uncompressed RGBA texture in-place (smoothness → roughness)
static void InvertUncompressed(ExtractedTexture& tex)
{
    for (uint32_t i = 0; i < tex.width * tex.height; i++) {
        tex.pixels[i * 4 + 0] = 255 - tex.pixels[i * 4 + 0];
        tex.pixels[i * 4 + 1] = 255 - tex.pixels[i * 4 + 1];
        tex.pixels[i * 4 + 2] = 255 - tex.pixels[i * 4 + 2];
        // leave alpha
    }
}

// Convert FO4 smoothness/spec mask → Remix roughness by inverting RGB
static void SmoothnessToRoughness(ExtractedTexture& tex)
{
    switch (tex.dxgiFormat) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            InvertUncompressed(tex);
            break;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
            DecompressAndInvert(tex);
            break;
        default:
            // BC7/other: can't easily invert, leave as-is
            break;
    }
}

// ---------------------------------------------------------------------------
// Tangent-space normal → hemispherical octahedral encoding
// (Port of NVIDIA's LightspeedOctahedralConverter, MIT license)
// ---------------------------------------------------------------------------
static void DecompressNormalBC(ExtractedTexture& tex)
{
    bool isBC1 = (tex.dxgiFormat == DXGI_FORMAT_BC1_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC1_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC1_TYPELESS);
    bool isBC3 = (tex.dxgiFormat == DXGI_FORMAT_BC3_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC3_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC3_TYPELESS);
    bool isBC5 = (tex.dxgiFormat == DXGI_FORMAT_BC5_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC5_SNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC5_TYPELESS);

    if (!isBC1 && !isBC3 && !isBC5) return;

    uint32_t bw = (tex.width + 3) / 4;
    uint32_t bh = (tex.height + 3) / 4;
    uint32_t blockSize = isBC1 ? 8 : 16;

    std::vector<uint8_t> rgba(tex.width * tex.height * 4);
    const uint8_t* src = tex.pixels.data();

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t block[4][4][4];

            if (isBC5) {
                // BC5: two alpha-style blocks for R and G; reconstruct Z
                uint8_t rChan[4][4], gChan[4][4];
                DecodeBC3AlphaBlock(src, rChan);
                DecodeBC3AlphaBlock(src + 8, gChan);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        float nx = rChan[y][x] / 255.0f * 2.0f - 1.0f;
                        float ny = gChan[y][x] / 255.0f * 2.0f - 1.0f;
                        float nz2 = 1.0f - nx * nx - ny * ny;
                        float nz = nz2 > 0.0f ? sqrtf(nz2) : 0.0f;
                        block[y][x][0] = rChan[y][x];
                        block[y][x][1] = gChan[y][x];
                        block[y][x][2] = (uint8_t)(nz * 127.5f + 127.5f);
                        block[y][x][3] = 255;
                    }
            } else if (isBC3) {
                uint8_t alphas[4][4];
                DecodeBC3AlphaBlock(src, alphas);
                DecodeBC1ColorBlock(src + 8, block);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        block[y][x][3] = alphas[y][x];
            } else {
                DecodeBC1ColorBlock(src, block);
            }
            src += blockSize;

            for (int y = 0; y < 4; y++) {
                uint32_t py = by * 4 + y;
                if (py >= tex.height) continue;
                for (int x = 0; x < 4; x++) {
                    uint32_t px = bx * 4 + x;
                    if (px >= tex.width) continue;
                    uint32_t offset = (py * tex.width + px) * 4;
                    memcpy(&rgba[offset], block[y][x], 4);
                }
            }
        }
    }

    tex.pixels = std::move(rgba);
    tex.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
}

static void ConvertNormalToOctahedral(ExtractedTexture& tex)
{
    // Decompress BC formats to RGBA first
    switch (tex.dxgiFormat) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC5_TYPELESS:
            DecompressNormalBC(tex);
            break;
        default:
            break;
    }

    // Must be uncompressed RGBA at this point
    if (tex.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM &&
        tex.dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        tex.dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
        tex.dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return;

    for (uint32_t i = 0; i < tex.width * tex.height; i++) {
        uint8_t* p = &tex.pixels[i * 4];

        // Decode tangent-space normal from [0,255] to [-1,1]
        float x = p[0] / 255.0f * 2.0f - 1.0f;
        float y = p[1] / 255.0f * 2.0f - 1.0f;
        float z = p[2] / 255.0f * 2.0f - 1.0f;

        // Clamp Z >= 0 (hemispherical — inward normals not supported by Remix)
        if (z < 0.0f) z = -z;

        // Normalize
        float len = sqrtf(x * x + y * y + z * z);
        if (len > 1e-6f) { x /= len; y /= len; z /= len; }
        else { x = 0; y = 0; z = 1; }

        // Octahedral projection: project onto octahedron surface
        float absSum = fabsf(x) + fabsf(y) + fabsf(z);
        float ox = x / absSum;
        float oy = y / absSum;

        // Hemisphere encoding
        float rx = (ox + oy) * 0.5f + 0.5f;
        float ry = (ox - oy) * 0.5f + 0.5f;

        p[0] = (uint8_t)fminf(fmaxf(rx * 255.0f + 0.5f, 0.0f), 255.0f);
        p[1] = (uint8_t)fminf(fmaxf(ry * 255.0f + 0.5f, 0.0f), 255.0f);
        p[2] = 0;
        p[3] = 255;
    }

    tex.dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
}

// ---------------------------------------------------------------------------
// Generic texture extraction from any NiTexture slot
// ---------------------------------------------------------------------------
enum class TexturePostProcess { None, InvertRGB, Octahedral };

static uint64_t ExtractMaterialTexture(NiTexture* tex, const char* slotName,
                                       ID3D11Device* device,
                                       std::vector<ExtractedTexture>& newTextures,
                                       TexturePostProcess postProcess = TexturePostProcess::None)
{
    if (!tex) return 0;

    BSRenderData* renderData = tex->rendererData;
    if (!renderData) return 0;

    ID3D11Resource* resource = renderData->resource;
    if (!resource) return 0;

    // Stable hash from texture name so hashes are consistent across runs
    const char* texName = tex->name.c_str();
    uint64_t hash = FnvHash(texName ? texName : "");
    // Include post-processing mode so variants don't collide
    if (postProcess == TexturePostProcess::InvertRGB)  hash = FnvHashCombine(hash, 1);
    if (postProcess == TexturePostProcess::Octahedral) hash = FnvHashCombine(hash, 2);

    // Check cache first
    auto it = g_textureCache.find(hash);
    if (it != g_textureCache.end()) {
        return hash;
    }

    // QueryInterface to ID3D11Texture2D
    ID3D11Texture2D* tex2D = nullptr;
    HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void**>(&tex2D));
    if (FAILED(hr) || !tex2D) return 0;

    ExtractedTexture extracted;
    bool ok = ReadbackTexture(device, tex2D, hash, extracted);
    tex2D->Release();

    if (!ok) return 0;

    // --- Debug dump: diffuse control (no post-processing) ---
    if (postProcess == TexturePostProcess::None) {
        static int s_dumpDiffuse = 0;
        if (s_dumpDiffuse < 2) {
            ExtractedTexture tmp = extracted;
            // Decompress BC1 to RGBA so we can dump it
            bool isBC1tmp = (tmp.dxgiFormat == DXGI_FORMAT_BC1_UNORM ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC1_UNORM_SRGB ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC1_TYPELESS);
            if (isBC1tmp) {
                DecompressNormalBC(tmp); // reuses the BC1 decode path to get RGBA
            }
            if (tmp.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                tmp.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                char path[256];
                snprintf(path, sizeof(path), "c:/temp/fo4_debug_diffuse_%d.tga", s_dumpDiffuse++);
                DebugDumpTGA(path, tmp.pixels.data(), tmp.width, tmp.height);
                _MESSAGE("FO4RemixPlugin: DEBUG dumped diffuse -> %s (%ux%u)", path, tmp.width, tmp.height);
            }
        }
    }

    // --- Debug dump: raw BC5 decode (before post-processing) ---
    // Dump first 3 normal + first 3 roughness textures for inspection.
    {
        static int s_dumpNormalRaw = 0, s_dumpRoughnessRaw = 0;
        bool dumpRaw = false;
        if (postProcess == TexturePostProcess::Octahedral && s_dumpNormalRaw < 3) dumpRaw = true;
        if (postProcess == TexturePostProcess::InvertRGB  && s_dumpRoughnessRaw < 3) dumpRaw = true;

        if (dumpRaw) {
            // BC5 needs decompression to RGBA before we can dump, so do a temporary decode
            ExtractedTexture tmp = extracted; // copy
            bool isBC5tmp = (tmp.dxgiFormat == DXGI_FORMAT_BC5_UNORM ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC5_SNORM ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC5_TYPELESS);
            if (isBC5tmp) {
                DecompressNormalBC(tmp); // decodes to RGBA with reconstructed Z
            }
            if (tmp.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                tmp.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
                char path[256];
                if (postProcess == TexturePostProcess::Octahedral)
                    snprintf(path, sizeof(path), "c:/temp/fo4_debug_normal_raw_%d.tga", s_dumpNormalRaw++);
                else
                    snprintf(path, sizeof(path), "c:/temp/fo4_debug_roughness_raw_%d.tga", s_dumpRoughnessRaw++);
                DebugDumpTGA(path, tmp.pixels.data(), tmp.width, tmp.height);
                _MESSAGE("FO4RemixPlugin: DEBUG dumped raw decode -> %s (%ux%u)", path, tmp.width, tmp.height);
            }
        }
    }

    if (postProcess == TexturePostProcess::InvertRGB) {
        SmoothnessToRoughness(extracted);
    } else if (postProcess == TexturePostProcess::Octahedral) {
        ConvertNormalToOctahedral(extracted);
    }

    // --- Debug dump: after post-processing ---
    {
        static int s_dumpNormalPost = 0, s_dumpRoughnessPost = 0;
        bool dumpPost = false;
        if (postProcess == TexturePostProcess::Octahedral && s_dumpNormalPost < 3) dumpPost = true;
        if (postProcess == TexturePostProcess::InvertRGB  && s_dumpRoughnessPost < 3) dumpPost = true;

        if (dumpPost && (extracted.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                         extracted.dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
            char path[256];
            if (postProcess == TexturePostProcess::Octahedral)
                snprintf(path, sizeof(path), "c:/temp/fo4_debug_normal_post_%d.tga", s_dumpNormalPost++);
            else
                snprintf(path, sizeof(path), "c:/temp/fo4_debug_roughness_post_%d.tga", s_dumpRoughnessPost++);
            DebugDumpTGA(path, extracted.pixels.data(), extracted.width, extracted.height);
            _MESSAGE("FO4RemixPlugin: DEBUG dumped post-process -> %s (%ux%u)", path, extracted.width, extracted.height);
        }
    }

    if (g_config.logTextures) {
        const char* texName = tex->name.c_str();
        _MESSAGE("FO4RemixPlugin: Extracted %s texture \"%s\" %ux%u fmt=%u hash=0x%016llX%s",
                 slotName, texName ? texName : "<null>",
                 extracted.width, extracted.height,
                 (unsigned)extracted.dxgiFormat, hash,
                 postProcess == TexturePostProcess::InvertRGB ? " (inverted)" :
                 postProcess == TexturePostProcess::Octahedral ? " (octahedral)" : "");
    }

    g_textureCache[hash] = extracted;
    newTextures.push_back(std::move(extracted));

    return hash;
}

// Get the BSLightingShaderMaterialBase from a shape, or nullptr
static BSLightingShaderMaterialBase* GetLightingMaterial(BSTriShape* shape)
{
    if (!shape) return nullptr;
    NiProperty* prop = shape->shaderProperty;
    if (!prop) return nullptr;
    BSShaderProperty* shaderProp = static_cast<BSShaderProperty*>(prop);
    BSShaderMaterial* material = shaderProp->shaderMaterial;
    if (!material || material->GetFeature() != 2) return nullptr;
    return static_cast<BSLightingShaderMaterialBase*>(material);
}

// ---------------------------------------------------------------------------
// Extract vertex/index data from a single BSTriShape
// ---------------------------------------------------------------------------
static bool ExtractTriShape(BSTriShape* shape, uint64_t baseHash,
                            std::vector<ExtractedMesh>& out,
                            ID3D11Device* device,
                            std::vector<ExtractedTexture>& newTextures)
{
    if (!shape || shape->numVertices == 0 || shape->numTriangles == 0)
        return false;

    // Renderer data → vertex/index buffers
    auto* gfxData = static_cast<BSGraphics::TriShape*>(shape->pRendererData);
    if (!gfxData || !gfxData->pVB || !gfxData->pIB)
        return false;

    uint8_t* vbData = static_cast<uint8_t*>(gfxData->pVB->pData);
    uint8_t* ibData = static_cast<uint8_t*>(gfxData->pIB->pData);
    if (!vbData || !ibData)
        return false;

    uint64_t desc = shape->vertexDesc;
    uint16_t vertexSize = shape->GetVertexSize();
    if (vertexSize == 0) return false;

    bool hasUVs        = (desc & BSGeometry::kFlag_UVs) != 0;
    bool hasNormals    = (desc & BSGeometry::kFlag_Normals) != 0;
    bool hasColors     = (desc & BSGeometry::kFlag_VertexColors) != 0;

    // Nibble 1 (szVertex) is a base added to each component offset nibble.
    // For half-float positions szVertex=0 and offsets are already absolute.
    // For full-precision positions szVertex=3 (12 bytes / 4) and must be added.
    uint32_t szVertex = (desc >> 4) & 0xF;
    uint32_t oUV     = (szVertex + ((desc >>  8) & 0xF)) * 4;
    uint32_t oNormal = (szVertex + ((desc >> 16) & 0xF)) * 4;
    uint32_t oColor  = (szVertex + ((desc >> 24) & 0xF)) * 4;

    // FullPrecision flag: positions are 3 full floats (12 bytes)
    // Otherwise: positions are 4 half-floats (8 bytes)
    bool posHalfFloat = !(desc & BSGeometry::kFlag_FullPrecision);

    const char* shapeName = shape->m_name.c_str();
    if (g_config.logShapeInfo) {
        _MESSAGE("FO4RemixPlugin: Shape \"%s\" vertexSize=%u desc=0x%016llX posHalf=%d "
                 "flags: UV=%d Norm=%d Color=%d FullPrec=%d Skinned=%d verts=%u tris=%u",
                 shapeName ? shapeName : "<null>",
                 vertexSize, desc, posHalfFloat,
                 hasUVs, hasNormals, hasColors,
                 (desc & BSGeometry::kFlag_FullPrecision) ? 1 : 0,
                 (desc & BSGeometry::kFlag_Skinned) ? 1 : 0,
                 shape->numVertices, shape->numTriangles);
    }

    ExtractedMesh mesh;
    // Combine REFR-based hash with shape name for stable per-shape uniqueness
    mesh.hash = FnvHashCombine(baseHash, FnvHash(shapeName ? shapeName : ""));
    mesh.vertices.resize(shape->numVertices);

    for (uint16_t i = 0; i < shape->numVertices; i++) {
        uint8_t* v = vbData + (uint32_t)i * vertexSize;
        remixapi_HardcodedVertex& out_v = mesh.vertices[i];
        memset(&out_v, 0, sizeof(out_v));

        // Position (always at offset 0)
        if (posHalfFloat) {
            uint16_t* pos = reinterpret_cast<uint16_t*>(v);
            out_v.position[0] = HalfToFloat(pos[0]);
            out_v.position[1] = HalfToFloat(pos[1]);
            out_v.position[2] = HalfToFloat(pos[2]);
        } else {
            float* pos = reinterpret_cast<float*>(v);
            out_v.position[0] = pos[0];
            out_v.position[1] = pos[1];
            out_v.position[2] = pos[2];
        }

        // UVs
        if (hasUVs && oUV + 4 <= vertexSize) {
            uint16_t* uv = reinterpret_cast<uint16_t*>(v + oUV);
            out_v.texcoord[0] = HalfToFloat(uv[0]);
            out_v.texcoord[1] = HalfToFloat(uv[1]);
        }

        // Normals (packed as 3 unsigned bytes + 1 byte bitangent sign)
        if (hasNormals && oNormal + 4 <= vertexSize) {
            uint8_t* n = v + oNormal;
            out_v.normal[0] = UnpackByte(n[0]);
            out_v.normal[1] = UnpackByte(n[1]);
            out_v.normal[2] = UnpackByte(n[2]);
        } else {
            out_v.normal[0] = 0.0f;
            out_v.normal[1] = 0.0f;
            out_v.normal[2] = 1.0f;
        }

        // Color (BGRA byte order → pack as B8G8R8A8)
        if (hasColors && oColor + 4 <= vertexSize) {
            memcpy(&out_v.color, v + oColor, 4);
        } else {
            out_v.color = 0xFFFFFFFF; // white
        }

    }

    // Validate vertex positions — reject mesh if any are NaN/Inf
    for (uint16_t i = 0; i < shape->numVertices; i++) {
        const auto& pos = mesh.vertices[i].position;
        for (int j = 0; j < 3; j++) {
            if (std::isnan(pos[j]) || std::isinf(pos[j])) {
                if (g_config.logRejections)
                    _MESSAGE("FO4RemixPlugin: Rejecting mesh \"%s\" - vertex %u has NaN/Inf position",
                             shapeName ? shapeName : "<null>", i);
                return false;
            }
        }
    }

    // Indices (uint16 → uint32)
    uint32_t indexCount = shape->numTriangles * 3;
    uint16_t* indices16 = reinterpret_cast<uint16_t*>(ibData);
    mesh.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; i++) {
        uint32_t idx = indices16[i];
        if (idx >= shape->numVertices) {
            if (g_config.logRejections)
                _MESSAGE("FO4RemixPlugin: Rejecting mesh \"%s\" - index[%u]=%u >= numVertices=%u",
                         shapeName ? shapeName : "<null>", i, idx, shape->numVertices);
            return false;
        }
        mesh.indices[i] = idx;
    }

    // World transform → row-major 3x4
    // Negate X and Z axes to mirror the world into Remix's LH coordinate system
    const NiTransform& xf = shape->m_worldTransform;
    float scale = xf.scale;
    // Swap X and Y columns in rotation to match camera coordinate swap
    for (int r = 0; r < 3; r++) {
        mesh.worldTransform[r][0] = xf.rot.data[r][1] * scale;
        mesh.worldTransform[r][1] = xf.rot.data[r][0] * scale;
        mesh.worldTransform[r][2] = xf.rot.data[r][2] * scale;
    }
    // Swap X and Y in translation too
    mesh.worldTransform[0][3] = xf.pos.y;
    mesh.worldTransform[1][3] = xf.pos.x;
    mesh.worldTransform[2][3] = xf.pos.z;

    // Compute local-space bounding extent for diagnostics
    float minPos[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float maxPos[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint16_t i = 0; i < shape->numVertices; i++) {
        for (int j = 0; j < 3; j++) {
            if (mesh.vertices[i].position[j] < minPos[j]) minPos[j] = mesh.vertices[i].position[j];
            if (mesh.vertices[i].position[j] > maxPos[j]) maxPos[j] = mesh.vertices[i].position[j];
        }
    }
    float extentX = (maxPos[0] - minPos[0]) * scale;
    float extentY = (maxPos[1] - minPos[1]) * scale;
    float extentZ = (maxPos[2] - minPos[2]) * scale;
    float maxExtent = extentX;
    if (extentY > maxExtent) maxExtent = extentY;
    if (extentZ > maxExtent) maxExtent = extentZ;

    // Reject shapes with garbage vertex data (huge but finite extents)
    if (maxExtent > g_config.maxExtent) {
        if (g_config.logRejections)
            _MESSAGE("FO4RemixPlugin: Rejecting shape \"%s\" - extent %.0f exceeds max (%.0f)",
                     shapeName ? shapeName : "<null>", maxExtent, g_config.maxExtent);
        return false;
    }

    // Log shapes with large world extent to help identify unwanted geometry
    if (g_config.logLargeShapes && maxExtent > 500.0f) {
        _MESSAGE("FO4RemixPlugin: LARGE shape \"%s\" extent=(%.0f, %.0f, %.0f) maxExt=%.0f "
                 "worldPos=(%.1f, %.1f, %.1f) verts=%u tris=%u",
                 shapeName ? shapeName : "<null>",
                 extentX, extentY, extentZ, maxExtent,
                 xf.pos.x, xf.pos.y, xf.pos.z,
                 shape->numVertices, shape->numTriangles);
    }

    // Extract textures from the lighting material
    BSLightingShaderMaterialBase* lightingMat = GetLightingMaterial(shape);
    mesh.diffuseTextureHash   = lightingMat ? ExtractMaterialTexture(lightingMat->spDiffuseTexture, "diffuse", device, newTextures) : 0;
    mesh.normalTextureHash    = lightingMat ? ExtractMaterialTexture(lightingMat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral) : 0;
    mesh.roughnessTextureHash = lightingMat ? ExtractMaterialTexture(lightingMat->spSmoothnessSpecMaskTexture, "roughness", device, newTextures, TexturePostProcess::InvertRGB) : 0;

    out.push_back(std::move(mesh));
    return true;
}

// ---------------------------------------------------------------------------
// Recursively walk an NiNode tree and extract all BSTriShape children
// ---------------------------------------------------------------------------
static void WalkNode(NiAVObject* obj, uint64_t baseHash,
                     std::vector<ExtractedMesh>& out,
                     ID3D11Device* device,
                     std::vector<ExtractedTexture>& newTextures,
                     int depth = 0)
{
    if (!obj || depth > 32) return;

    // Skip invisible nodes
    if (obj->flags & NiAVObject::kFlagNotVisible)
        return;

    // Skip non-renderable geometry by node name
    const char* nodeName = obj->m_name.c_str();
    if (nodeName && nodeName[0]) {
        if (strstr(nodeName, "Marker") ||
            strstr(nodeName, "Portal") ||
            strstr(nodeName, "Trigger") ||
            strstr(nodeName, "MultiBound") ||
            strstr(nodeName, "Collision") ||
            strstr(nodeName, "bhk")) {
            return;
        }
    }

    // Check if this is a BSTriShape
    BSTriShape* tri = obj->GetAsBSTriShape();
    if (tri) {
        ExtractTriShape(tri, baseHash, out, device, newTextures);
        return; // BSTriShape is a leaf — no children
    }

    // If it's an NiNode, recurse into children
    NiNode* node = obj->GetAsNiNode();
    if (node) {
        for (uint16_t i = 0; i < node->m_children.m_emptyRunStart; i++) {
            NiAVObject* child = node->m_children.m_data[i];
            if (child) {
                WalkNode(child, baseHash, out, device, newTextures, depth + 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Lightweight readiness check — is the player in a cell with loaded 3D?
// ---------------------------------------------------------------------------
bool SceneExtractor::IsPlayerCellReady()
{
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer)
        return false;
    uintptr_t player = *ppPlayer;

    // parentCell must exist
    uintptr_t cellPtr = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
    if (!cellPtr)
        return false;

    // Cell must have objects
    struct SimpleArray {
        uintptr_t* entries;
        uint32_t capacity;
        uint32_t pad0C;
        uint32_t count;
    };
    auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);
    if (!objectList.entries || objectList.count == 0)
        return false;

    // Player's own 3D must be loaded (strong signal that cell 3D is populated)
    uintptr_t loadedData = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_LOADED_DATA);
    if (!loadedData)
        return false;
    NiNode* rootNode = *reinterpret_cast<NiNode**>(loadedData + OFF_LOADED_ROOT_NODE);
    if (!rootNode)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Extract all geometry from loaded references in the player's current cell
// ---------------------------------------------------------------------------
ExtractionResult SceneExtractor::ExtractPlayerCell(ID3D11Device* device)
{
    std::vector<ExtractedMesh> result;
    std::vector<ExtractedTexture> newTextures;

    // Get player pointer
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) {
        _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - no player");
        return { std::move(result), std::move(newTextures) };
    }
    uintptr_t player = *ppPlayer;

    // Get parentCell
    uintptr_t cellPtr = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
    if (!cellPtr) {
        _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - no parentCell");
        return { std::move(result), std::move(newTextures) };
    }

    // Access cell objectList (tArray<TESObjectREFR*> at offset 0x70)
    struct SimpleArray {
        uintptr_t* entries;
        uint32_t capacity;
        uint32_t pad0C;
        uint32_t count;
    };
    auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);

    _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - cell has %u objects", objectList.count);

    uint32_t meshCount = 0;

    for (uint32_t i = 0; i < objectList.count; i++) {
        uintptr_t refrPtr = objectList.entries[i];
        if (!refrPtr) continue;

        // Get LoadedData
        uintptr_t loadedData = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_LOADED_DATA);
        if (!loadedData) continue;

        // Get root NiNode
        NiNode* rootNode = *reinterpret_cast<NiNode**>(loadedData + OFF_LOADED_ROOT_NODE);
        if (!rootNode) continue;

        // Stable base hash from REFR form ID (consistent across runs)
        uint32_t refrFormID = *reinterpret_cast<uint32_t*>(refrPtr + OFF_FORM_ID);
        uint64_t baseHash = FnvHashCombine(0xCBF29CE484222325ULL, (uint64_t)refrFormID);

        size_t before = result.size();
        WalkNode(rootNode, baseHash, result, device, newTextures);
        meshCount += (uint32_t)(result.size() - before);
    }

    auto lights = LightExtractor::ExtractPlayerCellLights();

    // Count texture types per mesh
    uint32_t hasDiffuse = 0, hasNormal = 0, hasRoughness = 0;
    for (auto& m : result) {
        if (m.diffuseTextureHash)   hasDiffuse++;
        if (m.normalTextureHash)    hasNormal++;
        if (m.roughnessTextureHash) hasRoughness++;
    }

    _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - %u meshes (%u diffuse, %u normal, %u roughness), "
             "%zu new textures (%zu cached), %zu lights from %u objects",
             meshCount, hasDiffuse, hasNormal, hasRoughness,
             newTextures.size(), g_textureCache.size(), lights.size(), objectList.count);

    return { std::move(result), std::move(newTextures), std::move(lights) };
}

// ---------------------------------------------------------------------------
// Clear the texture readback cache
// ---------------------------------------------------------------------------
void SceneExtractor::ClearTextureCache()
{
    _MESSAGE("FO4RemixPlugin: ClearTextureCache - clearing %zu entries", g_textureCache.size());
    g_textureCache.clear();
}

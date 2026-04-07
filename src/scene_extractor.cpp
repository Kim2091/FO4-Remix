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
static RelocPtr<uintptr_t> s_g_dataHandler(0x030DC000);

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
static constexpr uintptr_t OFF_CELL_FLAGS       = 0x40;
static constexpr uintptr_t OFF_CELL_LAND        = 0x58;  // TESObjectLAND*
static constexpr uintptr_t OFF_CELL_WORLD_SPACE = 0xC8;
static constexpr uint16_t  CELL_FLAG_IS_INTERIOR = 0x0001;
static constexpr uintptr_t OFF_LAND_QUADRANTS   = 0x40;  // BSMultiBoundNode*[4]
static constexpr int       LAND_QUADRANT_COUNT  = 4;

// TES singleton and GridCellArray offsets (from static analysis of Fallout4.exe)
static RelocPtr<uintptr_t> s_g_tes(0x032D2048);
static constexpr uintptr_t OFF_TES_GRID_CELLS   = 0x18;  // GridCellArray*
static constexpr uintptr_t OFF_GRID_DIMENSION    = 0x10;  // int32 (= uGridsToLoad)
static constexpr uintptr_t OFF_GRID_CELL_ARRAY   = 0x18;  // TESObjectCELL** (flat dim*dim)



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

        // BC2 / DXT3
        case DXGI_FORMAT_BC2_TYPELESS:      // 73
        case DXGI_FORMAT_BC2_UNORM:         // 74
        case DXGI_FORMAT_BC2_UNORM_SRGB:    // 75
            bw = (width + 3) / 4;  if (bw < 1) bw = 1;
            bh = (height + 3) / 4; if (bh < 1) bh = 1;
            return bw * bh * 16;

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

        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
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

// Decompress a BC2 (DXT3) texture to R8G8B8A8
// BC2 = 8 bytes explicit 4-bit alpha + 8 bytes BC1 color per 4x4 block
static bool DecompressBC2(ExtractedTexture& tex)
{
    bool isBC2 = (tex.dxgiFormat == DXGI_FORMAT_BC2_UNORM ||
                  tex.dxgiFormat == DXGI_FORMAT_BC2_UNORM_SRGB ||
                  tex.dxgiFormat == DXGI_FORMAT_BC2_TYPELESS);
    if (!isBC2) return false;

    uint32_t bw = (tex.width + 3) / 4;
    uint32_t bh = (tex.height + 3) / 4;

    std::vector<uint8_t> rgba(tex.width * tex.height * 4);
    const uint8_t* src = tex.pixels.data();

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            // First 8 bytes: explicit 4-bit alpha for each of the 16 pixels
            uint8_t alphas[4][4];
            for (int y = 0; y < 4; y++) {
                uint8_t lo = src[y * 2];
                uint8_t hi = src[y * 2 + 1];
                alphas[y][0] = (lo & 0x0F) | ((lo & 0x0F) << 4);
                alphas[y][1] = (lo >> 4)   | ((lo >> 4) << 4);
                alphas[y][2] = (hi & 0x0F) | ((hi & 0x0F) << 4);
                alphas[y][3] = (hi >> 4)   | ((hi >> 4) << 4);
            }

            // Next 8 bytes: BC1 color block
            uint8_t block[4][4][4];
            DecodeBC1ColorBlock(src + 8, block);

            // Combine color + alpha
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++)
                    block[y][x][3] = alphas[y][x];
            }

            src += 16;

            // Write pixels
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
    return true;
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
                // BC5: R=specular, G=smoothness. Invert G to get roughness.
                uint8_t rChan[4][4], gChan[4][4];
                DecodeBC3AlphaBlock(src, rChan);
                DecodeBC3AlphaBlock(src + 8, gChan);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        uint8_t roughness = 255 - gChan[y][x];
                        block[y][x][0] = roughness;
                        block[y][x][1] = roughness;
                        block[y][x][2] = roughness;
                        block[y][x][3] = 255;
                    }
                src += blockSize;

                // Write pixels
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

    // BC2 (DXT3) isn't supported by Remix natively — decompress to RGBA8
    DecompressBC2(extracted);

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

// Extract emissive data from a shape's shader property and material
static void ExtractEmissiveData(BSTriShape* shape, BSLightingShaderMaterialBase* lightingMat,
                                ID3D11Device* device, std::vector<ExtractedTexture>& newTextures,
                                uint64_t& outTexHash, float& outR, float& outG, float& outB, float& outIntensity)
{
    outTexHash = 0;
    outR = outG = outB = 0.0f;
    outIntensity = 0.0f;

    if (!shape || !lightingMat) return;

    // 1. Glow map texture from BSLightingShaderMaterialGlowmap
    if (g_config.emissiveGlowMapsEnabled &&
        lightingMat->GetType() == BSLightingShaderMaterialBase::kType_Glowmap)
    {
        auto* glowMat = static_cast<BSLightingShaderMaterialGlowmap*>(lightingMat);
        if (glowMat->spGlowMapTexture) {
            outTexHash = ExtractMaterialTexture(glowMat->spGlowMapTexture, "emissive", device, newTextures);
        }
    }

    // 2. Emissive color/scale from BSLightingShaderProperty
    if (g_config.emissiveColorEnabled) {
        NiProperty* prop = shape->shaderProperty;
        if (prop) {
            BSShaderProperty* shaderProp = static_cast<BSShaderProperty*>(prop);
            // Check EmitColor flag
            if (shaderProp->flags & BSShaderProperty::kShaderFlags_EmitColor) {
                BSLightingShaderProperty* lightingProp = static_cast<BSLightingShaderProperty*>(shaderProp);
                if (lightingProp->pEmissiveColor) {
                    outR = lightingProp->pEmissiveColor->r;
                    outG = lightingProp->pEmissiveColor->g;
                    outB = lightingProp->pEmissiveColor->b;
                }
                outIntensity = lightingProp->fEmitColorScale;
            }
        }
    }

    if (g_config.logEmissive && (outTexHash != 0 || outIntensity > 0.0f)) {
        const char* shapeName = shape->m_name.c_str();
        _MESSAGE("FO4RemixPlugin: [EMISSIVE] Shape \"%s\" glowTex=0x%016llX color=(%.3f,%.3f,%.3f) scale=%.3f",
                 shapeName ? shapeName : "<null>",
                 (unsigned long long)outTexHash, outR, outG, outB, outIntensity);
    }
}

// ---------------------------------------------------------------------------
// Step 3: Read BSSkin data from a BSGeometry node (raw pointer access)
// ---------------------------------------------------------------------------
static bool ReadSkinData(uintptr_t bsGeometryPtr, ExtractedSkinnedMesh& out) {
    // 1. Read BSSkin::Instance pointer from BSGeometry+0x140
    uintptr_t skinInstPtr = *reinterpret_cast<uintptr_t*>(bsGeometryPtr + 0x140);
    if (skinInstPtr == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] skinInstance is null at BSGeometry 0x%p", (void*)bsGeometryPtr);
        return false;
    }

    // 2. Read bone count from boneNodes BSTArray at skinInst+0x10
    //    BSTArray: data* at +0x00, capacity(uint32) at +0x08, count(uint32) at +0x10
    uintptr_t boneNodesArrayPtr = *reinterpret_cast<uintptr_t*>(skinInstPtr + 0x10);
    uint32_t boneCount = *reinterpret_cast<uint32_t*>(skinInstPtr + 0x10 + 0x10);

    if (boneCount == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] boneCount is 0 at skinInst 0x%p", (void*)skinInstPtr);
        return false;
    }
    if (boneCount > kMaxBonesPerSkeleton) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] boneCount %u exceeds max %u at skinInst 0x%p",
                 boneCount, kMaxBonesPerSkeleton, (void*)skinInstPtr);
        return false;
    }
    if (boneNodesArrayPtr == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] boneNodesArray is null at skinInst 0x%p", (void*)skinInstPtr);
        return false;
    }

    // 3. Read boneData pointer from skinInst+0x40
    uintptr_t boneDataPtr = *reinterpret_cast<uintptr_t*>(skinInstPtr + 0x40);
    if (boneDataPtr == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] boneData is null at skinInst 0x%p", (void*)skinInstPtr);
        return false;
    }

    // 4. Read skeletonRoot pointer from skinInst+0x48
    uintptr_t skelRootPtr = *reinterpret_cast<uintptr_t*>(skinInstPtr + 0x48);
    out.skeletonRootPtr = skelRootPtr;

    // 5. Read inverse bind poses from boneData+0x10 NiTArray
    //    Array data pointer at boneData+0x10+0x00
    //    Each entry is 0x50 bytes: 0x10 NiBound + 0x40 NiTransform
    uintptr_t invBindArrayPtr = *reinterpret_cast<uintptr_t*>(boneDataPtr + 0x10);
    if (invBindArrayPtr == 0) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] invBindArray is null at boneData 0x%p", (void*)boneDataPtr);
        return false;
    }

    out.boneCount = boneCount;
    out.inverseBindPoses.resize(boneCount);
    out.boneNodePtrs.resize(boneCount);
    out.boneWorldTransformPtrs.resize(boneCount);

    // 6. Read boneWorldTransforms array at skinInst+0x28 (BSTArray)
    //    This array ALWAYS has valid pointers for every bone, even when boneNodes has nulls.
    //    For bones with NiNodes: points to NiAVObject+0x70 (the worldTransform)
    //    For bones without NiNodes (BSFlattenedBoneTree): points to the flat bone array entry
    uintptr_t boneWorldTransformArrayPtr = *reinterpret_cast<uintptr_t*>(skinInstPtr + 0x28);

    uint32_t nullBoneCount = 0;
    uint32_t nullTransformCount = 0;
    for (uint32_t i = 0; i < boneCount; i++) {
        // Inverse bind pose: skip 0x10 NiBound, read 0x40 NiTransform
        uintptr_t entryPtr = invBindArrayPtr + i * 0x50 + 0x10;
        memcpy(&out.inverseBindPoses[i], reinterpret_cast<void*>(entryPtr), sizeof(NiTransformPadded));

        // Bone node pointer (may be null for BSFlattenedBoneTree bones)
        uintptr_t boneNodePtr = reinterpret_cast<uintptr_t*>(boneNodesArrayPtr)[i];
        out.boneNodePtrs[i] = boneNodePtr;
        if (boneNodePtr == 0) nullBoneCount++;

        // Bone world transform pointer (always valid)
        uintptr_t transformPtr = boneWorldTransformArrayPtr
            ? reinterpret_cast<uintptr_t*>(boneWorldTransformArrayPtr)[i]
            : 0;
        out.boneWorldTransformPtrs[i] = transformPtr;
        if (transformPtr == 0) nullTransformCount++;
    }

    // Pre-allocate per-frame transform storage
    out.currentBoneTransforms.resize(boneCount);

    _MESSAGE("FO4RemixPlugin: [SKINNING] ReadSkinData OK: bones=%u skelRoot=0x%p nullNodes=%u nullTransforms=%u",
             boneCount, (void*)skelRootPtr, nullBoneCount, nullTransformCount);

    // Log first 3 bones for debug
    for (uint32_t i = 0; i < boneCount && i < 3; i++) {
        const auto& ib = out.inverseBindPoses[i];
        _MESSAGE("FO4RemixPlugin: [SKINNING]   Bone[%u]: node=0x%p invBind rot=[%.3f,%.3f,%.3f / %.3f,%.3f,%.3f / %.3f,%.3f,%.3f] "
                 "trans=[%.1f,%.1f,%.1f] scale=%.3f",
                 i, (void*)out.boneNodePtrs[i],
                 ib.rot[0][0], ib.rot[0][1], ib.rot[0][2],
                 ib.rot[1][0], ib.rot[1][1], ib.rot[1][2],
                 ib.rot[2][0], ib.rot[2][1], ib.rot[2][2],
                 ib.translate[0], ib.translate[1], ib.translate[2], ib.scale);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Step 5: Bone transform computation helpers
// ---------------------------------------------------------------------------

// Read an NiAVObject's world transform from game memory (at +0x70)
static NiTransformPadded ReadWorldTransform(uintptr_t niAVObjectPtr) {
    NiTransformPadded t;
    memcpy(&t, reinterpret_cast<void*>(niAVObjectPtr + 0x70), sizeof(NiTransformPadded));
    return t;
}

// Compute: result = boneWorld * invBind  (affine 3x4 matrix multiply)
// Both inputs are NiTransformPadded; output is a 3x4 row-major matrix for Remix.
static void ComputeBoneMatrix(
    const NiTransformPadded& boneWorld,
    const NiTransformPadded& invBind,
    float outMatrix[3][4])
{
    // Build effective 3x3 for boneWorld: rot[r][c] * scale
    float A[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            A[r][c] = boneWorld.rot[r][c] * boneWorld.scale;

    // Build effective 3x3 for invBind: rot[r][c] * scale
    float B[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            B[r][c] = invBind.rot[r][c] * invBind.scale;

    // Result rotation = A * B (3x3 matrix multiply)
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            outMatrix[r][c] = A[r][0] * B[0][c]
                            + A[r][1] * B[1][c]
                            + A[r][2] * B[2][c];
        }
    }

    // Result translation = A * invBind.translate + boneWorld.translate
    for (int r = 0; r < 3; r++) {
        outMatrix[r][3] = A[r][0] * invBind.translate[0]
                        + A[r][1] * invBind.translate[1]
                        + A[r][2] * invBind.translate[2]
                        + boneWorld.translate[r];
    }
}

// Apply the same coordinate system swap as static meshes and the camera.
// Both use a column swap on rotation (R*S) and component swap on translation (S*T).
// This must match exactly or skinned meshes will rotate incorrectly relative to the world.
static void ApplyCoordinateSwap(float matrix[3][4]) {
    // Swap COLUMNS 0 and 1 in the 3x3 rotation part (= right-multiply by swap matrix S).
    // This matches the static mesh path: worldTransform[r][0] = rot[r][1], [r][1] = rot[r][0].
    for (int r = 0; r < 3; r++) {
        std::swap(matrix[r][0], matrix[r][1]);
    }
    // Swap X/Y in translation, same as static meshes: pos.y → [0][3], pos.x → [1][3].
    std::swap(matrix[0][3], matrix[1][3]);
}

// Diagnostic: dump bone 0 matrices for the first skinned mesh, then auto-disable.
// Fires every ~120 frames while logBoneDiag is true, so you can capture multiple orientations.
static void DumpBoneDiagnostic(const NiTransformPadded& boneWorld,
                                const NiTransformPadded& invBind,
                                const float preSwap[3][4],
                                const float postSwap[3][4]) {
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] ======== Bone 0 Diagnostic ========");
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] boneWorld rot:");
    for (int r = 0; r < 3; r++)
        _MESSAGE("FO4RemixPlugin: [BONE_DIAG]   [%d] %+.6f %+.6f %+.6f", r,
                 boneWorld.rot[r][0], boneWorld.rot[r][1], boneWorld.rot[r][2]);
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] boneWorld translate: %+.3f %+.3f %+.3f  scale: %.4f",
             boneWorld.translate[0], boneWorld.translate[1], boneWorld.translate[2], boneWorld.scale);

    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] invBind rot:");
    for (int r = 0; r < 3; r++)
        _MESSAGE("FO4RemixPlugin: [BONE_DIAG]   [%d] %+.6f %+.6f %+.6f", r,
                 invBind.rot[r][0], invBind.rot[r][1], invBind.rot[r][2]);
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] invBind translate: %+.3f %+.3f %+.3f  scale: %.4f",
             invBind.translate[0], invBind.translate[1], invBind.translate[2], invBind.scale);

    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] computed (pre-swap):");
    for (int r = 0; r < 3; r++)
        _MESSAGE("FO4RemixPlugin: [BONE_DIAG]   [%d] %+.6f %+.6f %+.6f | %+.3f", r,
                 preSwap[r][0], preSwap[r][1], preSwap[r][2], preSwap[r][3]);

    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] final (post-swap):");
    for (int r = 0; r < 3; r++)
        _MESSAGE("FO4RemixPlugin: [BONE_DIAG]   [%d] %+.6f %+.6f %+.6f | %+.3f", r,
                 postSwap[r][0], postSwap[r][1], postSwap[r][2], postSwap[r][3]);
    _MESSAGE("FO4RemixPlugin: [BONE_DIAG] ====================================");
}

// Called every frame to update bone transforms for all tracked skinned meshes.
void SceneExtractor::UpdateSkinnedBoneTransforms(std::vector<ExtractedSkinnedMesh>& skinnedMeshes) {
    static uint32_t s_diagFrameCounter = 0;
    bool doDiag = g_config.logBoneDiag && !skinnedMeshes.empty();
    bool diagThisFrame = doDiag && (s_diagFrameCounter++ % 120 == 0);

    for (auto& sm : skinnedMeshes) {
        // Safety: validate array sizes match boneCount
        if (sm.boneWorldTransformPtrs.size() < sm.boneCount ||
            sm.inverseBindPoses.size() < sm.boneCount ||
            sm.currentBoneTransforms.size() < sm.boneCount) {
            continue;
        }

        if (sm.boneCount == 0 || sm.boneCount > kMaxBonesPerSkeleton) {
            continue;
        }

        // Skeleton root validity check
        if (sm.skeletonRootPtr != 0) {
            __try {
                volatile uint8_t probe = *reinterpret_cast<uint8_t*>(sm.skeletonRootPtr);
                (void)probe;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
        }

        for (uint32_t i = 0; i < sm.boneCount; i++) {
            float boneMatrix[3][4];

            // Use boneWorldTransformPtrs -- always valid for every bone,
            // even when boneNodePtrs[i] is null (BSFlattenedBoneTree).
            // Each pointer points directly to a NiTransformPadded (0x40 bytes).
            uintptr_t transformPtr = sm.boneWorldTransformPtrs[i];

            if (transformPtr == 0) {
                // Fallback: identity (should never happen with boneWorldTransforms)
                memset(boneMatrix, 0, sizeof(boneMatrix));
                boneMatrix[0][0] = 1.0f;
                boneMatrix[1][1] = 1.0f;
                boneMatrix[2][2] = 1.0f;
            } else {
                __try {
                    // Read the bone's current world transform directly
                    NiTransformPadded boneWorld;
                    memcpy(&boneWorld, reinterpret_cast<void*>(transformPtr), sizeof(NiTransformPadded));

                    ComputeBoneMatrix(boneWorld, sm.inverseBindPoses[i], boneMatrix);

                    // Diagnostic: capture pre-swap matrix for bone 0 of first mesh
                    if (diagThisFrame && i == 0 && &sm == &skinnedMeshes[0]) {
                        float preSwap[3][4];
                        memcpy(preSwap, boneMatrix, sizeof(preSwap));
                        ApplyCoordinateSwap(boneMatrix);
                        DumpBoneDiagnostic(boneWorld, sm.inverseBindPoses[0], preSwap, boneMatrix);
                    } else {
                        ApplyCoordinateSwap(boneMatrix);
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    memset(boneMatrix, 0, sizeof(boneMatrix));
                    boneMatrix[0][0] = 1.0f;
                    boneMatrix[1][1] = 1.0f;
                    boneMatrix[2][2] = 1.0f;
                    sm.boneWorldTransformPtrs[i] = 0;
                }
            }

            memcpy(sm.currentBoneTransforms[i].data(), boneMatrix, 12 * sizeof(float));
        }
    }
}

// Helper: dump BSDynamicTriShape memory layout (SEH-safe, no C++ objects)
static void DumpDynamicShapeLayout(uintptr_t shapeAddr, uint32_t szVertex) {
    _MESSAGE("FO4RemixPlugin: [DYNAMIC] shape=0x%p szVertex=%u probing offsets 0x170-0x1A8:",
             (void*)shapeAddr, szVertex);
    __try {
        for (uint32_t off = 0x170; off <= 0x1A8; off += 8) {
            uintptr_t val = *reinterpret_cast<uintptr_t*>(shapeAddr + off);
            _MESSAGE("FO4RemixPlugin: [DYNAMIC]   +0x%03X = 0x%016llX", off, val);
        }
        for (uint32_t off = 0x170; off <= 0x1A0; off += 4) {
            uint32_t val32 = *reinterpret_cast<uint32_t*>(shapeAddr + off);
            _MESSAGE("FO4RemixPlugin: [DYNAMIC]   +0x%03X (u32) = %u (0x%08X)", off, val32, val32);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   exception reading shape memory");
    }
}

static void DumpDynamicVertexData(uint8_t* dynVerts, uint32_t posStride) {
    _MESSAGE("FO4RemixPlugin: [DYNAMIC] dynVerts=0x%p stride=%u first 16 bytes:", dynVerts, posStride);
    __try {
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 dynVerts[0], dynVerts[1], dynVerts[2], dynVerts[3],
                 dynVerts[4], dynVerts[5], dynVerts[6], dynVerts[7],
                 dynVerts[8], dynVerts[9], dynVerts[10], dynVerts[11],
                 dynVerts[12], dynVerts[13], dynVerts[14], dynVerts[15]);
        uint16_t* asHalf = reinterpret_cast<uint16_t*>(dynVerts);
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   as half: %.3f %.3f %.3f %.3f",
                 HalfToFloat(asHalf[0]), HalfToFloat(asHalf[1]),
                 HalfToFloat(asHalf[2]), HalfToFloat(asHalf[3]));
        float* asFloat = reinterpret_cast<float*>(dynVerts);
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   as f32:  %.3f %.3f %.3f %.3f",
                 asFloat[0], asFloat[1], asFloat[2], asFloat[3]);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        _MESSAGE("FO4RemixPlugin: [DYNAMIC]   exception reading dynVerts data");
    }
}

// ---------------------------------------------------------------------------
// Step 4: Extract a skinned BSTriShape — base mesh + blend weights/indices + BSSkin
// ---------------------------------------------------------------------------
static bool ExtractSkinnedTriShape(BSTriShape* shape, uint64_t baseHash,
                                   std::vector<ExtractedSkinnedMesh>& out,
                                   ID3D11Device* device,
                                   std::vector<ExtractedTexture>& newTextures,
                                   uint32_t ownerFormID)
{
    if (!shape || shape->numVertices == 0 || shape->numTriangles == 0)
        return false;

    // Skip effect shaders — not real geometry
    {
        NiProperty* prop = shape->shaderProperty;
        if (prop) {
            BSShaderProperty* sp = static_cast<BSShaderProperty*>(prop);
            BSShaderMaterial* mat = sp->shaderMaterial;
            if (!mat || mat->GetFeature() != 2)
                return false;
        }
    }

    // Renderer data -> vertex/index buffers
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

    bool hasUVs     = (desc & BSGeometry::kFlag_UVs) != 0;
    bool hasNormals = (desc & BSGeometry::kFlag_Normals) != 0;
    bool hasColors  = (desc & BSGeometry::kFlag_VertexColors) != 0;

    uint32_t szVertex = (desc >> 4) & 0xF;
    uint32_t oUV     = (szVertex + ((desc >>  8) & 0xF)) * 4;
    uint32_t oNormal = (szVertex + ((desc >> 16) & 0xF)) * 4;
    uint32_t oColor  = (szVertex + ((desc >> 24) & 0xF)) * 4;

    bool posHalfFloat = !(desc & BSGeometry::kFlag_FullPrecision);

    // BSDynamicTriShape detection: szVertex (bits [7:4]) is non-zero for dynamic shapes.
    // Face/head/hair meshes are BSDynamicTriShape — morphed positions live in
    // dynamicVertices, not in pVB->pData.
    // Dynamic buffer always uses half-float positions (matching static VB format).
    uint8_t* posData = vbData;
    uint32_t posStride = vertexSize;
    bool isDynamic = (szVertex != 0);
    if (isDynamic) {
        uintptr_t shapeAddr = reinterpret_cast<uintptr_t>(shape);

        // Dump BSDynamicTriShape memory layout (first dynamic mesh only)
        static bool s_dumpedOnce = false;
        if (!s_dumpedOnce) {
            s_dumpedOnce = true;
            DumpDynamicShapeLayout(shapeAddr, szVertex);
        }

        // Try offset 0x180 (F4SE standard for dynamicVertices)
        uint8_t* dynVerts = *reinterpret_cast<uint8_t**>(shapeAddr + 0x180);
        if (dynVerts) {
            posData = dynVerts;
            posStride = szVertex * 4;
            DumpDynamicVertexData(dynVerts, posStride);
        } else {
            isDynamic = false;
        }
    }

    // Step 2: Compute blend weight/index offsets
    uint32_t blendWeightOffset = (uint32_t)((desc >> 26) & 0x3C);
    uint32_t blendIndexOffset  = blendWeightOffset + 8;

    const char* shapeName = shape->m_name.c_str();

    _MESSAGE("FO4RemixPlugin: [SKINNING] Extracting skinned shape \"%s\" verts=%u tris=%u bones offset: weight=%u index=%u dynamic=%d",
             shapeName ? shapeName : "<null>",
             shape->numVertices, shape->numTriangles,
             blendWeightOffset, blendIndexOffset, isDynamic);

    ExtractedSkinnedMesh sm;
    sm.ownerFormID = ownerFormID;
    sm.vertexCount = shape->numVertices;
    sm.indexCount = shape->numTriangles * 3;

    // Generate a unique mesh hash (include ownerFormID for per-REFR uniqueness)
    sm.hash = FnvHashCombine(baseHash, FnvHash(shapeName ? shapeName : ""));
    sm.hash = FnvHashCombine(sm.hash, (uint64_t)0x534B494EULL); // "SKIN" tag

    // ---- Extract base vertex data (positions, normals, UVs, colors) ----
    sm.vertices.resize(shape->numVertices);

    for (uint16_t i = 0; i < shape->numVertices; i++) {
        uint8_t* v = vbData + (uint32_t)i * vertexSize;
        remixapi_HardcodedVertex& out_v = sm.vertices[i];
        memset(&out_v, 0, sizeof(out_v));

        // Position: read from dynamic buffer for BSDynamicTriShape, static VB otherwise.
        // Dynamic buffer always stores HALF4 positions (verified via memory dump), even when
        // kFlag_FullPrecision is set — because szVertex serves double-duty as both the
        // dynamic vertex size AND the attribute offset base for non-dynamic shapes.
        uint8_t* pv = posData + (uint32_t)i * posStride;
        if (isDynamic || posHalfFloat) {
            uint16_t* pos = reinterpret_cast<uint16_t*>(pv);
            out_v.position[0] = HalfToFloat(pos[0]);
            out_v.position[1] = HalfToFloat(pos[1]);
            out_v.position[2] = HalfToFloat(pos[2]);
        } else {
            float* pos = reinterpret_cast<float*>(pv);
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

        // Normals
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

        // Color
        if (hasColors && oColor + 4 <= vertexSize) {
            memcpy(&out_v.color, v + oColor, 4);
        } else {
            out_v.color = 0xFFFFFFFF;
        }
    }

    // Validate vertex positions and compute bounds
    float sMinPos[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float sMaxPos[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint16_t i = 0; i < shape->numVertices; i++) {
        const auto& pos = sm.vertices[i].position;
        for (int j = 0; j < 3; j++) {
            if (std::isnan(pos[j]) || std::isinf(pos[j])) {
                _MESSAGE("FO4RemixPlugin: [SKINNING] Rejecting skinned mesh \"%s\" - vertex %u has NaN/Inf position (%.3f, %.3f, %.3f)",
                         shapeName ? shapeName : "<null>", i, pos[0], pos[1], pos[2]);
                return false;
            }
            if (pos[j] < sMinPos[j]) sMinPos[j] = pos[j];
            if (pos[j] > sMaxPos[j]) sMaxPos[j] = pos[j];
        }
    }
    _MESSAGE("FO4RemixPlugin: [SKINNING] Mesh \"%s\" obj-space bounds: (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) extent=(%.1f,%.1f,%.1f)",
             shapeName ? shapeName : "<null>",
             sMinPos[0], sMinPos[1], sMinPos[2],
             sMaxPos[0], sMaxPos[1], sMaxPos[2],
             sMaxPos[0]-sMinPos[0], sMaxPos[1]-sMinPos[1], sMaxPos[2]-sMinPos[2]);

    // ---- Indices (uint16 -> uint32) ----
    uint32_t indexCount = shape->numTriangles * 3;
    uint16_t* indices16 = reinterpret_cast<uint16_t*>(ibData);
    sm.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; i++) {
        uint32_t idx = indices16[i];
        if (idx >= shape->numVertices) {
            _MESSAGE("FO4RemixPlugin: [SKINNING] Rejecting skinned mesh \"%s\" - index[%u]=%u >= numVertices=%u",
                     shapeName ? shapeName : "<null>", i, idx, shape->numVertices);
            return false;
        }
        sm.indices[i] = idx;
    }

    // ---- Step 2: Extract blend weights and blend indices ----
    sm.blendWeights.resize(sm.vertexCount * kBonesPerVertex);
    sm.blendIndices.resize(sm.vertexCount * kBonesPerVertex);

    float minWeightSum = FLT_MAX, maxWeightSum = -FLT_MAX;
    double totalWeightSum = 0.0;
    uint32_t badWeightCount = 0;
    uint32_t maxBoneIdx = 0;

    for (uint32_t i = 0; i < sm.vertexCount; i++) {
        uint32_t base = i * kBonesPerVertex;
        uint8_t* v = vbData + (uint32_t)i * vertexSize;

        // Read blend weights (HALF4)
        const uint16_t* hw = reinterpret_cast<const uint16_t*>(v + blendWeightOffset);
        float w0 = HalfToFloat(hw[0]);
        float w1 = HalfToFloat(hw[1]);
        float w2 = HalfToFloat(hw[2]);
        float w3 = 1.0f - w0 - w1 - w2;  // 4th weight is implicit
        if (w3 < 0.0f) w3 = 0.0f;

        sm.blendWeights[base + 0] = w0;
        sm.blendWeights[base + 1] = w1;
        sm.blendWeights[base + 2] = w2;
        sm.blendWeights[base + 3] = w3;

        // Validate weight sum
        float wSum = w0 + w1 + w2 + w3;
        if (wSum < minWeightSum) minWeightSum = wSum;
        if (wSum > maxWeightSum) maxWeightSum = wSum;
        totalWeightSum += wSum;
        if (fabsf(wSum - 1.0f) > 0.01f) badWeightCount++;

        // Read blend indices (R8G8B8A8)
        const uint8_t* bi = v + blendIndexOffset;
        sm.blendIndices[base + 0] = bi[0];
        sm.blendIndices[base + 1] = bi[1];
        sm.blendIndices[base + 2] = bi[2];
        sm.blendIndices[base + 3] = bi[3];

        for (int j = 0; j < 4; j++) {
            if (bi[j] > maxBoneIdx) maxBoneIdx = bi[j];
        }
    }

    _MESSAGE("FO4RemixPlugin: [SKINNING] Vertex weight stats: min_sum=%.4f max_sum=%.4f avg_sum=%.4f bad_count=%u/%u",
             minWeightSum, maxWeightSum, (float)(totalWeightSum / sm.vertexCount),
             badWeightCount, sm.vertexCount);
    _MESSAGE("FO4RemixPlugin: [SKINNING] Bone index range: [0, %u]", maxBoneIdx);

    // ---- Step 3: Read BSSkin data ----
    if (!ReadSkinData(reinterpret_cast<uintptr_t>(shape), sm)) {
        _MESSAGE("FO4RemixPlugin: [SKINNING] ReadSkinData failed for \"%s\"",
                 shapeName ? shapeName : "<null>");
        return false;
    }

    // Validate bone indices against actual bone count
    for (uint32_t i = 0; i < sm.vertexCount * kBonesPerVertex; i++) {
        if (sm.blendIndices[i] >= sm.boneCount) {
            _MESSAGE("FO4RemixPlugin: [SKINNING] Bone index %u >= boneCount %u, clamping to 0",
                     sm.blendIndices[i], sm.boneCount);
            sm.blendIndices[i] = 0;
        }
    }

    // ---- Extract textures ----
    BSLightingShaderMaterialBase* lightingMat = GetLightingMaterial(shape);
    sm.diffuseTextureHash   = lightingMat ? ExtractMaterialTexture(lightingMat->spDiffuseTexture, "diffuse", device, newTextures) : 0;
    sm.normalTextureHash    = lightingMat ? ExtractMaterialTexture(lightingMat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral) : 0;
    sm.roughnessTextureHash = lightingMat ? ExtractMaterialTexture(lightingMat->spSmoothnessSpecMaskTexture, "roughness", device, newTextures, TexturePostProcess::InvertRGB) : 0;

    // Extract emissive data (glow map texture + emissive color/scale)
    ExtractEmissiveData(shape, lightingMat, device, newTextures,
                        sm.emissiveTextureHash, sm.emissiveColorR, sm.emissiveColorG,
                        sm.emissiveColorB, sm.emissiveIntensity);

    // ---- Extract alpha test state ----
    sm.alphaTestEnabled = false;
    sm.alphaTestType = 7;
    sm.alphaTestRef = 128;
    NiProperty* alphaPropRaw = shape->effectState;
    if (alphaPropRaw) {
        NiAlphaProperty* alphaProp = static_cast<NiAlphaProperty*>(alphaPropRaw);
        bool testEnabled = (alphaProp->alphaFlags >> 9) & 1;
        if (testEnabled) {
            int niTestFunc = (alphaProp->alphaFlags >> 10) & 7;
            static const int niToVk[] = { 7, 1, 2, 3, 4, 5, 6, 0 };
            sm.alphaTestEnabled = true;
            sm.alphaTestType = niToVk[niTestFunc];
            sm.alphaTestRef = alphaProp->alphaThreshold;
        }
    }

    _MESSAGE("FO4RemixPlugin: [SKINNING] Extracted skinned mesh: hash=0x%016llX owner=0x%08X bones=%u vertices=%u indices=%u",
             (unsigned long long)sm.hash, ownerFormID, sm.boneCount, sm.vertexCount, sm.indexCount);

    out.push_back(std::move(sm));
    return true;
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

    // Skip effect shaders (god rays, glows, particles, etc.) — not real geometry
    {
        NiProperty* prop = shape->shaderProperty;
        if (prop) {
            BSShaderProperty* sp = static_cast<BSShaderProperty*>(prop);
            BSShaderMaterial* mat = sp->shaderMaterial;
            if (!mat || mat->GetFeature() != 2) // 2 = BSLightingShaderMaterialBase
                return false;
        }
    }

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

    // BSDynamicTriShape: morphed positions in dynamicVertices (offset 0x180)
    uint8_t* posData = vbData;
    uint32_t posStride = vertexSize;
    bool isDynamic = (szVertex != 0);
    if (isDynamic) {
        uint8_t* dynVerts = *reinterpret_cast<uint8_t**>(
            reinterpret_cast<uintptr_t>(shape) + 0x180);
        if (dynVerts) {
            posData = dynVerts;
            posStride = szVertex * 4;
        } else {
            isDynamic = false;
        }
    }

    const char* shapeName = shape->m_name.c_str();
    if (g_config.logShapeInfo) {
        _MESSAGE("FO4RemixPlugin: Shape \"%s\" vertexSize=%u desc=0x%016llX posHalf=%d "
                 "flags: UV=%d Norm=%d Color=%d FullPrec=%d Skinned=%d dynamic=%d verts=%u tris=%u",
                 shapeName ? shapeName : "<null>",
                 vertexSize, desc, posHalfFloat,
                 hasUVs, hasNormals, hasColors,
                 (desc & BSGeometry::kFlag_FullPrecision) ? 1 : 0,
                 (desc & BSGeometry::kFlag_Skinned) ? 1 : 0,
                 isDynamic,
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

        // Position: dynamic buffer always stores HALF4 regardless of FullPrecision flag
        uint8_t* pv = posData + (uint32_t)i * posStride;
        if (isDynamic || posHalfFloat) {
            uint16_t* pos = reinterpret_cast<uint16_t*>(pv);
            out_v.position[0] = HalfToFloat(pos[0]);
            out_v.position[1] = HalfToFloat(pos[1]);
            out_v.position[2] = HalfToFloat(pos[2]);
        } else {
            float* pos = reinterpret_cast<float*>(pv);
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
    if (lightingMat && lightingMat->GetType() == BSLightingShaderMaterialBase::kType_Landscape) {
        // Landscape materials store textures in per-layer arrays, not the base class fields.
        auto* landMat = static_cast<BSLightingShaderMaterialLandscape*>(lightingMat);
        mesh.diffuseTextureHash   = ExtractMaterialTexture(landMat->spLandscapeDiffuseTexture[0], "diffuse", device, newTextures);
        mesh.normalTextureHash    = ExtractMaterialTexture(landMat->spLandscapeNormalTexture[0], "normal", device, newTextures, TexturePostProcess::Octahedral);
        mesh.roughnessTextureHash = ExtractMaterialTexture(landMat->spLandscapeSmoothSpecTexture[0], "roughness", device, newTextures, TexturePostProcess::InvertRGB);
    } else {
        mesh.diffuseTextureHash   = lightingMat ? ExtractMaterialTexture(lightingMat->spDiffuseTexture, "diffuse", device, newTextures) : 0;
        mesh.normalTextureHash    = lightingMat ? ExtractMaterialTexture(lightingMat->spNormalTexture, "normal", device, newTextures, TexturePostProcess::Octahedral) : 0;
        mesh.roughnessTextureHash = lightingMat ? ExtractMaterialTexture(lightingMat->spSmoothnessSpecMaskTexture, "roughness", device, newTextures, TexturePostProcess::InvertRGB) : 0;
    }

    // Extract emissive data (glow map texture + emissive color/scale)
    ExtractEmissiveData(shape, lightingMat, device, newTextures,
                        mesh.emissiveTextureHash, mesh.emissiveColorR, mesh.emissiveColorG,
                        mesh.emissiveColorB, mesh.emissiveIntensity);

    // Extract alpha test state from NiAlphaProperty (effectState on BSGeometry)
    mesh.alphaTestEnabled = false;
    mesh.alphaTestType = 7; // Always (no test)
    mesh.alphaTestRef = 128;
    NiProperty* alphaPropRaw = shape->effectState;
    if (alphaPropRaw) {
        NiAlphaProperty* alphaProp = static_cast<NiAlphaProperty*>(alphaPropRaw);
        bool testEnabled = (alphaProp->alphaFlags >> 9) & 1;
        if (testEnabled) {
            int niTestFunc = (alphaProp->alphaFlags >> 10) & 7;
            // Map NiAlphaProperty::TestFunction to Remix/VkCompareOp
            // NI:  Always=0, Less=1, Equal=2, LessEq=3, Greater=4, NotEq=5, GreaterEq=6, Never=7
            // VK:  Never=0,  Less=1, Equal=2, LessEq=3, Greater=4, NotEq=5, GreaterEq=6, Always=7
            static const int niToVk[] = { 7, 1, 2, 3, 4, 5, 6, 0 };
            mesh.alphaTestEnabled = true;
            mesh.alphaTestType = niToVk[niTestFunc];
            mesh.alphaTestRef = alphaProp->alphaThreshold;
        }
    }

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
                     std::vector<ExtractedSkinnedMesh>* skinnedOut = nullptr,
                     uint32_t ownerFormID = 0,
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
        // Check skinned flag and route accordingly
        uint64_t vertexDesc = tri->vertexDesc;
        if ((vertexDesc & kVertexFlag_Skinned) && skinnedOut && g_config.skinningEnabled) {
            ExtractSkinnedTriShape(tri, baseHash, *skinnedOut, device, newTextures, ownerFormID);
        } else {
            ExtractTriShape(tri, baseHash, out, device, newTextures);
        }
        return; // BSTriShape is a leaf -- no children
    }

    // If it's an NiNode, recurse into children
    NiNode* node = obj->GetAsNiNode();
    if (node) {
        for (uint16_t i = 0; i < node->m_children.m_emptyRunStart; i++) {
            NiAVObject* child = node->m_children.m_data[i];
            if (child) {
                WalkNode(child, baseHash, out, device, newTextures, skinnedOut, ownerFormID, depth + 1);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Return the player's current parent cell pointer (0 if unavailable)
// ---------------------------------------------------------------------------
uintptr_t SceneExtractor::GetPlayerCellPtr()
{
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) return 0;
    uintptr_t player = *ppPlayer;
    return *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
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
// Return all cells currently loaded by the engine (from DataHandler::cellList)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Read exterior grid cells from the TES singleton's GridCellArray.
//
// TES singleton (RelocPtr at 0x032D2048) has GridCellArray* at +0x18.
// GridCellArray has:
//   +0x10: int32 gridDimension  (= uGridsToLoad, typically 5)
//   +0x18: TESObjectCELL**      (flat dim*dim array of cell pointers)
//
// We iterate the flat array and collect cells that have loaded 3D.
// ---------------------------------------------------------------------------
static void CollectGridCells(
    std::vector<CellInfo>& result,
    std::unordered_set<uintptr_t>& seen)
{
    uintptr_t* ppTES = reinterpret_cast<uintptr_t*>(s_g_tes.GetPtr());
    if (!ppTES || !*ppTES) return;
    uintptr_t tes = *ppTES;

    uintptr_t gridPtr = *reinterpret_cast<uintptr_t*>(tes + OFF_TES_GRID_CELLS);
    if (!gridPtr) return;

    int32_t dim = *reinterpret_cast<int32_t*>(gridPtr + OFF_GRID_DIMENSION);
    if (dim <= 0 || dim > 11) return;  // sanity check (uGridsToLoad is 3-11)

    uintptr_t* cellArray = *reinterpret_cast<uintptr_t**>(gridPtr + OFF_GRID_CELL_ARRAY);
    if (!cellArray) return;

    int32_t total = dim * dim;
    for (int32_t i = 0; i < total; i++) {
        uintptr_t cellPtr = cellArray[i];
        if (!cellPtr) continue;
        if (seen.count(cellPtr)) continue;

        // Verify the cell has objects with loaded 3D
        struct SimpleArray {
            uintptr_t* entries;
            uint32_t capacity;
            uint32_t pad0C;
            uint32_t count;
        };
        auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);
        if (!objectList.entries || objectList.count == 0) continue;

        bool hasLoaded3D = false;
        uint32_t limit = objectList.count < 32u ? objectList.count : 32u;
        for (uint32_t j = 0; j < limit; j++) {
            uintptr_t refrPtr = objectList.entries[j];
            if (!refrPtr) continue;
            uintptr_t ld = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_LOADED_DATA);
            if (!ld) continue;
            NiNode* rn = *reinterpret_cast<NiNode**>(ld + OFF_LOADED_ROOT_NODE);
            if (rn) { hasLoaded3D = true; break; }
        }
        if (!hasLoaded3D) continue;

        uint32_t formID = *reinterpret_cast<uint32_t*>(cellPtr + OFF_FORM_ID);
        result.push_back({ cellPtr, formID });
        seen.insert(cellPtr);
    }
}

std::vector<CellInfo> SceneExtractor::GetLoadedCells()
{
    std::vector<CellInfo> result;
    std::unordered_set<uintptr_t> seen;

    // Always include the player's parentCell — this is the one cell guaranteed
    // to have loaded 3D.
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) return result;
    uintptr_t player = *ppPlayer;

    uintptr_t playerCell = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
    if (!playerCell) return result;

    uint32_t playerCellFormID = *reinterpret_cast<uint32_t*>(playerCell + OFF_FORM_ID);
    result.push_back({ playerCell, playerCellFormID });
    seen.insert(playerCell);

    // Check if the player is in an exterior worldspace
    uint16_t cellFlags = *reinterpret_cast<uint16_t*>(playerCell + OFF_CELL_FLAGS);
    bool isExterior = (cellFlags & CELL_FLAG_IS_INTERIOR) == 0;

    if (isExterior) {
        // Exterior: read the GridCellArray from the TES singleton.
        // DataHandler::cellList doesn't contain grid-loaded exterior cells.
        CollectGridCells(result, seen);
        return result;
    }

    // Interior: supplement with DataHandler::cellList for attached cells
    uintptr_t* ppDataHandler = reinterpret_cast<uintptr_t*>(s_g_dataHandler.GetPtr());
    if (!ppDataHandler || !*ppDataHandler) return result;
    uintptr_t dh = *ppDataHandler;

    // NiTArray<TESObjectCELL*> cellList at offset 0xF58
    uintptr_t cellListBase = dh + 0xF58;
    uintptr_t* cellData = *reinterpret_cast<uintptr_t**>(cellListBase + 0x08);
    uint16_t emptyRunStart = *reinterpret_cast<uint16_t*>(cellListBase + 0x12);

    if (!cellData || emptyRunStart == 0) return result;

    for (uint16_t i = 0; i < emptyRunStart; i++) {
        uintptr_t cellPtr = cellData[i];
        if (!cellPtr) continue;
        if (seen.count(cellPtr)) continue;

        struct SimpleArray {
            uintptr_t* entries;
            uint32_t capacity;
            uint32_t pad0C;
            uint32_t count;
        };
        auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);
        if (!objectList.entries || objectList.count == 0) continue;

        bool hasLoaded3D = false;
        for (uint32_t j = 0; j < objectList.count; j++) {
            uintptr_t refrPtr = objectList.entries[j];
            if (!refrPtr) continue;
            uintptr_t ld = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_LOADED_DATA);
            if (!ld) continue;
            NiNode* rn = *reinterpret_cast<NiNode**>(ld + OFF_LOADED_ROOT_NODE);
            if (rn) { hasLoaded3D = true; break; }
        }
        if (!hasLoaded3D) continue;

        uint32_t formID = *reinterpret_cast<uint32_t*>(cellPtr + OFF_FORM_ID);
        result.push_back({ cellPtr, formID });
        seen.insert(cellPtr);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Extract all geometry from loaded references in a specific cell
// ---------------------------------------------------------------------------
ExtractionResult SceneExtractor::ExtractCell(uintptr_t cellPtr, ID3D11Device* device)
{
    std::vector<ExtractedMesh> result;
    std::vector<ExtractedSkinnedMesh> skinnedResult;
    std::vector<ExtractedTexture> newTextures;

    if (!cellPtr) {
        return { std::move(result), std::move(skinnedResult), std::move(newTextures) };
    }

    // Access cell objectList (tArray<TESObjectREFR*> at offset 0x70)
    struct SimpleArray {
        uintptr_t* entries;
        uint32_t capacity;
        uint32_t pad0C;
        uint32_t count;
    };
    auto& objectList = *reinterpret_cast<SimpleArray*>(cellPtr + OFF_CELL_OBJECT_LIST);

    uint32_t cellFormID = *reinterpret_cast<uint32_t*>(cellPtr + OFF_FORM_ID);
    _MESSAGE("FO4RemixPlugin: ExtractCell 0x%08X - cell has %u objects", cellFormID, objectList.count);

    uint32_t meshCount = 0;

    for (uint32_t i = 0; i < objectList.count; i++) {
        uintptr_t refrPtr = objectList.entries[i];
        if (!refrPtr) continue;

        uintptr_t loadedData = *reinterpret_cast<uintptr_t*>(refrPtr + OFF_REFR_LOADED_DATA);
        if (!loadedData) continue;

        NiNode* rootNode = *reinterpret_cast<NiNode**>(loadedData + OFF_LOADED_ROOT_NODE);
        if (!rootNode) continue;

        uint32_t refrFormID = *reinterpret_cast<uint32_t*>(refrPtr + OFF_FORM_ID);
        uint64_t baseHash = FnvHashCombine(0xCBF29CE484222325ULL, (uint64_t)refrFormID);

        size_t before = result.size();
        WalkNode(rootNode, baseHash, result, device, newTextures, &skinnedResult, refrFormID);
        meshCount += (uint32_t)(result.size() - before);
    }

    // Extract terrain geometry from LAND quadrant nodes (BSMultiBoundNode).
    // TESObjectLAND is at cell+0x58, its quadrant node array is at LAND+0x40.
    // (Terrain is never skinned, so no skinned output is passed.)
    uint32_t terrainMeshCount = 0;
    uintptr_t landPtr = *reinterpret_cast<uintptr_t*>(cellPtr + OFF_CELL_LAND);
    if (landPtr) {
        uintptr_t* quadrants = *reinterpret_cast<uintptr_t**>(landPtr + OFF_LAND_QUADRANTS);
        if (quadrants) {
            uint32_t landFormID = *reinterpret_cast<uint32_t*>(landPtr + OFF_FORM_ID);
            for (int q = 0; q < LAND_QUADRANT_COUNT; q++) {
                uintptr_t nodePtr = quadrants[q];
                if (!nodePtr) continue;
                NiNode* quadNode = reinterpret_cast<NiNode*>(nodePtr);
                uint64_t terrainHash = FnvHashCombine(0xCBF29CE484222325ULL, (uint64_t)landFormID);
                terrainHash = FnvHashCombine(terrainHash, (uint64_t)q);
                size_t before = result.size();
                WalkNode(quadNode, terrainHash, result, device, newTextures);
                terrainMeshCount += (uint32_t)(result.size() - before);
            }
        }
    }

    auto lights = LightExtractor::ExtractCellLights(cellPtr);

    uint32_t hasDiffuse = 0, hasNormal = 0, hasRoughness = 0;
    for (auto& m : result) {
        if (m.diffuseTextureHash)   hasDiffuse++;
        if (m.normalTextureHash)    hasNormal++;
        if (m.roughnessTextureHash) hasRoughness++;
    }

    _MESSAGE("FO4RemixPlugin: ExtractCell 0x%08X - %u meshes + %u terrain + %zu skinned (%u diffuse, %u normal, %u roughness), "
             "%zu new textures (%zu cached), %zu lights from %u objects",
             cellFormID, meshCount, terrainMeshCount, skinnedResult.size(),
             hasDiffuse, hasNormal, hasRoughness,
             newTextures.size(), g_textureCache.size(), lights.size(), objectList.count);

    return { std::move(result), std::move(skinnedResult), std::move(newTextures), std::move(lights) };
}

// ---------------------------------------------------------------------------
// Extract all geometry from loaded references in the player's current cell
// ---------------------------------------------------------------------------
ExtractionResult SceneExtractor::ExtractPlayerCell(ID3D11Device* device)
{
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) {
        _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - no player");
        return {};
    }
    uintptr_t player = *ppPlayer;
    uintptr_t cellPtr = *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
    if (!cellPtr) {
        _MESSAGE("FO4RemixPlugin: ExtractPlayerCell - no parentCell");
        return {};
    }
    return ExtractCell(cellPtr, device);
}

// ---------------------------------------------------------------------------
// Clear the texture readback cache
// ---------------------------------------------------------------------------
void SceneExtractor::ClearTextureCache()
{
    _MESSAGE("FO4RemixPlugin: ClearTextureCache - clearing %zu entries", g_textureCache.size());
    g_textureCache.clear();
}

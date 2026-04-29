#include "bs_extraction.h"
#include "config.h"

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
static RelocPtr<uintptr_t> s_g_dataHandler(0x030DC000);

// Known offsets (verified by STATIC_ASSERTs in F4SE SDK headers):
//   TESObjectREFR::parentCell  = 0xB8  (GameReferences.h)
//   TESObjectREFR::unkF0       = 0xF0  (LoadedData*)
//   LoadedData::rootNode       = 0x08  (NiNode*)
//   TESObjectCELL::objectList  = 0x70  (tArray<TESObjectREFR*>)

static constexpr uintptr_t OFF_FORM_ID          = 0x14;
static constexpr uintptr_t OFF_REFR_PARENT_CELL = 0xB8;
static constexpr uintptr_t OFF_REFR_POS         = 0xD0;  // NiPoint3 (3 floats)
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



// Packed unsigned byte -> [-1, 1]
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
// GPU readback: copy a single mip level of a texture into CPU memory.
// Helper for ReadbackAllMips. Output ExtractedTexture is single-mip.
// ---------------------------------------------------------------------------
static bool ReadbackOneMip(ID3D11DeviceContext* ctx,
                           ID3D11Device* device,
                           ID3D11Texture2D* tex2D,
                           DXGI_FORMAT format,
                           uint32_t mipLevel,
                           uint32_t mipWidth,
                           uint32_t mipHeight,
                           ExtractedTexture& out)
{
    using Microsoft::WRL::ComPtr;

    uint32_t dataSize = ComputeMip0Size(mipWidth, mipHeight, format);
    if (dataSize == 0) return false;

    // Staging texture matching this mip's dimensions
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width              = mipWidth;
    stagingDesc.Height             = mipHeight;
    stagingDesc.MipLevels          = 1;
    stagingDesc.ArraySize          = 1;
    stagingDesc.Format             = format;
    stagingDesc.SampleDesc.Count   = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage              = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags          = 0;
    stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        _MESSAGE("FO4RemixPlugin: ReadbackOneMip - CreateTexture2D staging failed mip=%u hr=0x%08X",
                 mipLevel, (unsigned)hr);
        return false;
    }

    // Copy the requested mip from the source texture into mip 0 of staging
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex2D, mipLevel, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        _MESSAGE("FO4RemixPlugin: ReadbackOneMip - Map failed mip=%u hr=0x%08X",
                 mipLevel, (unsigned)hr);
        return false;
    }

    uint32_t blockSize = 0;
    bool bc = IsBlockCompressed(format, blockSize);

    uint32_t numRows;       // scanline-rows (or block-rows for BC)
    uint32_t expectedPitch; // tight row pitch

    if (bc) {
        uint32_t bw = (mipWidth  + 3) / 4; if (bw < 1) bw = 1;
        uint32_t bh = (mipHeight + 3) / 4; if (bh < 1) bh = 1;
        numRows       = bh;
        expectedPitch = bw * blockSize;
    } else {
        numRows       = mipHeight;
        expectedPitch = mipWidth * 4; // all uncompressed formats we support are 4 bpp
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

    out.width      = mipWidth;
    out.height     = mipHeight;
    out.dxgiFormat = format;
    out.mipLevels  = 1;
    return true;
}

// ---------------------------------------------------------------------------
// GPU readback: read every mip level of a texture into CPU memory, one
// ExtractedTexture per mip. The source texture's actual MipLevels field
// determines the chain length (game textures typically ship full chains;
// some procedural / runtime textures have only 1).
//
// Why all mips: the path tracer samples normal/diffuse maps at varying LOD
// based on screen-space pixel footprint. Without a mip chain, a distant
// surface fetches the full-res mip at sub-pixel rate, average-out flattens
// the normal, and the BRDF degenerates -- most visibly on water, where a
// flat normal + high IOR + Fresnel collapses to a perfect mirror. With the
// chain present, distant sampling fetches a pre-filtered lower mip and
// detail rolls off smoothly.
//
// Returns false on any failure (partial chain is worse than no chain --
// dxvk-remix would render with malformed mips).
// ---------------------------------------------------------------------------
static bool ReadbackAllMips(ID3D11Device* device, ID3D11Texture2D* tex2D,
                            std::vector<ExtractedTexture>& outMips)
{
    using Microsoft::WRL::ComPtr;

    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    if (!ctx) return false;

    D3D11_TEXTURE2D_DESC desc;
    tex2D->GetDesc(&desc);

    if (ComputeMip0Size(desc.Width, desc.Height, desc.Format) == 0) {
        _MESSAGE("FO4RemixPlugin: ReadbackAllMips - unsupported DXGI format %u, skipping",
                 (unsigned)desc.Format);
        return false;
    }

    uint32_t srcMipCount = desc.MipLevels > 0 ? desc.MipLevels : 1;
    outMips.clear();
    outMips.reserve(srcMipCount);

    for (uint32_t i = 0; i < srcMipCount; i++) {
        uint32_t mipW = desc.Width  >> i; if (mipW == 0) mipW = 1;
        uint32_t mipH = desc.Height >> i; if (mipH == 0) mipH = 1;

        ExtractedTexture mip;
        if (!ReadbackOneMip(ctx.Get(), device, tex2D, desc.Format,
                            i, mipW, mipH, mip)) {
            outMips.clear();
            return false;
        }
        outMips.push_back(std::move(mip));
    }

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

// Unified BC1/BC3/BC5 block decompression with per-pixel transform
enum class BCTransform { None, InvertRGB, NormalReconstructZ };

static bool DecompressBC(ExtractedTexture& tex, BCTransform transform)
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
                // BC5: two alpha-style blocks for R and G channels
                uint8_t rChan[4][4], gChan[4][4];
                DecodeBC3AlphaBlock(src, rChan);
                DecodeBC3AlphaBlock(src + 8, gChan);

                if (transform == BCTransform::InvertRGB) {
                    // R=specular, G=smoothness. Invert G to get roughness.
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            uint8_t roughness = 255 - gChan[y][x];
                            block[y][x][0] = roughness;
                            block[y][x][1] = roughness;
                            block[y][x][2] = roughness;
                            block[y][x][3] = 255;
                        }
                } else {
                    // NormalReconstructZ (or None): reconstruct Z from R,G
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

            // Write decoded pixels to output buffer
            if (transform == BCTransform::InvertRGB && !isBC5) {
                // Invert RGB for BC1/BC3 (BC5 already handled above)
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
            } else {
                // No transform, or BC5 already transformed above
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
            DecompressBC(tex, BCTransform::InvertRGB);
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
static void ConvertNormalToOctahedral(ExtractedTexture& tex)
{
    // Decompress BC formats to RGBA first
    switch (tex.dxgiFormat) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC5_TYPELESS:
            DecompressBC(tex, BCTransform::NormalReconstructZ);
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
uint64_t BsExtraction::ExtractMaterialTexture(NiTexture* tex, const char* slotName,
                                       ID3D11Device* device,
                                       std::vector<ExtractedTexture>& newTextures,
                                       TexturePostProcess postProcess)
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

    // Read every mip from the source D3D texture. Each entry is a single-mip
    // ExtractedTexture so we can reuse the existing per-mip decompression and
    // post-process functions unchanged. They get concatenated into a packed
    // mip chain at the end.
    std::vector<ExtractedTexture> mips;
    bool ok = ReadbackAllMips(device, tex2D, mips);
    tex2D->Release();

    if (!ok || mips.empty()) return 0;

    // Per-mip pipeline: BC2 (DXT3) -> RGBA8, then any further BC decompression
    // handled by the post-process stage's BC5/BC1 decoders. Each step operates
    // on a single mip so the existing single-mip-aware functions need no
    // changes.
    for (auto& mip : mips) {
        DecompressBC2(mip);
    }

    // --- Debug dump: diffuse control (no post-processing) ---
    // Only dump mip 0 -- the lower mips are auto-generated and uninteresting
    // for visual debugging. The dump runs at first-extraction (s_dump counter)
    // so it captures the highest-resolution image we'll ever present.
    if (postProcess == TexturePostProcess::None) {
        static int s_dumpDiffuse = 0;
        if (s_dumpDiffuse < 2) {
            ExtractedTexture tmp = mips[0];
            // Decompress BC1 to RGBA so we can dump it
            bool isBC1tmp = (tmp.dxgiFormat == DXGI_FORMAT_BC1_UNORM ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC1_UNORM_SRGB ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC1_TYPELESS);
            if (isBC1tmp) {
                DecompressBC(tmp, BCTransform::None); // reuses the BC1 decode path to get RGBA
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
    // Dump mip 0 of first 3 normal + first 3 roughness textures for inspection.
    {
        static int s_dumpNormalRaw = 0, s_dumpRoughnessRaw = 0;
        bool dumpRaw = false;
        if (postProcess == TexturePostProcess::Octahedral && s_dumpNormalRaw < 3) dumpRaw = true;
        if (postProcess == TexturePostProcess::InvertRGB  && s_dumpRoughnessRaw < 3) dumpRaw = true;

        if (dumpRaw) {
            // BC5 needs decompression to RGBA before we can dump, so do a temporary decode of mip 0
            ExtractedTexture tmp = mips[0]; // copy mip 0
            bool isBC5tmp = (tmp.dxgiFormat == DXGI_FORMAT_BC5_UNORM ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC5_SNORM ||
                             tmp.dxgiFormat == DXGI_FORMAT_BC5_TYPELESS);
            if (isBC5tmp) {
                DecompressBC(tmp, BCTransform::NormalReconstructZ); // decodes to RGBA with reconstructed Z
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

    // Apply post-processing per-mip. SmoothnessToRoughness and
    // ConvertNormalToOctahedral both run BC decode internally if needed and
    // emit RGBA8, so mips that started in different BC formats end up in a
    // common output format -- which matters because we concatenate them
    // into a single packed buffer below.
    for (auto& mip : mips) {
        if (postProcess == TexturePostProcess::InvertRGB) {
            SmoothnessToRoughness(mip);
        } else if (postProcess == TexturePostProcess::Octahedral) {
            ConvertNormalToOctahedral(mip);
        }
    }

    // --- Debug dump: after post-processing (mip 0 only) ---
    {
        static int s_dumpNormalPost = 0, s_dumpRoughnessPost = 0;
        bool dumpPost = false;
        if (postProcess == TexturePostProcess::Octahedral && s_dumpNormalPost < 3) dumpPost = true;
        if (postProcess == TexturePostProcess::InvertRGB  && s_dumpRoughnessPost < 3) dumpPost = true;

        if (dumpPost && (mips[0].dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
                         mips[0].dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) {
            char path[256];
            if (postProcess == TexturePostProcess::Octahedral)
                snprintf(path, sizeof(path), "c:/temp/fo4_debug_normal_post_%d.tga", s_dumpNormalPost++);
            else
                snprintf(path, sizeof(path), "c:/temp/fo4_debug_roughness_post_%d.tga", s_dumpRoughnessPost++);
            DebugDumpTGA(path, mips[0].pixels.data(), mips[0].width, mips[0].height);
            _MESSAGE("FO4RemixPlugin: DEBUG dumped post-process -> %s (%ux%u)", path, mips[0].width, mips[0].height);
        }
    }

    // --- Sanity: every mip must share the same final dxgiFormat. The pipeline
    // above produces RGBA8 for any BC source; uncompressed sources stay in
    // their original (BGRA8 / RGBA8) format. Mixed formats across mips would
    // mean a mid-chain decompression diverged -- bail rather than ship a
    // malformed packed buffer that dxvk-remix would interpret as garbage.
    DXGI_FORMAT chainFmt = mips[0].dxgiFormat;
    for (auto& mip : mips) {
        if (mip.dxgiFormat != chainFmt) {
            _MESSAGE("FO4RemixPlugin: ExtractMaterialTexture - mip format mismatch "
                     "(mip0=%u midmip=%u) for hash=0x%016llX, dropping",
                     (unsigned)chainFmt, (unsigned)mip.dxgiFormat, hash);
            return 0;
        }
    }

    // Concatenate all mips into a single packed buffer. Remix expects the
    // mip chain tightly packed, mip0 first, no padding.
    ExtractedTexture extracted;
    extracted.hash       = hash;
    extracted.width      = mips[0].width;
    extracted.height     = mips[0].height;
    extracted.dxgiFormat = chainFmt;
    extracted.mipLevels  = (uint32_t)mips.size();

    size_t total = 0;
    for (const auto& mip : mips) total += mip.pixels.size();
    extracted.pixels.reserve(total);
    for (auto& mip : mips) {
        extracted.pixels.insert(extracted.pixels.end(),
                                mip.pixels.begin(), mip.pixels.end());
    }

    if (g_config.logTextures) {
        const char* texName = tex->name.c_str();
        _MESSAGE("FO4RemixPlugin: Extracted %s texture \"%s\" %ux%u mips=%u fmt=%u hash=0x%016llX%s",
                 slotName, texName ? texName : "<null>",
                 extracted.width, extracted.height, extracted.mipLevels,
                 (unsigned)extracted.dxgiFormat, hash,
                 postProcess == TexturePostProcess::InvertRGB ? " (inverted)" :
                 postProcess == TexturePostProcess::Octahedral ? " (octahedral)" : "");
    }

    g_textureCache[hash] = extracted;
    newTextures.push_back(std::move(extracted));

    return hash;
}

// Get the BSLightingShaderMaterialBase from a shape, or nullptr
BSLightingShaderMaterialBase* BsExtraction::GetLightingMaterial(BSTriShape* shape)
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
void BsExtraction::ExtractEmissiveData(BSTriShape* shape, BSLightingShaderMaterialBase* lightingMat,
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

// Parse vertices and indices from a BSTriShape. Returns false if the shape
// should be skipped (effect shader, missing data, NaN positions, bad indices).
// When logRejections is false, NaN/Inf and bad-index rejections are silent.
bool BsExtraction::ParseShapeGeometry(BSTriShape* shape, ParsedGeometry& out, bool logRejections)
{
    if (!shape || shape->numVertices == 0 || shape->numTriangles == 0)
        return false;

    // Skip effect shaders — not real geometry. Per F4SE NiMaterials.h:32,
    // GetFeature() returns 2 for lighting and 1 for effect; water returns
    // a different value. The original "!= 2" check over-rejected water as
    // a side effect of being written when only lighting was hooked.
    {
        NiProperty* prop = shape->shaderProperty;
        if (prop) {
            BSShaderProperty* sp = static_cast<BSShaderProperty*>(prop);
            BSShaderMaterial* mat = sp->shaderMaterial;
            if (!mat || mat->GetFeature() == 1)
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

    // Parse vertices
    out.vertices.resize(shape->numVertices);

    for (uint16_t i = 0; i < shape->numVertices; i++) {
        uint8_t* v = vbData + (uint32_t)i * vertexSize;
        remixapi_HardcodedVertex& out_v = out.vertices[i];
        memset(&out_v, 0, sizeof(out_v));

        // Position
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

    // Validate vertex positions
    const char* shapeName = shape->m_name.c_str();
    for (uint16_t i = 0; i < shape->numVertices; i++) {
        const auto& pos = out.vertices[i].position;
        for (int j = 0; j < 3; j++) {
            if (std::isnan(pos[j]) || std::isinf(pos[j])) {
                if (logRejections)
                    _MESSAGE("FO4RemixPlugin: Rejecting mesh \"%s\" - vertex %u has NaN/Inf position",
                             shapeName ? shapeName : "<null>", i);
                return false;
            }
        }
    }

    // Index conversion (uint16 -> uint32)
    uint32_t indexCount = shape->numTriangles * 3;
    uint16_t* indices16 = reinterpret_cast<uint16_t*>(ibData);
    out.indices.resize(indexCount);
    for (uint32_t i = 0; i < indexCount; i++) {
        uint32_t idx = indices16[i];
        if (idx >= shape->numVertices) {
            if (logRejections)
                _MESSAGE("FO4RemixPlugin: Rejecting mesh \"%s\" - index[%u]=%u >= numVertices=%u",
                         shapeName ? shapeName : "<null>", i, idx, shape->numVertices);
            return false;
        }
        out.indices[i] = idx;
    }

    out.vertexDesc = desc;
    out.vertexSize = vertexSize;
    out.vbData = vbData;
    out.isDynamic = isDynamic;

    return true;
}

// ---------------------------------------------------------------------------
// Extract alpha-test + alpha-blend state from the geometry's NiAlphaProperty
// ---------------------------------------------------------------------------
void BsExtraction::ExtractAlphaState(BSGeometry* geo, ExtractedMesh& mesh) {
    mesh.alphaTestEnabled = false;
    mesh.alphaTestType    = 7;   // Always (no test)
    mesh.alphaTestRef     = 128;
    mesh.alphaBlendEnabled    = false;
    mesh.srcColorBlendFactor  = 1;  // VK_BLEND_FACTOR_ONE
    mesh.dstColorBlendFactor  = 0;  // VK_BLEND_FACTOR_ZERO

    if (!geo) return;
    NiProperty* alphaPropRaw = geo->effectState;
    if (!alphaPropRaw) return;

    NiAlphaProperty* alphaProp = static_cast<NiAlphaProperty*>(alphaPropRaw);

    // Alpha test: bit 9 = enabled, bits 10-12 = function
    bool testEnabled = (alphaProp->alphaFlags >> 9) & 1;
    if (testEnabled) {
        int niTestFunc = (alphaProp->alphaFlags >> 10) & 7;
        // NI:  Always=0, Less=1, Equal=2, LessEq=3, Greater=4, NotEq=5, GreaterEq=6, Never=7
        // VK:  Never=0,  Less=1, Equal=2, LessEq=3, Greater=4, NotEq=5, GreaterEq=6, Always=7
        static const int niToVk[] = { 7, 1, 2, 3, 4, 5, 6, 0 };
        mesh.alphaTestEnabled = true;
        mesh.alphaTestType    = niToVk[niTestFunc];
        mesh.alphaTestRef     = alphaProp->alphaThreshold;
    }

    // Alpha blend: bit 0 = enabled, bits 1-4 = src factor, bits 5-8 = dst factor
    static const uint32_t niBlendToVk[] = {
        1,   //  0 kOne          -> VK_BLEND_FACTOR_ONE
        0,   //  1 kZero         -> VK_BLEND_FACTOR_ZERO
        2,   //  2 kSrcColor     -> VK_BLEND_FACTOR_SRC_COLOR
        3,   //  3 kInvSrcColor  -> VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
        4,   //  4 kDestColor    -> VK_BLEND_FACTOR_DST_COLOR
        5,   //  5 kInvDestColor -> VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
        6,   //  6 kSrcAlpha     -> VK_BLEND_FACTOR_SRC_ALPHA
        7,   //  7 kInvSrcAlpha  -> VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
        8,   //  8 kDestAlpha    -> VK_BLEND_FACTOR_DST_ALPHA
        9,   //  9 kInvDestAlpha -> VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
        14,  // 10 kSrcAlphaSat  -> VK_BLEND_FACTOR_SRC_ALPHA_SATURATE
    };

    const bool blendEnabled = alphaProp->alphaFlags & 1;
    if (blendEnabled) {
        const int niSrc = (alphaProp->alphaFlags >> 1) & 0xF;
        const int niDst = (alphaProp->alphaFlags >> 5) & 0xF;
        const uint32_t vkSrc = (niSrc < 11) ? niBlendToVk[niSrc] : 1;
        const uint32_t vkDst = (niDst < 11) ? niBlendToVk[niDst] : 0;
        mesh.alphaBlendEnabled    = true;
        mesh.srcColorBlendFactor  = vkSrc;
        mesh.dstColorBlendFactor  = vkDst;
    }
}

// ---------------------------------------------------------------------------
// Return the player's current parent cell pointer (0 if unavailable)
// ---------------------------------------------------------------------------
uintptr_t BsExtraction::GetPlayerCellPtr()
{
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) return 0;
    uintptr_t player = *ppPlayer;
    return *reinterpret_cast<uintptr_t*>(player + OFF_REFR_PARENT_CELL);
}

// ---------------------------------------------------------------------------
// Read the player's world position. Outputs unchanged (remain 0) if the
// player singleton is absent (e.g. main menu).
// ---------------------------------------------------------------------------
void BsExtraction::GetPlayerPosition(float& outX, float& outY, float& outZ)
{
    uintptr_t* ppPlayer = reinterpret_cast<uintptr_t*>(s_g_player.GetPtr());
    if (!ppPlayer || !*ppPlayer) return;
    uintptr_t player = *ppPlayer;
    const float* pos = reinterpret_cast<const float*>(player + OFF_REFR_POS);
    outX = pos[0];
    outY = pos[1];
    outZ = pos[2];
}

// ---------------------------------------------------------------------------
// Lightweight readiness check — is the player in a cell with loaded 3D?
// ---------------------------------------------------------------------------
bool BsExtraction::IsPlayerCellReady()
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

std::vector<CellInfo> BsExtraction::GetLoadedCells()
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
// Clear the texture readback cache
// ---------------------------------------------------------------------------
void BsExtraction::ClearTextureCache()
{
    _MESSAGE("FO4RemixPlugin: ClearTextureCache - clearing %zu entries", g_textureCache.size());
    g_textureCache.clear();
}

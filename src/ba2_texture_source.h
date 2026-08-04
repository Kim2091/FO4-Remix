#pragma once

#include "bs_extraction.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Ba2TextureSource {

enum class Status {
    Ready,
    NotFound,
    Unsupported,
    ReadFailed,
};

struct ReadInfo {
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t sourceMipCount = 0;
    uint32_t uploadWidth = 0;
    uint32_t uploadHeight = 0;
    uint32_t uploadMipCount = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::string archiveName;
};

// Resolve archive/entry metadata and the mip range selected by the configured
// dimension cap without opening or inflating the texture chunks. This lets a
// worker probe the converted disk cache before paying the authored-read cost.
Status Query(const char* resourcePath, DXGI_FORMAT liveFormat,
             uint32_t expectedWidth, uint32_t expectedHeight,
             uint32_t maxTextureDimension, ReadInfo& outInfo);

// Read a full authored mip chain from the DX10 BA2 archives beside the
// running Fallout4.exe. Thread-safe after the call-once archive index build;
// intended for the plugin's texture worker pool, never the render thread.
Status Read(const char* resourcePath, DXGI_FORMAT liveFormat,
            uint32_t expectedWidth, uint32_t expectedHeight,
            uint32_t maxTextureDimension,
            std::vector<ExtractedTexture>& outMips, ReadInfo& outInfo);

const char* StatusName(Status status);

} // namespace Ba2TextureSource

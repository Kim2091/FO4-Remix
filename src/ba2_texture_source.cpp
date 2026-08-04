#include "ba2_texture_source.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "f4se/PluginAPI.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

#pragma pack(push, 1)
struct Ba2Header {
    char magic[4];
    uint32_t version;
    char type[4];
    uint32_t fileCount;
    uint64_t nameTableOffset;
};

struct Ba2TextureRecord {
    uint32_t nameHash;
    char extension[4];
    uint32_t directoryHash;
    uint8_t unknown0C;
    uint8_t chunkCount;
    uint16_t chunkHeaderSize;
    uint16_t height;
    uint16_t width;
    uint8_t mipCount;
    uint8_t format;
    uint16_t unknown16;
};

struct Ba2ChunkRecord {
    uint64_t offset;
    uint32_t packedSize;
    uint32_t unpackedSize;
    uint16_t startMip;
    uint16_t endMip;
    uint32_t alignment;
};
#pragma pack(pop)

static_assert(sizeof(Ba2Header) == 24, "BA2 header layout");
static_assert(sizeof(Ba2TextureRecord) == 24, "BA2 texture record layout");
static_assert(sizeof(Ba2ChunkRecord) == 24, "BA2 chunk record layout");

struct ArchiveInfo {
    std::string path;
    std::string name;
};

struct TextureEntry {
    uint32_t archiveIndex = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t mipCount = 0;
    uint8_t format = 0;
    std::vector<Ba2ChunkRecord> chunks;
};

struct PendingRecord {
    Ba2TextureRecord texture = {};
    std::vector<Ba2ChunkRecord> chunks;
};

std::once_flag g_indexOnce;
std::vector<ArchiveInfo> g_archives;
std::unordered_map<std::string, std::vector<TextureEntry>> g_entries;
bool g_indexReady = false;

template <typename T>
bool ReadExact(std::ifstream& stream, T& value)
{
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return stream.good();
}

bool ReadBytes(std::ifstream& stream, void* dst, size_t size)
{
    if (size > static_cast<size_t>((std::numeric_limits<std::streamsize>::max)())) {
        return false;
    }
    stream.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
    return stream.good();
}

std::string LowerAscii(std::string value)
{
    for (char& c : value) {
        if (c == '/') c = '\\';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string NormalizeResourcePath(const char* path)
{
    if (!path || !*path) return {};
    std::string normalized = LowerAscii(path);
    while (normalized.rfind(".\\", 0) == 0) normalized.erase(0, 2);
    while (!normalized.empty() && normalized.front() == '\\') {
        normalized.erase(normalized.begin());
    }
    if (normalized.rfind("data\\", 0) == 0) normalized.erase(0, 5);
    if (normalized.rfind("textures\\", 0) != 0) {
        normalized.insert(0, "textures\\");
    }
    return normalized;
}

uint32_t FormatFamily(DXGI_FORMAT format)
{
    switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB: return 1;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB: return 2;
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB: return 3;
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM: return 4;
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM: return 5;
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB: return 7;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 8;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 9;
        default: return 0;
    }
}

DXGI_FORMAT NormalizeFormat(DXGI_FORMAT authored, DXGI_FORMAT live)
{
    if (FormatFamily(authored) != 0 &&
        FormatFamily(authored) == FormatFamily(live)) {
        return live;
    }
    switch (authored) {
        case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return authored;
    }
}

uint32_t MipSize(uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    const uint32_t bw = (std::max)(1u, (width + 3) / 4);
    const uint32_t bh = (std::max)(1u, (height + 3) / 4);
    switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
            return bw * bh * 8;
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
            return bw * bh * 16;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            if (width > UINT32_MAX / 4 || height > UINT32_MAX / (width * 4)) return 0;
            return width * height * 4;
        default:
            return 0;
    }
}

bool IsBlockCompressed(DXGI_FORMAT format)
{
    const uint32_t family = FormatFamily(format);
    return family >= 1 && family <= 7;
}

uint32_t MipDim(uint32_t base, uint32_t mip)
{
    return (std::max)(1u, base >> mip);
}

std::string GameDataDirectory()
{
    char exePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    char* slash = std::strrchr(exePath, '\\');
    if (!slash) return {};
    *slash = 0;
    return std::string(exePath) + "\\Data";
}

int ArchivePriority(const std::string& name)
{
    const std::string lower = LowerAscii(name);
    if (lower.rfind("fallout4 - textures", 0) == 0) {
        return lower.find("patch") != std::string::npos ? 1 : 0;
    }
    return 2;
}

bool IndexArchive(const std::string& path, const std::string& name)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;

    Ba2Header header = {};
    if (!ReadExact(stream, header) ||
        std::memcmp(header.magic, "BTDX", 4) != 0 ||
        std::memcmp(header.type, "DX10", 4) != 0 ||
        header.fileCount == 0 || header.fileCount > 1000000) {
        return false;
    }

    std::vector<PendingRecord> pending(header.fileCount);
    for (uint32_t i = 0; i < header.fileCount; ++i) {
        if (!ReadExact(stream, pending[i].texture)) return false;
        const uint8_t chunkCount = pending[i].texture.chunkCount;
        if (chunkCount == 0 || chunkCount > 32) return false;
        pending[i].chunks.resize(chunkCount);
        for (Ba2ChunkRecord& chunk : pending[i].chunks) {
            if (!ReadExact(stream, chunk)) return false;
        }
    }

    stream.seekg(static_cast<std::streamoff>(header.nameTableOffset), std::ios::beg);
    if (!stream) return false;

    std::vector<std::pair<std::string, TextureEntry>> localEntries;
    localEntries.reserve(header.fileCount);
    uint32_t indexed = 0;
    for (uint32_t i = 0; i < header.fileCount; ++i) {
        uint16_t nameLength = 0;
        if (!ReadExact(stream, nameLength) || nameLength == 0 || nameLength > 4096) {
            return false;
        }
        std::string resource(nameLength, '\0');
        if (!ReadBytes(stream, resource.data(), resource.size())) return false;
        resource = NormalizeResourcePath(resource.c_str());
        if (resource.empty()) continue;

        const Ba2TextureRecord& record = pending[i].texture;
        if (record.width == 0 || record.height == 0 || record.mipCount == 0) continue;
        TextureEntry entry;
        entry.width = record.width;
        entry.height = record.height;
        entry.mipCount = record.mipCount;
        entry.format = record.format;
        entry.chunks = std::move(pending[i].chunks);
        localEntries.emplace_back(std::move(resource), std::move(entry));
        ++indexed;
    }
    if (indexed == 0) return false;

    const uint32_t archiveIndex = static_cast<uint32_t>(g_archives.size());
    g_archives.push_back({ path, name });
    for (auto& [resource, entry] : localEntries) {
        entry.archiveIndex = archiveIndex;
        g_entries[resource].push_back(std::move(entry));
    }
    return true;
}

void BuildIndex()
{
    const std::string dataDir = GameDataDirectory();
    if (dataDir.empty()) return;

    struct Candidate { std::string path; std::string name; int priority; };
    std::vector<Candidate> archives;
    WIN32_FIND_DATAA fd = {};
    const std::string pattern = dataDir + "\\*.ba2";
    HANDLE find = FindFirstFileA(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Candidate candidate;
            candidate.name = fd.cFileName;
            candidate.path = dataDir + "\\" + candidate.name;
            candidate.priority = ArchivePriority(candidate.name);
            archives.push_back(std::move(candidate));
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    std::sort(archives.begin(), archives.end(), [](const Candidate& a,
                                                    const Candidate& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    uint32_t dx10Archives = 0;
    for (const Candidate& archive : archives) {
        if (IndexArchive(archive.path, archive.name)) {
            ++dx10Archives;
        }
    }
    g_indexReady = dx10Archives != 0 && !g_entries.empty();
    _MESSAGE("FO4RemixPlugin: [BA2Tex] index %s: %u DX10 archives, "
             "%zu resource paths",
             g_indexReady ? "ready" : "UNAVAILABLE",
             dx10Archives, g_entries.size());
}

const TextureEntry* SelectEntry(const std::vector<TextureEntry>& entries,
                                DXGI_FORMAT liveFormat,
                                uint32_t expectedWidth,
                                uint32_t expectedHeight)
{
    const TextureEntry* fallback = nullptr;
    const TextureEntry* dimensionMatch = nullptr;
    for (const TextureEntry& entry : entries) {
        const DXGI_FORMAT authored = static_cast<DXGI_FORMAT>(entry.format);
        if (FormatFamily(authored) == 0 ||
            (FormatFamily(liveFormat) != 0 &&
             FormatFamily(authored) != FormatFamily(liveFormat))) {
            continue;
        }
        fallback = &entry;
        if ((!expectedWidth || entry.width == expectedWidth) &&
            (!expectedHeight || entry.height == expectedHeight)) {
            dimensionMatch = &entry;
        }
    }
    return dimensionMatch ? dimensionMatch : fallback;
}

bool InflateChunk(std::ifstream& stream, const Ba2ChunkRecord& chunk,
                  std::vector<uint8_t>& output)
{
    const uint32_t storedSize = chunk.packedSize ? chunk.packedSize
                                                  : chunk.unpackedSize;
    if (storedSize == 0 || chunk.unpackedSize == 0 ||
        chunk.unpackedSize > (256u << 20)) {
        return false;
    }
    std::vector<uint8_t> stored(storedSize);
    stream.seekg(static_cast<std::streamoff>(chunk.offset), std::ios::beg);
    if (!stream || !ReadBytes(stream, stored.data(), stored.size())) return false;
    if (!chunk.packedSize) {
        output = std::move(stored);
        return output.size() == chunk.unpackedSize;
    }

    output.resize(chunk.unpackedSize);
    uLongf outputSize = static_cast<uLongf>(output.size());
    const int result = uncompress(output.data(), &outputSize,
                                  stored.data(), static_cast<uLong>(stored.size()));
    return result == Z_OK && outputSize == output.size();
}

struct ResolvedTextureEntry {
    const TextureEntry* entry = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t firstMip = 0;
    uint32_t usableEnd = 0;
};

Ba2TextureSource::Status ResolveTextureEntry(
    const char* resourcePath, DXGI_FORMAT liveFormat,
    uint32_t expectedWidth, uint32_t expectedHeight,
    uint32_t maxTextureDimension,
    ResolvedTextureEntry& resolved, Ba2TextureSource::ReadInfo& outInfo)
{
    resolved = {};
    outInfo = {};
    const std::string normalized = NormalizeResourcePath(resourcePath);
    if (normalized.empty()) return Ba2TextureSource::Status::NotFound;

    std::call_once(g_indexOnce, BuildIndex);
    if (!g_indexReady) return Ba2TextureSource::Status::ReadFailed;
    const auto found = g_entries.find(normalized);
    if (found == g_entries.end()) return Ba2TextureSource::Status::NotFound;
    const TextureEntry* entry = SelectEntry(found->second, liveFormat,
                                            expectedWidth, expectedHeight);
    if (!entry || entry->archiveIndex >= g_archives.size()) {
        return Ba2TextureSource::Status::Unsupported;
    }

    const DXGI_FORMAT format = NormalizeFormat(
        static_cast<DXGI_FORMAT>(entry->format), liveFormat);
    if (FormatFamily(format) == 0 || entry->mipCount > 32) {
        return Ba2TextureSource::Status::Unsupported;
    }

    uint32_t firstMip = 0;
    while (maxTextureDimension > 0 && firstMip + 1 < entry->mipCount &&
           (MipDim(entry->width, firstMip) > maxTextureDimension ||
            MipDim(entry->height, firstMip) > maxTextureDimension)) {
        ++firstMip;
    }
    uint32_t usableEnd = entry->mipCount;
    if (IsBlockCompressed(format)) {
        for (uint32_t mip = firstMip; mip < usableEnd; ++mip) {
            if (MipDim(entry->width, mip) < 4 ||
                MipDim(entry->height, mip) < 4) {
                usableEnd = mip;
                break;
            }
        }
    }
    if (firstMip >= usableEnd) return Ba2TextureSource::Status::Unsupported;

    resolved.entry = entry;
    resolved.format = format;
    resolved.firstMip = firstMip;
    resolved.usableEnd = usableEnd;
    outInfo.sourceWidth = entry->width;
    outInfo.sourceHeight = entry->height;
    outInfo.sourceMipCount = entry->mipCount;
    outInfo.uploadWidth = MipDim(entry->width, firstMip);
    outInfo.uploadHeight = MipDim(entry->height, firstMip);
    outInfo.uploadMipCount = usableEnd - firstMip;
    outInfo.format = format;
    outInfo.archiveName = g_archives[entry->archiveIndex].name;
    return Ba2TextureSource::Status::Ready;
}

} // namespace

Ba2TextureSource::Status Ba2TextureSource::Query(
    const char* resourcePath, DXGI_FORMAT liveFormat,
    uint32_t expectedWidth, uint32_t expectedHeight,
    uint32_t maxTextureDimension, ReadInfo& outInfo)
{
    ResolvedTextureEntry resolved;
    return ResolveTextureEntry(resourcePath, liveFormat, expectedWidth,
                               expectedHeight, maxTextureDimension,
                               resolved, outInfo);
}

Ba2TextureSource::Status Ba2TextureSource::Read(
    const char* resourcePath, DXGI_FORMAT liveFormat,
    uint32_t expectedWidth, uint32_t expectedHeight,
    uint32_t maxTextureDimension,
    std::vector<ExtractedTexture>& outMips, ReadInfo& outInfo)
{
    outMips.clear();
    ResolvedTextureEntry resolved;
    const Status resolveStatus = ResolveTextureEntry(
        resourcePath, liveFormat, expectedWidth, expectedHeight,
        maxTextureDimension, resolved, outInfo);
    if (resolveStatus != Status::Ready) return resolveStatus;
    const TextureEntry* entry = resolved.entry;
    const DXGI_FORMAT format = resolved.format;
    const uint32_t firstMip = resolved.firstMip;
    const uint32_t usableEnd = resolved.usableEnd;

    std::vector<ExtractedTexture> decoded(entry->mipCount);
    std::vector<bool> present(entry->mipCount, false);
    const ArchiveInfo& archive = g_archives[entry->archiveIndex];
    std::ifstream stream(archive.path, std::ios::binary);
    if (!stream) return Status::ReadFailed;

    uint64_t totalRaw = 0;
    for (const Ba2ChunkRecord& chunk : entry->chunks) {
        if (chunk.startMip > chunk.endMip || chunk.endMip >= entry->mipCount) {
            return Status::Unsupported;
        }
        std::vector<uint8_t> raw;
        if (!InflateChunk(stream, chunk, raw)) return Status::ReadFailed;
        totalRaw += raw.size();
        if (totalRaw > (256ull << 20)) return Status::Unsupported;

        size_t cursor = 0;
        for (uint32_t mip = chunk.startMip; mip <= chunk.endMip; ++mip) {
            const uint32_t width = MipDim(entry->width, mip);
            const uint32_t height = MipDim(entry->height, mip);
            const uint32_t size = MipSize(width, height, format);
            if (size == 0 || cursor > raw.size() || size > raw.size() - cursor) {
                return Status::ReadFailed;
            }
            if (mip >= firstMip && mip < usableEnd) {
                ExtractedTexture& target = decoded[mip];
                target.width = width;
                target.height = height;
                target.dxgiFormat = format;
                target.mipLevels = 1;
                target.pixels.assign(raw.begin() + cursor,
                                     raw.begin() + cursor + size);
                present[mip] = true;
            }
            cursor += size;
        }
    }

    outMips.reserve(usableEnd - firstMip);
    for (uint32_t mip = firstMip; mip < usableEnd; ++mip) {
        if (!present[mip]) {
            outMips.clear();
            return Status::ReadFailed;
        }
        outMips.push_back(std::move(decoded[mip]));
    }

    return Status::Ready;
}

const char* Ba2TextureSource::StatusName(Status status)
{
    switch (status) {
        case Status::Ready: return "ready";
        case Status::NotFound: return "not-found";
        case Status::Unsupported: return "unsupported";
        case Status::ReadFailed: return "read-failed";
        default: return "unknown";
    }
}

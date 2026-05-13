#pragma once
#include <windows.h>
#include <map>
#include <vector>
#include <string>
#include <cstdint>

namespace rbpo {

enum class AvObjectType : uint8_t {
    PE     = 0,
    Script = 1,
};

struct AvRecord {
    uint64_t             prefix;
    uint32_t             sigLen;
    std::vector<uint8_t> sigHash;
    int64_t              offsetBegin;
    int64_t              offsetEnd;
    AvObjectType         type;
    std::vector<uint8_t> recordSig;
};

struct AvDbInfo {
    std::wstring date;
    uint32_t     count;
};

void         AvLoad();
AvDbInfo     AvGetInfo();
bool         AvScanFile(const std::wstring& path, std::wstring& threatName);
std::wstring AvScanDirectory(const std::wstring& dirPath);

} // namespace rbpo

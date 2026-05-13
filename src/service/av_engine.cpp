#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <cstdint>
#include <cstring>
#include "av_engine.h"

#pragma comment(lib, "bcrypt.lib")

namespace rbpo {

extern "C" void RBPOLog(const char* fmt, ...);

static std::vector<uint8_t> Sha256(const void* data, size_t len)
{
    std::vector<uint8_t> result(32, 0);
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return result;

    DWORD objSz = 0, copied = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                      (PUCHAR)&objSz, sizeof(DWORD), &copied, 0);

    std::vector<uint8_t> obj(objSz);
    if (BCryptCreateHash(hAlg, &hHash, obj.data(), objSz, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }

    BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0);
    BCryptFinishHash(hHash, result.data(), (ULONG)result.size(), 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

static std::map<uint64_t, std::vector<AvRecord>> g_db;
static std::wstring g_dbDate;
static uint32_t     g_dbCount = 0;
static std::mutex   g_dbMtx;

static std::vector<uint8_t> ComputeRecordSig(const AvRecord& r)
{
    std::vector<uint8_t> buf;
    buf.reserve(8 + 4 + r.sigHash.size() + 8 + 8 + 1);

    for (int i = 0; i < 8; i++)
        buf.push_back((uint8_t)((r.prefix >> (i * 8)) & 0xFF));
    for (int i = 0; i < 4; i++)
        buf.push_back((uint8_t)((r.sigLen >> (i * 8)) & 0xFF));
    buf.insert(buf.end(), r.sigHash.begin(), r.sigHash.end());
    for (int i = 0; i < 8; i++)
        buf.push_back((uint8_t)(((uint64_t)r.offsetBegin >> (i * 8)) & 0xFF));
    for (int i = 0; i < 8; i++)
        buf.push_back((uint8_t)(((uint64_t)r.offsetEnd   >> (i * 8)) & 0xFF));
    buf.push_back((uint8_t)r.type);

    return Sha256(buf.data(), buf.size());
}

static AvRecord MakeRecord(const std::vector<uint8_t>& sig,
                            int64_t offsetBegin, int64_t offsetEnd,
                            AvObjectType type)
{
    AvRecord r;
    r.prefix = 0;
    for (int i = 0; i < 8 && i < (int)sig.size(); i++)
        r.prefix |= ((uint64_t)sig[i] << (i * 8));
    r.sigLen      = (uint32_t)sig.size();
    r.sigHash     = Sha256(sig.data(), sig.size());
    r.offsetBegin = offsetBegin;
    r.offsetEnd   = offsetEnd;
    r.type        = type;
    r.recordSig   = ComputeRecordSig(r);
    return r;
}

void AvLoad()
{
    std::lock_guard<std::mutex> lk(g_dbMtx);
    g_db.clear();

    std::vector<uint8_t> sig1 = {
        'R','B','P','O','T','E','S','T',
        'V','R','S','1','.','0','0','0'
    };
    AvRecord rec1 = MakeRecord(sig1, -1, -1, AvObjectType::PE);
    g_db[rec1.prefix].push_back(rec1);

    std::vector<uint8_t> sig2 = {
        '#','R','B','P','O','T','E','S',
        'T','V','R','S','2','.','0','0'
    };
    AvRecord rec2 = MakeRecord(sig2, -1, -1, AvObjectType::Script);
    g_db[rec2.prefix].push_back(rec2);

    g_dbDate  = L"2026-05-13";
    g_dbCount = 0;
    for (auto& [k, v] : g_db)
        g_dbCount += (uint32_t)v.size();

    RBPOLog("AvLoad: loaded %u records, date=%ls", g_dbCount, g_dbDate.c_str());
}

AvDbInfo AvGetInfo()
{
    std::lock_guard<std::mutex> lk(g_dbMtx);
    AvDbInfo info;
    info.date  = g_dbDate;
    info.count = g_dbCount;
    return info;
}

static AvObjectType DetectFileType(const std::wstring& path,
                                    const std::vector<uint8_t>& head)
{
    size_t dot = path.rfind(L'.');
    if (dot != std::wstring::npos) {
        std::wstring ext = path.substr(dot);
        for (auto& c : ext) c = towlower(c);
        if (ext == L".py" || ext == L".ps1" || ext == L".js" || ext == L".vbs")
            return AvObjectType::Script;
    }
    if (head.size() >= 2 && head[0] == 'M' && head[1] == 'Z')
        return AvObjectType::PE;
    return AvObjectType::Script;
}

static bool ScanStream(const std::vector<uint8_t>& data,
                       AvObjectType fileType,
                       std::wstring& threatName)
{
    if (g_db.empty()) return false;
    size_t sz = data.size();

    for (size_t pos = 0; pos + 8 <= sz; ++pos) {
        uint64_t key = 0;
        for (int i = 0; i < 8; i++)
            key |= ((uint64_t)data[pos + i] << (i * 8));

        auto it = g_db.find(key);
        if (it == g_db.end()) continue;

        for (const AvRecord& rec : it->second) {
            if (rec.type != fileType) continue;

            int64_t ipos = (int64_t)pos;
            if (rec.offsetBegin >= 0 && ipos < rec.offsetBegin) continue;
            if (rec.offsetEnd   >= 0 && ipos > rec.offsetEnd)   continue;

            uint32_t extra = rec.sigLen > 8 ? rec.sigLen - 8 : 0;
            if (pos + 8 + extra > sz) continue;

            std::vector<uint8_t> candidate(data.begin() + (ptrdiff_t)pos,
                                           data.begin() + (ptrdiff_t)(pos + 8 + extra));
            auto hash = Sha256(candidate.data(), candidate.size());

            if (hash != rec.sigHash) continue;

            threatName = std::wstring(L"RBPO.Test.") +
                         (rec.type == AvObjectType::PE ? L"PE.Virus" : L"Script.Virus");
            return true;
        }
    }
    return false;
}

bool AvScanFile(const std::wstring& path, std::wstring& threatName)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        RBPOLog("AvScanFile: cannot open '%ls' err=%lu", path.c_str(), GetLastError());
        return false;
    }

    LARGE_INTEGER sz = {};
    GetFileSizeEx(hFile, &sz);

    if (sz.QuadPart > 256LL * 1024 * 1024) {
        RBPOLog("AvScanFile: '%ls' too large (%lld bytes), skipped", path.c_str(), sz.QuadPart);
        CloseHandle(hFile);
        return false;
    }

    std::vector<uint8_t> data((size_t)sz.QuadPart);
    DWORD rd = 0;
    ReadFile(hFile, data.data(), (DWORD)data.size(), &rd, nullptr);
    CloseHandle(hFile);
    data.resize(rd);

    AvObjectType type = DetectFileType(path, data);

    std::lock_guard<std::mutex> lk(g_dbMtx);
    return ScanStream(data, type, threatName);
}

std::wstring AvScanDirectory(const std::wstring& dirPath)
{
    std::wstring results;

    std::wstring search = dirPath;
    if (!search.empty() && search.back() != L'\\')
        search += L'\\';
    search += L'*';

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return results;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring full = dirPath;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            results += AvScanDirectory(full);
        } else {
            std::wstring threat;
            if (AvScanFile(full, threat))
                results += full + L": " + threat + L"\n";
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return results;
}

} // namespace rbpo

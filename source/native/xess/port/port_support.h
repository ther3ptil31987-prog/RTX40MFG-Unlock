#pragma once
// Isolated, exact-provider XeSS capability experiment. No on-disk provider edits.
#include "../xess_mod.h"
#include "../runtime_layout.h"
#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

static std::mutex logMutex;
static std::string escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 32) out += char(c);
    }
    return out;
}
static std::string utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
static void logEvent(const char* stage, const std::string& fields = "") {
    xess_mod::WriteLog(stage, fields);
}
static void checked(HRESULT hr, const char* stage) {
    logEvent(stage, "\"result\":" + std::to_string(int(hr)));
    if (FAILED(hr)) throw std::runtime_error(stage);
}
static void fgChecked(xefg_swapchain_result_t r, const char* stage) {
    logEvent(stage, "\"result\":" + std::to_string(int(r)));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) throw std::runtime_error(stage);
}
static void fgLog(const char* message, xefg_swapchain_logging_level_t level, void*) {
    logEvent("xefg_log", "\"level\":" + std::to_string(int(level)) + ",\"message\":\"" + escape(message) + "\"");
}
static void llLog(const char* message, xell_logging_level_t level) {
    logEvent("xell_log", "\"level\":" + std::to_string(int(level)) + ",\"message\":\"" + escape(message) + "\"");
}
static std::string sha256(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("hash_open");
    BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("hash_algorithm");
    DWORD size{}, bytes{};
    NTSTATUS status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &bytes, 0);
    std::vector<UCHAR> object(size);
    if (status >= 0) status = BCryptCreateHash(alg, &hash, object.data(), size, nullptr, 0, 0);
    std::array<char, 65536> buf{};
    while (status >= 0 && f) {
        f.read(buf.data(), buf.size());
        if (f.gcount()) status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(f.gcount()), 0);
    }
    std::array<UCHAR, 32> digest{};
    if (status >= 0 && !f.bad()) status = BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0);
    else status = -1;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0) throw std::runtime_error("hash_failed");
    std::string out;
    for (UCHAR b : digest) { char h[3]; sprintf_s(h, "%02x", b); out += h; }
    return out;
}
static HMODULE verifyModule(const wchar_t* name, const char* expected) {
    HMODULE module = GetModuleHandleW(name);
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    if (!module || !n || n >= std::size(path)) throw std::runtime_error("module_path");
    std::string actual = sha256(path);
    logEvent("module", "\"name\":\"" + utf8(name) + "\",\"path\":\"" + escape(utf8(path)) +
             "\",\"sha256\":\"" + actual + "\"");
    if (actual != expected) throw std::runtime_error("module_hash_rejected");
    return module;
}

/* Registry helpers for DllRegisterServer. */
#pragma once

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace a3d {

inline bool setKey(const char *key, const char *name, const char *value)
{
    HKEY h{};
    if(RegCreateKeyExA(HKEY_CLASSES_ROOT, key, 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr)
        != ERROR_SUCCESS)
        return false;
    const LONG r{RegSetValueExA(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
        DWORD(std::strlen(value) + 1))};
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

inline HRESULT registerClass(HMODULE mod, const char *clsid, const char *progid, const char *desc)
{
    char path[MAX_PATH]{}, key[256]{};
    GetModuleFileNameA(mod, path, MAX_PATH);
    bool ok{true};
    ok &= setKey(progid, nullptr, desc);
    snprintf(key, sizeof(key), "%s\\CLSID", progid);
    ok &= setKey(key, nullptr, clsid);
    snprintf(key, sizeof(key), "CLSID\\%s", clsid);
    ok &= setKey(key, nullptr, desc);
    snprintf(key, sizeof(key), "CLSID\\%s\\InprocServer32", clsid);
    ok &= setKey(key, nullptr, path);
    ok &= setKey(key, "ThreadingModel", "Apartment");
    return ok ? S_OK : E_ACCESSDENIED;
}

inline HRESULT unregisterClass(const char *clsid, const char *progid)
{
    char key[256]{};
    snprintf(key, sizeof(key), "CLSID\\%s\\InprocServer32", clsid);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, key);
    snprintf(key, sizeof(key), "CLSID\\%s", clsid);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, key);
    snprintf(key, sizeof(key), "%s\\CLSID", progid);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, key);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, progid);
    return S_OK;
}

} // namespace a3d

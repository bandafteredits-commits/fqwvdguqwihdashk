// cp_inject.h - shared injector code for cpcull_inject.exe / cplatency_inject.exe.
// Exit code: 0 = injected (or already loaded), 1 = Minecraft not running, 2 = injection failed.
// The exe (HTMI.proxx.exe) reads that exit code. Bedrock is a UWP/AppContainer process, so the DLL
// is first copied to C:\Users\Public\CheesyProxxy and that folder + file are granted to
// ALL APPLICATION PACKAGES (S-1-15-2-1); without that LoadLibrary fails with access denied.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>
#include <wchar.h>

#define CP_PROC L"Minecraft.Windows.exe"
#define CP_DIR  L"C:\\Users\\Public\\CheesyProxxy"

static bool cp_grant(const wchar_t* path, DWORD mask, DWORD inherit) {
    PSID sid = NULL;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sid)) return false;
    PACL oldAcl = NULL; PSECURITY_DESCRIPTOR sd = NULL; bool ok = false;
    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &oldAcl, NULL, &sd) == ERROR_SUCCESS) {
        EXPLICIT_ACCESS_W ea; ZeroMemory(&ea, sizeof ea);
        ea.grfAccessPermissions = mask; ea.grfAccessMode = GRANT_ACCESS; ea.grfInheritance = inherit;
        BuildTrusteeWithSidW(&ea.Trustee, sid);
        PACL newAcl = NULL;
        if (SetEntriesInAclW(1, &ea, oldAcl, &newAcl) == ERROR_SUCCESS) {
            ok = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, newAcl, NULL) == ERROR_SUCCESS;
            LocalFree(newAcl);
        }
    }
    if (sd) LocalFree(sd);
    LocalFree(sid);
    return ok;
}

static DWORD cp_find_pid(const wchar_t* exe) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof pe; DWORD pid = 0;
    if (Process32FirstW(s, &pe)) do { if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; } } while (Process32NextW(s, &pe));
    CloseHandle(s);
    return pid;
}

static bool cp_module_loaded(DWORD pid, const wchar_t* dll) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (s == INVALID_HANDLE_VALUE) { if (GetLastError() == ERROR_BAD_LENGTH) continue; return false; }
        MODULEENTRY32W me; me.dwSize = sizeof me; bool found = false;
        if (Module32FirstW(s, &me)) do { if (_wcsicmp(me.szModule, dll) == 0) { found = true; break; } } while (Module32NextW(s, &me));
        CloseHandle(s);
        return found;
    }
    return false;
}

static int cp_inject_main(const wchar_t* dllName) {
    DWORD pid = cp_find_pid(CP_PROC);
    if (!pid) return 1;
    if (cp_module_loaded(pid, dllName)) return 0;          // already in: nothing to do, the DLL polls its cfg

    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t* slash = wcsrchr(self, L'\\'); if (slash) *(slash + 1) = 0;
    wchar_t src[MAX_PATH], dst[MAX_PATH];
    wsprintfW(src, L"%s%s", self, dllName);
    wsprintfW(dst, L"%s\\%s", CP_DIR, dllName);

    CreateDirectoryW(CP_DIR, NULL);
    cp_grant(CP_DIR, FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE,
             SUB_CONTAINERS_AND_OBJECTS_INHERIT);
    if (!CopyFileW(src, dst, FALSE) && GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES) return 2;   // DLL missing
    cp_grant(dst, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, NO_INHERITANCE);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return 2;
    int rc = 2;
    SIZE_T bytes = (wcslen(dst) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote && WriteProcessMemory(proc, remote, dst, bytes, NULL)) {
        FARPROC ll = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");   // same address in every process
        HANDLE th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)ll, remote, 0, NULL);
        if (th) {
            DWORD w = WaitForSingleObject(th, 8000), code = 0;
            if (w == WAIT_OBJECT_0 && GetExitCodeThread(th, &code) && code != 0) rc = 0;   // low 32 bits of the HMODULE
            else if (cp_module_loaded(pid, dllName)) rc = 0;
            CloseHandle(th);
        }
    }
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return rc;
}

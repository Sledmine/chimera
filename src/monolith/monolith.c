#define MODS_PATH "mods\\"

#include <w32api.h>
#include <windows.h>
#include <winbase.h>
#include <winver.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct {
    HMODULE module;
} LoadedDLL;

static LoadedDLL *loaded_dlls;
static int dll_count = 0;
static int dll_capacity = 16;

static int string_copy(char *out, const char *in) {
    int i=0;
    while(*in) {
        *(out++) = *(in++);
        i++;
    }
    *out = 0;
    return i;
}

static inline char to_lowercase(char a) {
    return (a >= 'A' && a <= 'Z') ? ('a' + a - 'A') : a;
}

static inline int string_equal(const char *a, const char *b) {
    while(*a && (to_lowercase(*a) == to_lowercase(*b))) {
        a++;
        b++;
    }
    return *a == *b;
}

// Unload all DLLs
static void unload_dlls() {
    if(!loaded_dlls) {
        return;
    }

    for(int i = 0; i < dll_count; i++) {
        FreeLibrary(loaded_dlls[i].module);
    }

    VirtualFree(loaded_dlls, sizeof(*loaded_dlls) * dll_capacity, MEM_DECOMMIT | MEM_RELEASE);
    loaded_dlls = NULL;
}

// Load DLLs; return true if Chimera could be loaded
static BOOL load_dlls() {
    dll_count = 0;
    loaded_dlls = VirtualAlloc(NULL, sizeof(*loaded_dlls) * dll_capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    // Load Chimera
    extern void instantiate_chimera();
    extern void destroy_chimera();
    extern int find_signatures();
    extern void print_signature_errors();
    extern int halo_type();
    instantiate_chimera();
    if(find_signatures() == 0) {
        print_signature_errors();
        destroy_chimera();
        return FALSE;
    }

    // Now try to load everything in mods
    WIN32_FIND_DATA find_file_data;
    HMODULE handle = FindFirstFile(MODS_PATH "*.dll", &find_file_data);
    BOOL ok = handle != INVALID_HANDLE_VALUE;
    while(ok) {
        // Find it!
        char path[MAX_PATH * 2];
        int end = string_copy(path, MODS_PATH);
        end += string_copy(path + end, find_file_data.cFileName);
        ok = FindNextFile(handle, &find_file_data);

        // Wine workaround; As of 4.3-staging, *.dll matches anything with an extension that starts with .dll apparently. Also prevent chimera.dll from being loaded here.
        if(!string_equal(path + end - 4, ".dll") || string_equal(path, MODS_PATH "chimera.dll")) {
            continue;
        }

        // Attempt to load it
        printf("Loading %s...", path);
        fflush(stdout);
        loaded_dlls[dll_count].module = LoadLibrary(path);
        if(loaded_dlls[dll_count].module == NULL) {
            printf("failed\n");
            continue;
        }
        printf("done\n");

        // Reallocate if needed
        if(++dll_count == dll_capacity) {
            int new_dll_capacity = dll_capacity * 2;
            LoadedDLL *new_loaded_dlls = VirtualAlloc(NULL, sizeof(*loaded_dlls) * new_dll_capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            for(int i = 0; i < dll_count; i++) {
                new_loaded_dlls[i].module = loaded_dlls[i].module;
            }
            VirtualFree(loaded_dlls, sizeof(*loaded_dlls) * dll_capacity, MEM_DECOMMIT | MEM_RELEASE);
            loaded_dlls = new_loaded_dlls;
            dll_capacity = new_dll_capacity;
        }
    }

    return TRUE;
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    static WSADATA fun = {};

    (void)reserved;
    (void)instance;

    switch(reason) {
        case DLL_PROCESS_ATTACH:
            {
                // Get the exe path
                static char exe_path[MAX_PATH];
                memset(exe_path, 0, sizeof(exe_path));
                GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

                // Check the exe version
                DWORD handle;
                DWORD ver_info_size = GetFileVersionInfoSize(exe_path, &handle);
                const char *ve = "Version check error";
                if(ver_info_size == 0) {
                    MessageBox(NULL, "Executable does not have valid version information.", ve, MB_OK | MB_ICONERROR);
                    ExitProcess(137);
                }

                LPBYTE ver_info = (LPBYTE)malloc(ver_info_size);
                if(!ver_info) {
                    ExitProcess(137);
                }

                if(!GetFileVersionInfo(exe_path, 0, ver_info_size, ver_info)) {
                    MessageBox(NULL, "Could not get file version information from executable.", ve, MB_OK | MB_ICONERROR);
                    free(ver_info);
                    ExitProcess(137);
                }

                LPVOID ver_buf;
                UINT ver_buf_size;
                if(!VerQueryValue(ver_info, "\\StringFileInfo\\040904B0\\FileVersion", &ver_buf, &ver_buf_size)) {
                    MessageBox(NULL, "Executable file version was not found.", ve, MB_OK | MB_ICONERROR);
                    free(ver_info);
                    ExitProcess(137);
                }

                char *exe_version = ver_buf;
                if(ver_buf_size != 14 || *(exe_version + 13) != '\0') {
                    MessageBox(NULL, "Executable file version is not a valid halo version string.", ve, MB_OK | MB_ICONERROR);
                    free(ver_info);
                    ExitProcess(137);
                }

                // We only support Halo Trial or retail Halo PC and Custom Edition that has been updated to 1.10
                const char *demo_version = "01.00.00.0578";
                const char *full_version = "01.00.10.0621";
                if(strcmp(exe_version, full_version) != 0 && strcmp(exe_version, demo_version) != 0) {
                    char message[256];
                    memset(message, 0, sizeof(message));
                    snprintf(message, sizeof(message), "Current game version is %s.\nOnly the following versions can be used with Chimera:\n\n%s (Halo Trial)\n%s (Retail Halo PC/Halo Custom Edition)", exe_version, demo_version, full_version);
                    MessageBox(NULL, message, ve, MB_OK | MB_ICONERROR);
                    free(ver_info);
                    ExitProcess(137);
                }

                free(ver_info);

                // Get the current directory
                static char cd_path[MAX_PATH];
                memset(cd_path, 0, sizeof(cd_path));
                DWORD cd_len = GetCurrentDirectory(sizeof(cd_path), cd_path);

                // Check if we're CD'd into the Halo directory
                bool in_cd;
                if(strncmp(cd_path, exe_path, cd_len) == 0) { // does the start match?
                    in_cd = true;

                    // Next, we need to skip extraneous separators
                    cd_len++;
                    while(exe_path[cd_len] == '\\' && exe_path[cd_len] == '/') {
                        cd_len++;
                    }

                    // If we have any more path separators before hitting the end, we're not in the same directory
                    for(const char *i = exe_path + cd_len; *i; i++) {
                        if(*i == '\\' || *i == '/') {
                            in_cd = false;
                            break;
                        }
                    }
                }
                else {
                    in_cd = false;
                }

                // Make sure it's the same
                if(!in_cd) {
                    ExitProcess(135);
                }

                // Do this
                WSAStartup(MAKEWORD(2,2), &fun);

                // Enable DEP (if doable)
                #ifndef CHIMERA_WINXP
                SetProcessDEPPolicy(PROCESS_DEP_ENABLE);
                #else
                HANDLE kernel32 = GetModuleHandle("kernel32.dll");
                BOOL WINAPI (*SetProcessDEPPolicy_fn)(DWORD) = GetProcAddress(kernel32, "SetProcessDEPPolicy");
                if(SetProcessDEPPolicy_fn) {
                    SetProcessDEPPolicy_fn(1);
                }
                #endif

                // Terminate if we have to
                if(!load_dlls()) {
                    ExitProcess(133);
                }

                // Check if keystone is loaded. If so, bail.
                if(GetModuleHandle("keystone.dll")) {
                    ExitProcess(197);
                }
            }
            break;
        case DLL_PROCESS_DETACH:
            unload_dlls();

            WSACleanup();

            break;
    }
    return TRUE;
}

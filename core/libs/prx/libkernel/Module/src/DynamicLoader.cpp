#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <nid/NidCompute.hpp>
#include <array>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
extern "C" const void* __aps5_get_pending_module_args_nid_no_patch();
extern "C" void __aps5_set_module_init_result_nid_no_patch(int);
#else
#include <dlfcn.h>
#include <link.h>
#endif

namespace {
thread_local std::array<char, 512> loaderError{};
thread_local bool pendingError = false;
void Error(const char* message) {
    std::snprintf(loaderError.data(), loaderError.size(), "%s", message);
    pendingError = true;
}
struct Module {
    void* native = nullptr;
    bool owned = true;
    bool global = false;
    ~Module() {
        if (owned && native) {
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(native));
#else
            ::dlclose(native);
#endif
        }
    }
};
std::mutex modulesMutex;
std::map<std::uintptr_t, std::shared_ptr<Module>> modules;
std::uintptr_t nextHandle = 0x20000000;
std::map<const void*, std::uintptr_t> imageIds;
#ifdef _WIN32
struct DeferredLifecycle {
    std::uint32_t init;
    std::uint32_t fini;
    std::vector<std::uint32_t> constructors;
    std::vector<std::uint32_t> destructors;
    std::vector<std::string> dependencies;
};
struct DeferredState {
    bool started = false;
    int result = 0;
    std::size_t references = 1;
};
std::recursive_mutex lifecycleMutex;
std::map<void*, DeferredState> lifecycleStates;

bool ReadDeferredLifecycle(void* native, DeferredLifecycle& lifecycle) {
    const auto* base = static_cast<const std::uint8_t*>(native);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        if (std::memcmp(section.Name, ".aps5mod", 8) != 0) continue;
        if (section.VirtualAddress > nt->OptionalHeader.SizeOfImage || section.Misc.VirtualSize > nt->OptionalHeader.SizeOfImage - section.VirtualAddress) throw std::runtime_error("dlopen: invalid deferred module metadata");
        const auto* data = base + section.VirtualAddress;
        std::size_t position = 0;
        const auto word = [&]() {
            if (position > section.Misc.VirtualSize || section.Misc.VirtualSize - position < 4) throw std::runtime_error("dlopen: truncated deferred module metadata");
            std::uint32_t value;
            std::memcpy(&value, data + position, 4);
            position += 4;
            return value;
        };
        if (word() != 1 || word() != 1) throw std::runtime_error("dlopen: deferred module imports have not been bound by the relinked executable");
        lifecycle.init = word();
        lifecycle.fini = word();
        const auto constructors = word();
        const auto destructors = word();
        const auto dependencies = word();
        const auto address = [&](std::uint32_t rva, std::size_t size) {
            if (rva > nt->OptionalHeader.SizeOfImage || size > nt->OptionalHeader.SizeOfImage - rva) throw std::runtime_error("dlopen: invalid deferred lifecycle address");
        };
        address(lifecycle.init, 1);
        address(lifecycle.fini, 1);
        for (std::uint32_t item = 0; item < constructors; ++item) {
            const auto slot = word();
            address(slot, sizeof(void*));
            lifecycle.constructors.push_back(slot);
        }
        for (std::uint32_t item = 0; item < destructors; ++item) {
            const auto slot = word();
            address(slot, sizeof(void*));
            lifecycle.destructors.push_back(slot);
        }
        for (std::uint32_t item = 0; item < dependencies; ++item) {
            if (position >= section.Misc.VirtualSize) throw std::runtime_error("dlopen: truncated deferred dependency name");
            const auto* start = reinterpret_cast<const char*>(data + position);
            const auto* end = static_cast<const char*>(std::memchr(start, 0, section.Misc.VirtualSize - position));
            if (!end || end == start) throw std::runtime_error("dlopen: invalid deferred dependency name");
            lifecycle.dependencies.emplace_back(start, end);
            position += static_cast<std::size_t>(end - start) + 1;
        }
        return true;
    }
    return false;
}

int StopDeferredModule(void* native, std::size_t args, const void* argp);

int StartDeferredModule(void* native, std::size_t args, const void* argp) {
    std::lock_guard lock(lifecycleMutex);
    DeferredLifecycle lifecycle;
    if (!ReadDeferredLifecycle(native, lifecycle)) return 0;
    const auto found = lifecycleStates.find(native);
    if (found != lifecycleStates.end()) {
        if (!found->second.started) throw std::runtime_error("dlopen: recursive deferred module initialization");
        ++found->second.references;
        return found->second.result;
    }
    lifecycleStates.emplace(native, DeferredState{});
    std::vector<void*> dependencies;
    try {
        for (const auto& name : lifecycle.dependencies) {
            const auto path = ResolvePath_nid_no_patch(("/" + name).c_str());
            auto* dependency = GetModuleHandleW(path.c_str());
            if (!dependency) throw std::runtime_error("dlopen: deferred dependency is not mapped: " + name);
            const auto result = StartDeferredModule(dependency, 0, nullptr);
            dependencies.push_back(dependency);
            if (result != 0) throw std::runtime_error("dlopen: deferred dependency initialization failed: " + name);
        }
        auto* base = static_cast<std::uint8_t*>(native);
        int result = 0;
        if (lifecycle.init) result = reinterpret_cast<int (APS5_VABI *)(std::size_t, const void*, void*)>(base + lifecycle.init)(args, argp, nullptr);
        for (const auto slot : lifecycle.constructors) {
            auto callback = *reinterpret_cast<void (APS5_VABI **)(std::size_t, const void*, void*)>(base + slot);
            if (callback) callback(args, argp, nullptr);
        }
        lifecycleStates.at(native) = {true, result, 1};
        return result;
    } catch (...) {
        for (auto dependency = dependencies.rbegin(); dependency != dependencies.rend(); ++dependency) StopDeferredModule(*dependency, 0, nullptr);
        lifecycleStates.erase(native);
        throw;
    }
}

int StopDeferredModule(void* native, std::size_t args, const void* argp) {
    std::lock_guard lock(lifecycleMutex);
    if (!lifecycleStates.contains(native)) return 0;
    auto& state = lifecycleStates.at(native);
    if (!state.started) throw std::runtime_error("dlclose: module initialization has not finished");
    if (state.references > 1) {
        --state.references;
        return 0;
    }
    DeferredLifecycle lifecycle;
    if (!ReadDeferredLifecycle(native, lifecycle)) return 0;
    auto* base = static_cast<std::uint8_t*>(native);
    for (auto slot = lifecycle.destructors.rbegin(); slot != lifecycle.destructors.rend(); ++slot) {
        auto callback = *reinterpret_cast<void (APS5_VABI **)(std::size_t, const void*, void*)>(base + *slot);
        if (callback) callback(args, argp, nullptr);
    }
    const auto result = lifecycle.fini ? reinterpret_cast<int (APS5_VABI *)(std::size_t, const void*, void*)>(base + lifecycle.fini)(args, argp, nullptr) : 0;
    lifecycleStates.erase(native);
    for (auto dependency = lifecycle.dependencies.rbegin(); dependency != lifecycle.dependencies.rend(); ++dependency) {
        const auto path = ResolvePath_nid_no_patch(("/" + *dependency).c_str());
        auto* mapped = GetModuleHandleW(path.c_str());
        if (!mapped) throw std::runtime_error("dlclose: deferred dependency is no longer mapped: " + *dependency);
        StopDeferredModule(mapped, 0, nullptr);
    }
    return result;
}
#endif
void* Symbol(Module& module, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module.native), name));
#else
    return ::dlsym(module.native, name);
#endif
}
#ifndef _WIN32
std::uintptr_t MainProgramBase() {
    static const std::uintptr_t base = [] {
        struct Scan {
            std::uintptr_t value = 0;
        };
        Scan scan;
        ::dl_iterate_phdr(
            [](dl_phdr_info* info, size_t, void* data) {
                auto* found = static_cast<Scan*>(data);
                if (found->value == 0 && info->dlpi_name != nullptr && info->dlpi_name[0] == '\0') found->value = info->dlpi_addr;
                return found->value == 0 ? 0 : 1;
            },
            &scan);
        return scan.value;
    }();
    return base;
}

bool FromGuestModule(void* address) {
    Dl_info info{};
    if (address == nullptr || !::dladdr(address, &info) || info.dli_fname == nullptr) return false;
    if (reinterpret_cast<std::uintptr_t>(info.dli_fbase) == MainProgramBase()) return true;
    const std::string path(info.dli_fname);
    return path.ends_with(".prx");
}

void* DefaultScopeSymbol(const char* symbolName) {
    auto* result = ::dlsym(RTLD_DEFAULT, symbolName);
    return FromGuestModule(result) ? result : nullptr;
}
#endif

void* FindSymbol(Module& module, const char* name) {
    if (auto* symbol = Symbol(module, name)) return symbol;
    const auto nid = Nid::ComputeNid(name, "");
#ifdef _WIN32
    return Symbol(module, nid.c_str());
#else
    if (auto* symbol = Symbol(module, nid.c_str())) return symbol;
    constexpr char guestSuffix[] = "#guest";
    if (auto* symbol = Symbol(module, (std::string(name) + guestSuffix).c_str())) return symbol;
    return Symbol(module, (nid + guestSuffix).c_str());
#endif
}
}

extern "C" {
char* APS5_VABI dlerror_nid_postfix() {
    if (!pendingError) return nullptr;
    pendingError = false;
    return loaderError.data();
}
static std::filesystem::path RelinkedModulePath(const std::filesystem::path& path) {
    auto relinked = path;
    relinked += ".guest.prx";
    std::error_code error;
    return std::filesystem::is_regular_file(relinked, error) ? relinked : path;
}

void* APS5_VABI dlopen_nid_postfix(const char* path, int flags) {
    if ((flags & ~0x103) || (flags & 3) == 0 || (flags & 3) == 3) {
        Error("dlopen: unsupported flags"); return nullptr;
    }
    try {
        auto module = std::make_shared<Module>();
        module->global = (flags & 0x100) != 0 || !path;
#ifdef _WIN32
        if (!path) {
            module->native = GetModuleHandleW(nullptr);
            module->owned = false;
        } else {
            if (!*path) { Error("dlopen: empty module path"); return nullptr; }
            const auto resolved = RelinkedModulePath(ResolvePath_nid_no_patch(path));
            FILE* file = _wfopen(resolved.c_str(), L"rb");
            if (file) {
                std::uint16_t magic = 0;
                const auto readCount = std::fread(&magic, sizeof(magic), 1, file);
                std::fclose(file);
                if (readCount == 1 && magic != 0x5a4d) {
                    Error("dlopen: module is not a Windows PE image");
                    return nullptr;
                }
            }
            const auto oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
            module->native = LoadLibraryExW(resolved.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            SetErrorMode(oldMode);
        }
        if (!module->native) {
            char message[128];
            std::snprintf(message, sizeof(message), "dlopen: Windows loader error %lu (module must be host-compatible)", GetLastError());
            Error(message); return nullptr;
        }
        if (module->owned) {
            struct Pending { std::size_t args; const void* argp; };
            const auto& pending = *static_cast<const Pending*>(__aps5_get_pending_module_args_nid_no_patch());
            __aps5_set_module_init_result_nid_no_patch(StartDeferredModule(module->native, pending.args, pending.argp));
        }
#else
        const auto resolved = path ? RelinkedModulePath(ResolvePath_nid_no_patch(path)).string() : std::string{};
        const int nativeFlags = ((flags & 3) == 1 ? RTLD_LAZY : RTLD_NOW) |
            ((flags & 0x100) ? RTLD_GLOBAL : RTLD_LOCAL);
        module->native = ::dlopen(path ? resolved.c_str() : nullptr, nativeFlags);
        if (!module->native) { Error(::dlerror()); return nullptr; }
#endif
        std::lock_guard lock(modulesMutex);
        const auto handle = nextHandle++;
        modules.emplace(handle, std::move(module));
        return reinterpret_cast<void*>(handle);
    } catch (const std::exception& error) { Error(error.what()); return nullptr; }
}
void* APS5_VABI dlsym_nid_postfix(void* handle, const char* name) {
    if (!name || !*name) { Error("dlsym: empty symbol name"); return nullptr; }
    try {
        std::vector<std::shared_ptr<Module>> search;
        {
            std::lock_guard lock(modulesMutex);
            if (handle == nullptr || handle == reinterpret_cast<void*>(static_cast<std::intptr_t>(-2))) {
                for (const auto& [key, module] : modules) if (module->global) search.push_back(module);
            } else {
                auto found = modules.find(reinterpret_cast<std::uintptr_t>(handle));
                if (found == modules.end()) { Error("dlsym: invalid or unsupported module handle"); return nullptr; }
                search.push_back(found->second);
            }
        }
        for (const auto& module : search) if (auto* result = FindSymbol(*module, name)) return result;
#ifndef _WIN32
        if (handle == nullptr || handle == reinterpret_cast<void*>(static_cast<std::intptr_t>(-2))) {
            if (auto* result = DefaultScopeSymbol(name)) return result;
            const auto nid = Nid::ComputeNid(name, "");
            if (auto* result = DefaultScopeSymbol(nid.c_str())) return result;
        }
#endif
        Error("dlsym: symbol not found in supported module scope");
        return nullptr;
    } catch (const std::exception& error) { Error(error.what()); return nullptr; }
}
int APS5_VABI dlclose_nid_postfix(void* handle) {
    const auto key = reinterpret_cast<std::uintptr_t>(handle);
    std::shared_ptr<Module> module;
#ifdef _WIN32
    bool lastReference = true;
#endif
    {
        std::lock_guard lock(modulesMutex);
        auto found = modules.find(key);
        if (found == modules.end()) { Error("dlclose: invalid module handle"); return -1; }
        module = found->second;
#ifdef _WIN32
        for (const auto& [other, entry] : modules) {
            if (other != key && entry->native == module->native) lastReference = false;
        }
#endif
    }
#ifdef _WIN32
    if (module->owned && module->native) {
        struct Pending { std::size_t args; const void* argp; };
        const auto& pending = *static_cast<const Pending*>(__aps5_get_pending_module_args_nid_no_patch());
        __aps5_set_module_init_result_nid_no_patch(StopDeferredModule(module->native, pending.args, pending.argp));
    }
    if (module->owned && module->native && lastReference) {
        GuestAllocations::Mutation mutation;
        mutation.UnregisterImage(module->native);
    }
#endif
    {
        std::lock_guard lock(modulesMutex);
        auto found = modules.find(key);
        if (found == modules.end()) { Error("dlclose: invalid module handle"); return -1; }
        modules.erase(found);
    }
    // Unload outside the registry lock: module destructors may call loader APIs.
    module.reset();
    return 0;
}
std::int32_t ModuleIdForImage_nid_no_patch(const void* native) {
    std::lock_guard lock(modulesMutex);
    for (const auto& [handle, module] : modules) {
        if (module->native == native) return static_cast<std::int32_t>(handle);
    }
    const auto [found, inserted] = imageIds.emplace(native, nextHandle);
    if (inserted) ++nextHandle;
    return static_cast<std::int32_t>(found->second);
}
}

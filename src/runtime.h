#ifndef RUNTIME_H
#define RUNTIME_H

#include "platform.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

enum class ProfileLevel : uint8_t { None = 0, Full };

class Runtime {
public:
    Runtime();

    ~Runtime() {
        for (auto p: platforms_) {
            delete p;
        }
    }

    /// Registers the given platform into the runtime.
    template <typename T, typename... Args>
    void register_platform(const std::string& name, Args... args) {
        PlatformFactory<T> factory;
        Platform* p = factory.create(this, name, args...);
        platforms_.push_back(p);
    }

    /// Displays available platforms.
    void display_info() {
        info("Available platforms:");
        for (auto p: platforms_) {
            info("    * %: % device(s)", p->name(), p->dev_count());
        }
    }

    /// Allocates memory on the given device.
    void* alloc(PlatformId plat, DeviceId dev, int64_t size) {
        check_device(plat, dev);
        return platforms_[plat]->alloc(dev, size);
    }

    /// Allocates page-locked memory on the given platform and device.
    void* alloc_host(PlatformId plat, DeviceId dev, int64_t size) {
        check_device(plat, dev);
        return platforms_[plat]->alloc_host(dev, size);
    }

    /// Allocates unified memory on the given platform and device.
    void* alloc_unified(PlatformId plat, DeviceId dev, int64_t size) {
        check_device(plat, dev);
        return platforms_[plat]->alloc_unified(dev, size);
    }

    /// Returns the device memory associated with the page-locked memory.
    void* get_device_ptr(PlatformId plat, DeviceId dev, void* ptr) {
        check_device(plat, dev);
        return platforms_[plat]->get_device_ptr(dev, ptr);
    }

    /// Releases memory.
    void release(PlatformId plat, DeviceId dev, void* ptr) {
        check_device(plat, dev);
        platforms_[plat]->release(dev, ptr);
    }

    /// Releases previously allocated page-locked memory.
    void release_host(PlatformId plat, DeviceId dev, void* ptr) {
        check_device(plat, dev);
        platforms_[plat]->release_host(dev, ptr);
    }

    /// Associate a program string to a given filename.
    void register_file(const std::string& filename, const std::string& program_string) {
        files_[filename] = program_string;
    }

    std::string load_file(const std::string& filename) const;
    void store_file(const std::string& filename, const std::string& str) const;

    std::string load_cache(const std::string& str, const std::string& ext=".bin") const;
    void store_cache(const std::string& key, const std::string& str, const std::string ext=".bin") const;

    /// Launches a kernel on the platform and device.
    void launch_kernel(PlatformId plat, DeviceId dev,
                       const char* file, const char* kernel,
                       const uint32_t* grid, const uint32_t* block,
                       void** args, const uint32_t* sizes, const uint32_t* aligns, const uint32_t* allocs, const KernelArgType* types,
                       uint32_t num_args) {
        check_device(plat, dev);
        platforms_[plat]->launch_kernel(dev,
                                        file, kernel,
                                        grid, block,
                                        args, sizes, aligns, allocs, types,
                                        num_args);
    }

    /// Waits for the completion of all kernels on the given platform and device.
    void synchronize(PlatformId plat, DeviceId dev) {
        check_device(plat, dev);
        platforms_[plat]->synchronize(dev);
    }

    /// Copies memory.
    void copy(PlatformId plat_src, DeviceId dev_src, const void* src, int64_t offset_src,
              PlatformId plat_dst, DeviceId dev_dst, void* dst, int64_t offset_dst, int64_t size) {
        check_device(plat_src, dev_src);
        check_device(plat_dst, dev_dst);
        if (plat_src == plat_dst) {
            // Copy from same platform
            platforms_[plat_src]->copy(dev_src, src, offset_src, dev_dst, dst, offset_dst, size);
            debug("Copy between devices % and % on platform %", dev_src, dev_dst, plat_src);
        } else {
            // Copy from another platform
            if (plat_src == 0) {
                // Source is the CPU platform
                platforms_[plat_dst]->copy_from_host(src, offset_src, dev_dst, dst, offset_dst, size);
                debug("Copy from host to device % on platform %", dev_dst, plat_dst);
            } else if (plat_dst == 0) {
                // Destination is the CPU platform
                platforms_[plat_src]->copy_to_host(dev_src, src, offset_src, dst, offset_dst, size);
                debug("Copy to host from device % on platform %", dev_src, plat_src);
            } else {
                error("Cannot copy memory between different platforms");
            }
        }
    }

    bool profiling_enabled() { return profile_ == ProfileLevel::Full; }

private:
    void check_device(PlatformId plat, DeviceId dev) {
        assert((size_t)dev < platforms_[plat]->dev_count() && "Invalid device");
        unused(plat, dev);
    }

    ProfileLevel profile_;
    std::vector<Platform*> platforms_;
    std::unordered_map<std::string, std::string> files_;
};

Runtime& runtime();

#endif

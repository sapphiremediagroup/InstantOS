#pragma once

#include <stdint.h>

// Something a process maps whose pages it does not own: the mapping holds a
// reference for as long as it exists (split, forked or exec'd away included),
// so the pages cannot be freed under it. The pages are mapped with
// kSharedFrame, so address-space teardown and munmap leave them alone; the
// object frees them when its last reference goes. A GPU buffer object
// (intel_gen9_drm.cpp) is one.
struct MappedObject {
    virtual void retain() = 0;
    virtual void release() = 0;
    // The frame to map at `offset` into it when a page that is not there is
    // touched, or 0: then it is a real fault (a GPU buffer object is mapped
    // whole from the start; SharedPages makes its pages this way).
    virtual uint64_t faultPage(uint64_t offset) {
        (void)offset;
        return 0;
    }

protected:
    ~MappedObject() = default;
};

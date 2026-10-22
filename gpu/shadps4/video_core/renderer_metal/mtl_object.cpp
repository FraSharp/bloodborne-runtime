// SPDX-License-Identifier: GPL-2.0-or-later
// From Wooloomooloo2/bloodborne_mac_port (0b50589, GPL-2.0-or-later), adapted to bloodborne-runtime.
// bbport: Metal::Object reference counting through CoreFoundation (plain C++).

#include <CoreFoundation/CoreFoundation.h>
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {

Object Object::Retain(void* object) {
    if (object) {
        CFRetain(object);
    }
    return Adopt(object);
}

void Object::Reset() {
    if (ptr) {
        CFRelease(ptr);
        ptr = nullptr;
    }
}

} // namespace Metal

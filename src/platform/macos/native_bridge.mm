// SPDX-License-Identifier: Apache-2.0
// Objective-C++ bridge for Foundation APIs that have no C equivalent.
#import <Foundation/Foundation.h>

#include "macos_os.hpp"

namespace osie::macos {

MacPowerState read_power_state() {
    MacPowerState s;
    @autoreleasepool {
        NSProcessInfo* info = [NSProcessInfo processInfo];
        switch ([info thermalState]) {
            case NSProcessInfoThermalStateNominal: s.thermal = ThermalState::nominal; break;
            case NSProcessInfoThermalStateFair: s.thermal = ThermalState::fair; break;
            case NSProcessInfoThermalStateSerious: s.thermal = ThermalState::serious; break;
            case NSProcessInfoThermalStateCritical: s.thermal = ThermalState::critical; break;
        }
        if (@available(macOS 12.0, *)) {
            s.low_power_mode = [info isLowPowerModeEnabled];
        }
    }
    return s;
}

}  // namespace osie::macos

namespace osie {
std::unique_ptr<IOperatingSystem> make_native_os() {
    return std::make_unique<macos::MacOperatingSystem>();
}
}  // namespace osie

#pragma once

// COM and WinRT class activation (docs/side-dlls.md, "Default import policy"). Forwarding these
// would let guest code create any class registered on the host, networking ones included (hard
// rule 5), so they are HLE: a class is created only when an allowlist names it (empty for now)
// or, for WinRT, when the guest registered its factory itself. Everything else in COM and WinRT
// stays in-process and is forwarded by the policy.

namespace g6::loader {
class ImportRegistry;
}

namespace g6::imports::combase {

// Binds CoCreateInstance, CoCreateInstanceEx, CoCreateInstanceFromApp, CoGetClassObject,
// RoGetActivationFactory, RoActivateInstance, RoRegisterActivationFactories, and
// RoRevokeActivationFactories for the COM and WinRT API sets, combase.dll, and ole32.dll.
void register_activation(loader::ImportRegistry& imports);

} // namespace g6::imports::combase

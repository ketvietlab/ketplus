#pragma once
#include "cloud/CredentialBackend.h"
#include <Security/Security.h>
namespace ketplus::daemon {
// A non-null keychain restricts all access to an explicitly supplied isolated fixture.
std::shared_ptr<CredentialBackend> keychainCredentialBackend(SecKeychainRef keychain = nullptr);
} // namespace ketplus::daemon

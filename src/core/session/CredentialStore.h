// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace termx {

// Passwords are encrypted with Windows DPAPI (bound to the current Windows
// user) and stored base64-encoded. Nothing is ever written in clear text;
// on platforms without DPAPI, saving passwords is disabled.
namespace CredentialStore {
bool isAvailable();
QString encrypt(const QString &secret);        // empty on failure
QString decrypt(const QString &blobBase64);    // empty on failure
}

} // namespace termx

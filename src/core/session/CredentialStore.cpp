// SPDX-License-Identifier: GPL-3.0-or-later
#include "CredentialStore.h"

#include <QByteArray>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#endif

namespace termx::CredentialStore {

#ifdef _WIN32
namespace {
const char kEntropy[] = "TermX credential v1";
}

bool isAvailable() { return true; }

QString encrypt(const QString &secret)
{
    if (secret.isEmpty())
        return QString();
    QByteArray plain = secret.toUtf8();
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE *>(plain.data())};
    DATA_BLOB entropy{DWORD(sizeof(kEntropy) - 1), reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy))};
    DATA_BLOB out{};
    QString result;
    if (CryptProtectData(&in, L"TermX", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        result = QString::fromLatin1(QByteArray(reinterpret_cast<const char *>(out.pbData), int(out.cbData)).toBase64());
        LocalFree(out.pbData);
    }
    SecureZeroMemory(plain.data(), size_t(plain.size()));
    return result;
}

QString decrypt(const QString &blobBase64)
{
    if (blobBase64.isEmpty())
        return QString();
    QByteArray blob = QByteArray::fromBase64(blobBase64.toLatin1());
    DATA_BLOB in{DWORD(blob.size()), reinterpret_cast<BYTE *>(blob.data())};
    DATA_BLOB entropy{DWORD(sizeof(kEntropy) - 1), reinterpret_cast<BYTE *>(const_cast<char *>(kEntropy))};
    DATA_BLOB out{};
    QString result;
    if (CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        result = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    return result;
}
#else
bool isAvailable() { return false; }
QString encrypt(const QString &) { return QString(); }
QString decrypt(const QString &) { return QString(); }
#endif

} // namespace termx::CredentialStore

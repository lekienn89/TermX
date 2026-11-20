// SPDX-License-Identifier: GPL-3.0-or-later
#include "SshTransport.h"
#include "SftpEngine.h"
#include "SocketUtil.h"
#include "SshContext.h"
#include "SshForwarding.h"
#include "util/SecureWipe.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <libssh2.h>

#include <cstdlib>
#include <cstring>
#include <set>

namespace termx {

// Requests from the GUI thread to the SSH I/O thread.
struct SshControl {
    std::mutex mx;
    std::deque<SftpRequest> sftp;
    std::set<quint64> cancelled;
    std::vector<std::pair<quint64, PortForward>> adds;
    std::vector<quint64> removes;
    bool finished = false;              // the worker is gone: fail new requests at once
    std::atomic<quint64> nextId{1};
};

namespace {

std::mutex g_knownHostsMx;   // serialises read-modify-write of known_hosts

const char *kKexModern =
    "curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-sha2-nistp384,"
    "ecdh-sha2-nistp521,diffie-hellman-group-exchange-sha256,diffie-hellman-group16-sha512,"
    "diffie-hellman-group18-sha512,diffie-hellman-group14-sha256";
const char *kKexLegacy = ",diffie-hellman-group14-sha1,diffie-hellman-group-exchange-sha1,diffie-hellman-group1-sha1";
const char *kHostKeyModern =
    "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256";
const char *kHostKeyLegacy = ",ssh-rsa,ssh-dss";
// libssh2 drops names it was built without, so listing chacha20 is safe.
const char *kCipherModern =
    "chacha20-poly1305@openssh.com,aes128-gcm@openssh.com,aes256-gcm@openssh.com,aes128-ctr,aes192-ctr,aes256-ctr";
const char *kCipherLegacy = ",aes256-cbc,aes192-cbc,aes128-cbc,3des-cbc";
const char *kMacModern =
    "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-512";
const char *kMacLegacy = ",hmac-sha1-etm@openssh.com,hmac-sha1";

QString sshError(LIBSSH2_SESSION *s)
{
    char *msg = nullptr;
    int len = 0;
    libssh2_session_last_error(s, &msg, &len, 0);
    return msg ? QString::fromUtf8(msg, len) : QStringLiteral("unknown SSH error");
}

int knownHostKeyBit(int hostKeyType)
{
    switch (hostKeyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
    default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

QString hostKeyTypeName(int t)
{
    switch (t) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return QStringLiteral("RSA");
    case LIBSSH2_HOSTKEY_TYPE_DSS: return QStringLiteral("DSA");
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return QStringLiteral("ECDSA-256");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return QStringLiteral("ECDSA-384");
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return QStringLiteral("ECDSA-521");
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return QStringLiteral("ED25519");
#endif
    default: return QStringLiteral("unknown");
    }
}

class SshWorker : public TransportWorker {
    Q_OBJECT
public:
    SshWorker(SshOptions o, QString password, QString knownHosts, std::shared_ptr<SshControl> control)
        : m_opts(std::move(o)), m_password(std::move(password)), m_knownHosts(std::move(knownHosts)),
          m_control(std::move(control))
    {
    }

Q_SIGNALS:
    void sftpProgress(const termx::SftpProgress &progress);
    void sftpFinished(const termx::SftpResult &result);
    void forwardsChanged(const QVector<termx::ForwardStatus> &all);

public:

    ~SshWorker() override
    {
        secureWipe(m_password);
        if (m_shared) {
            std::lock_guard<std::mutex> lock(m_shared->mx);
            m_shared->waker = nullptr;
        }
        cleanup();
    }

    void start() override
    {
        run();
        closeControl();
        cleanup();
        finish();
    }

protected:
    void installWaker() override
    {
        std::lock_guard<std::mutex> lock(m_shared->mx);
        net::Waker *w = &m_waker;
        m_shared->waker = [w] { w->wake(); };
    }

private:
    // ---- helpers -----------------------------------------------------------
    bool fail(const QString &msg)
    {
        m_exitReason = msg;
        setState(TransportState::Failed, msg);
        return false;
    }

    // Wait for the socket in the directions libssh2 is blocked on.
    // Returns false when stop was requested or the deadline expired.
    bool waitForSession(const QDeadlineTimer &deadline)
    {
        if (stopRequested() || deadline.hasExpired())
            return false;
        const int dir = libssh2_session_block_directions(m_session);
        const int left = int(qMin<qint64>(deadline.remainingTime() < 0 ? 1000 : deadline.remainingTime(), 1000));
        const int r = net::waitSocket(m_sock, (dir & LIBSSH2_SESSION_BLOCK_INBOUND) != 0 || dir == 0,
                                      (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0, m_waker, left);
        if (r & net::WaitWoken)
            m_waker.drain();
        return !stopRequested();
    }

    template <typename Fn>
    int retry(Fn fn, const QDeadlineTimer &deadline)
    {
        for (;;) {
            const int rc = fn();
            if (rc != LIBSSH2_ERROR_EAGAIN)
                return rc;
            if (!waitForSession(deadline))
                return stopRequested() ? LIBSSH2_ERROR_SOCKET_DISCONNECT : LIBSSH2_ERROR_TIMEOUT;
        }
    }

    void cleanup()
    {
        if (m_session) {
            // Non-blocking disconnect: a dead peer must not hang us.
            libssh2_session_set_blocking(m_session, 0);
            libssh2_session_disconnect(m_session, "Normal shutdown");
            // Freeing channels waits for the server's CLOSE, which in
            // non-blocking mode means EAGAIN and a leaked session. With the
            // socket shut down every send/receive fails at once instead, so
            // libssh2 releases everything without network round trips.
            net::shutdownBoth(m_sock);
            if (m_channel)
                libssh2_channel_free(m_channel);
            m_channel = nullptr;
            for (int i = 0; i < 8 && libssh2_session_free(m_session) == LIBSSH2_ERROR_EAGAIN; ++i) {
            }
            m_session = nullptr;
        } else if (m_channel) {
            m_channel = nullptr;
        }
        if (m_sock != net::kInvalidSocket) {
            net::closeSocket(m_sock);
            m_sock = net::kInvalidSocket;
        }
    }

    // ---- connection --------------------------------------------------------
    void run()
    {
        setState(TransportState::Connecting, QStringLiteral("Connecting to %1:%2...").arg(m_opts.host).arg(m_opts.port));
        if (m_opts.host.trimmed().isEmpty()) {
            fail(QStringLiteral("No host name given"));
            return;
        }
        for (;;) {
            const int rc = connectAndHandshake();
            if (rc == 0)
                break;
            if (rc == kConnectFailed)
                return;
            if (rc == LIBSSH2_ERROR_KEX_FAILURE && !m_opts.allowLegacyAlgorithms && !stopRequested()) {
                // Old switches and routers: offer to retry with weak algorithms.
                const QString why = sshError(m_session);
                cleanup();
                PromptRequest req;
                req.kind = PromptRequest::Kind::LegacyAlgorithms;
                req.title = QStringLiteral("Legacy SSH algorithms");
                req.message = QStringLiteral(
                                  "%1 only offers old SSH algorithms (such as SHA-1 key exchange, ssh-rsa host keys "
                                  "or CBC ciphers). They are weak against a well-placed attacker but common on older "
                                  "network devices.\n\nConnect using legacy algorithms?\n\nDetails: %2")
                                  .arg(m_opts.host, why);
                const auto reply = ask(req);
                if (!reply || !reply->accepted) {
                    fail(QStringLiteral("SSH handshake failed: the server needs legacy algorithms, which are turned off"));
                    return;
                }
                m_opts.allowLegacyAlgorithms = true;
                if (reply->remember)
                    Q_EMIT legacyAlgorithmsAccepted();
                setState(TransportState::Connecting, QStringLiteral("Reconnecting with legacy algorithms..."));
                continue;
            }
            fail(QStringLiteral("SSH handshake failed: %1").arg(sshError(m_session)));
            return;
        }
        reportLegacyMethods();

        if (!verifyHostKey())
            return;
        const bool authed = authenticate();
        secureWipe(m_password);   // not needed after login; a reconnect starts a new worker
        if (!authed)
            return;
        if (!openShell())
            return;

        if (m_opts.keepAliveSeconds > 0)
            libssh2_keepalive_config(m_session, 0, unsigned(m_opts.keepAliveSeconds));

        setState(TransportState::Connected, QString());
        pump();
    }

    static constexpr int kConnectFailed = -10000;

    // 0 on success, a libssh2 error from the handshake, or kConnectFailed
    // (already reported) when the TCP connection or session setup failed.
    int connectAndHandshake()
    {
        QString err;
        m_sock = net::tcpConnect(m_opts.host, uint16_t(m_opts.port), m_opts.connectTimeoutSeconds * 1000,
                                 m_shared->stop, err);
        if (m_sock == net::kInvalidSocket) {
            fail(err);
            return kConnectFailed;
        }
        m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
        if (!m_session) {
            fail(QStringLiteral("Cannot create SSH session"));
            return kConnectFailed;
        }
        libssh2_session_set_blocking(m_session, 0);
        setMethodPreferences();
        QDeadlineTimer deadline(qint64(m_opts.connectTimeoutSeconds) * 1000);
        return retry([&] { return libssh2_session_handshake(m_session, m_sock); }, deadline);
    }

    void setMethodPreferences()
    {
        const bool legacy = m_opts.allowLegacyAlgorithms;
        auto pref = [&](int method, const char *modern, const char *old) {
            QByteArray list(modern);
            if (legacy)
                list += old;
            if (libssh2_session_method_pref(m_session, method, list.constData()) != 0)
                Q_EMIT notice(QStringLiteral("Note: could not set SSH algorithm preference (%1)").arg(sshError(m_session)));
        };
        pref(LIBSSH2_METHOD_KEX, kKexModern, kKexLegacy);
        pref(LIBSSH2_METHOD_HOSTKEY, kHostKeyModern, kHostKeyLegacy);
        pref(LIBSSH2_METHOD_CRYPT_CS, kCipherModern, kCipherLegacy);
        pref(LIBSSH2_METHOD_CRYPT_SC, kCipherModern, kCipherLegacy);
        pref(LIBSSH2_METHOD_MAC_CS, kMacModern, kMacLegacy);
        pref(LIBSSH2_METHOD_MAC_SC, kMacModern, kMacLegacy);
    }

    void reportLegacyMethods()
    {
        const char *kex = libssh2_session_methods(m_session, LIBSSH2_METHOD_KEX);
        const char *crypt = libssh2_session_methods(m_session, LIBSSH2_METHOD_CRYPT_CS);
        const QString k = QString::fromLatin1(kex ? kex : "");
        const QString c = QString::fromLatin1(crypt ? crypt : "");
        if (k.endsWith(QLatin1String("-sha1")) || c.contains(QLatin1String("cbc")))
            Q_EMIT notice(QStringLiteral("Warning: this server only supports legacy SSH algorithms (%1, %2).").arg(k, c));
    }

    // ---- host key ----------------------------------------------------------
    bool verifyHostKey()
    {
        size_t keyLen = 0;
        int keyType = 0;
        const char *key = libssh2_session_hostkey(m_session, &keyLen, &keyType);
        if (!key || keyLen == 0)
            return fail(QStringLiteral("Server did not send a host key"));

        QString fingerprint;
        if (const char *h = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA256)) {
            QByteArray b64 = QByteArray(h, 32).toBase64(QByteArray::OmitTrailingEquals);
            fingerprint = QStringLiteral("SHA256:") + QString::fromLatin1(b64);
        } else {
            QByteArray digest = QCryptographicHash::hash(QByteArray(key, int(keyLen)), QCryptographicHash::Sha256);
            fingerprint = QStringLiteral("SHA256:") + QString::fromLatin1(digest.toBase64(QByteArray::OmitTrailingEquals));
        }

        const QByteArray hostUtf8 = m_opts.host.trimmed().toUtf8();
        int check = LIBSSH2_KNOWNHOST_CHECK_FAILURE;
        {
            std::lock_guard<std::mutex> lock(g_knownHostsMx);
            LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(m_session);
            if (kh) {
                const QByteArray data = readKnownHosts();
                parseKnownHosts(kh, data);
                libssh2_knownhost *found = nullptr;
                check = libssh2_knownhost_checkp(kh, hostUtf8.constData(), m_opts.port, key, keyLen,
                                                 LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW, &found);
                libssh2_knownhost_free(kh);
            }
        }

        if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH)
            return true;

        const QString hostDisplay = m_opts.port == 22 ? m_opts.host : QStringLiteral("%1:%2").arg(m_opts.host).arg(m_opts.port);
        const bool mismatch = check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH;
        if (m_opts.hostKeyPolicy != HostKeyPolicy::Ask) {
            if (mismatch)
                return fail(QStringLiteral("Host key for %1 has CHANGED (%2 %3). Connection refused by the host key policy. "
                                           "If the device was re-keyed, remove its line from known_hosts.")
                                .arg(hostDisplay, hostKeyTypeName(keyType), fingerprint));
            if (m_opts.hostKeyPolicy == HostKeyPolicy::Strict)
                return fail(QStringLiteral("Host %1 is not in known_hosts (%2 %3). Connection refused by the strict "
                                           "host key policy.")
                                .arg(hostDisplay, hostKeyTypeName(keyType), fingerprint));
            saveHostKey(key, keyLen, keyType, false);
            Q_EMIT notice(QStringLiteral("New host %1 trusted: %2 %3").arg(hostDisplay, hostKeyTypeName(keyType), fingerprint));
            return true;
        }

        PromptRequest req;
        if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
            req.kind = PromptRequest::Kind::HostKeyChanged;
            req.title = QStringLiteral("WARNING: host key has changed");
            req.message = QStringLiteral(
                              "The host key for %1 does not match the one saved in known_hosts.\n\n"
                              "This can mean someone is intercepting the connection, or the device was "
                              "replaced / re-keyed.\n\nNew %2 key fingerprint:\n%3")
                              .arg(hostDisplay, hostKeyTypeName(keyType), fingerprint);
        } else {
            req.kind = PromptRequest::Kind::HostKeyUnknown;
            req.title = QStringLiteral("Unknown host key");
            req.message = QStringLiteral("First connection to %1.\n\n%2 key fingerprint:\n%3\n\n"
                                         "Do you trust this host?")
                              .arg(hostDisplay, hostKeyTypeName(keyType), fingerprint);
        }
        const auto reply = ask(req);
        if (!reply || !reply->accepted)
            return fail(QStringLiteral("Host key not accepted"));
        if (reply->remember)
            saveHostKey(key, keyLen, keyType, check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH);
        return true;
    }

    QByteArray readKnownHosts() const
    {
        QFile f(m_knownHosts);
        if (!f.open(QIODevice::ReadOnly))
            return QByteArray();
        return f.read(16 * 1024 * 1024);
    }

    static void parseKnownHosts(LIBSSH2_KNOWNHOSTS *kh, const QByteArray &data)
    {
        // Parse line by line from memory (no fopen: paths may be non-ASCII on Windows).
        for (const QByteArray &rawLine : data.split('\n')) {
            const QByteArray line = rawLine.trimmed();
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            libssh2_knownhost_readline(kh, line.constData(), size_t(line.size()), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        }
    }

    void saveHostKey(const char *key, size_t keyLen, int keyType, bool replace)
    {
        const int bit = knownHostKeyBit(keyType);
        if (bit == LIBSSH2_KNOWNHOST_KEY_UNKNOWN) {
            Q_EMIT notice(QStringLiteral("Host key type not supported by known_hosts; key was accepted for this session only."));
            return;
        }
        std::lock_guard<std::mutex> lock(g_knownHostsMx);
        LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(m_session);
        if (!kh)
            return;
        parseKnownHosts(kh, readKnownHosts());
        const QByteArray hostUtf8 = m_opts.host.trimmed().toUtf8();
        const QByteArray entryName = m_opts.port == 22 ? hostUtf8 : "[" + hostUtf8 + "]:" + QByteArray::number(m_opts.port);
        if (replace) {
            // Drop every existing entry for this host name (all key types).
            libssh2_knownhost *store = nullptr;
            libssh2_knownhost *prev = nullptr;
            QVector<libssh2_knownhost *> doomed;
            while (libssh2_knownhost_get(kh, &store, prev) == 0) {
                if (store->name && entryName == store->name)
                    doomed.push_back(store);
                prev = store;
            }
            for (auto *e : doomed)
                libssh2_knownhost_del(kh, e);
        }
        const QByteArray comment("added by TermX");
        libssh2_knownhost_addc(kh, entryName.constData(), nullptr, key, keyLen, comment.constData(), size_t(comment.size()),
                               LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | bit, nullptr);

        QByteArray out;
        libssh2_knownhost *store = nullptr;
        libssh2_knownhost *prev = nullptr;
        char line[8192];
        while (libssh2_knownhost_get(kh, &store, prev) == 0) {
            size_t outLen = 0;
            if (libssh2_knownhost_writeline(kh, store, line, sizeof(line), &outLen, LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0)
                out.append(line, int(outLen));
            prev = store;
        }
        libssh2_knownhost_free(kh);

        QDir().mkpath(QFileInfo(m_knownHosts).absolutePath());
        QSaveFile f(m_knownHosts);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(out);
            if (!f.commit())
                Q_EMIT notice(QStringLiteral("Could not save known_hosts: %1").arg(f.errorString()));
        }
    }

    // ---- authentication ----------------------------------------------------
    bool authenticate()
    {
        QDeadlineTimer deadline(QDeadlineTimer::Forever);
        if (m_opts.username.trimmed().isEmpty()) {
            PromptRequest req;
            req.title = QStringLiteral("Login");
            req.message = QStringLiteral("Username for %1").arg(m_opts.host);
            req.fields = {PromptField{QStringLiteral("Username"), true, QString()}};
            const auto r = ask(req);
            if (!r || !r->accepted || r->values.isEmpty() || r->values.first().isEmpty())
                return fail(QStringLiteral("Login cancelled"));
            m_opts.username = r->values.first();
        }
        m_user = m_opts.username.toUtf8();

        char *list = nullptr;
        for (;;) {
            list = libssh2_userauth_list(m_session, m_user.constData(), unsigned(m_user.size()));
            if (list || libssh2_userauth_authenticated(m_session))
                break;
            if (libssh2_session_last_errno(m_session) != LIBSSH2_ERROR_EAGAIN)
                return fail(QStringLiteral("SSH authentication failed: %1").arg(sshError(m_session)));
            if (!waitForSession(deadline))
                return fail(QStringLiteral("Cancelled"));
        }
        if (libssh2_userauth_authenticated(m_session))
            return true;   // "none" authentication accepted
        const QString methods = QString::fromLatin1(list);

        if (methods.contains(QLatin1String("publickey"))) {
            if (!m_opts.keyFile.isEmpty() && tryKeyFile())
                return true;
            if (m_opts.useAgent && tryAgent())
                return true;
        }
        if (stopRequested())
            return fail(QStringLiteral("Cancelled"));
        if (methods.contains(QLatin1String("keyboard-interactive")) && tryKeyboardInteractive())
            return true;
        if (stopRequested())
            return fail(QStringLiteral("Cancelled"));
        if (methods.contains(QLatin1String("password")) && tryPassword())
            return true;
        if (stopRequested())
            return fail(QStringLiteral("Cancelled"));
        return fail(QStringLiteral("Authentication failed (server offers: %1)").arg(methods));
    }

    bool tryKeyFile()
    {
        QFile f(m_opts.keyFile);
        if (!f.open(QIODevice::ReadOnly)) {
            Q_EMIT notice(QStringLiteral("Cannot read key file %1: %2").arg(m_opts.keyFile, f.errorString()));
            return false;
        }
        const QByteArray priv = f.read(1024 * 1024);
        QByteArray passphrase;
        QDeadlineTimer deadline(QDeadlineTimer::Forever);
        for (int attempt = 0; attempt < 4 && !stopRequested(); ++attempt) {
            const int rc = retry([&] {
                return libssh2_userauth_publickey_frommemory(m_session, m_user.constData(), size_t(m_user.size()),
                                                             nullptr, 0, priv.constData(), size_t(priv.size()),
                                                             passphrase.isEmpty() ? nullptr : passphrase.constData());
            }, deadline);
            if (rc == 0)
                return true;
            // Wrong / missing passphrase shows up as a file/key decoding error.
            if (rc != LIBSSH2_ERROR_FILE && rc != LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED && rc != LIBSSH2_ERROR_METHOD_NOT_SUPPORTED) {
                Q_EMIT notice(QStringLiteral("Public key rejected: %1").arg(sshError(m_session)));
                return false;
            }
            PromptRequest req;
            req.title = QStringLiteral("Key passphrase");
            req.message = attempt == 0 ? QStringLiteral("Passphrase for %1").arg(QFileInfo(m_opts.keyFile).fileName())
                                       : QStringLiteral("Wrong passphrase, try again (%1)").arg(QFileInfo(m_opts.keyFile).fileName());
            req.fields = {PromptField{QStringLiteral("Passphrase"), false, QString()}};
            const auto r = ask(req);
            if (!r || !r->accepted || r->values.isEmpty())
                return false;
            passphrase = r->values.first().toUtf8();
        }
        return false;
    }

    bool tryAgent()
    {
        LIBSSH2_AGENT *agent = libssh2_agent_init(m_session);
        if (!agent)
            return false;
        bool ok = false;
        // Agent IPC is local and quick; temporarily use blocking mode for it.
        libssh2_session_set_blocking(m_session, 1);
        libssh2_session_set_timeout(m_session, 15000);
        if (libssh2_agent_connect(agent) == 0) {
            if (libssh2_agent_list_identities(agent) == 0) {
                libssh2_agent_publickey *identity = nullptr;
                libssh2_agent_publickey *prev = nullptr;
                while (!stopRequested() && libssh2_agent_get_identity(agent, &identity, prev) == 0) {
                    if (libssh2_agent_userauth(agent, m_user.constData(), identity) == 0) {
                        ok = true;
                        break;
                    }
                    prev = identity;
                }
            }
            libssh2_agent_disconnect(agent);
        }
        libssh2_agent_free(agent);
        libssh2_session_set_timeout(m_session, 0);
        libssh2_session_set_blocking(m_session, 0);
        return ok;
    }

    static void kbdCallback(const char *name, int nameLen, const char *instruction, int instructionLen,
                            int numPrompts, const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                            LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
    {
        auto *self = static_cast<SshWorker *>(*abstract);
        self->answerKbd(QString::fromUtf8(name, nameLen), QString::fromUtf8(instruction, instructionLen),
                        numPrompts, prompts, responses);
    }

    void answerKbd(const QString &name, const QString &instruction, int numPrompts,
                   const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts, LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses)
    {
        QStringList answers;
        // Common case: a single hidden "Password:" prompt -> reuse the saved password once.
        if (numPrompts == 1 && !prompts[0].echo && !m_password.isEmpty() && !m_savedPasswordTried) {
            m_savedPasswordTried = true;
            answers << m_password;
        } else if (numPrompts > 0) {
            PromptRequest req;
            req.title = name.isEmpty() ? QStringLiteral("Login") : name;
            req.message = instruction.isEmpty() ? QStringLiteral("%1@%2").arg(m_opts.username, m_opts.host) : instruction;
            bool singleSecret = numPrompts == 1 && !prompts[0].echo;
            req.offerRemember = singleSecret;
            for (int i = 0; i < numPrompts; ++i) {
                PromptField f;
                f.label = QString::fromUtf8(reinterpret_cast<const char *>(prompts[i].text), int(prompts[i].length)).trimmed();
                f.echo = prompts[i].echo != 0;
                req.fields.push_back(f);
            }
            const auto r = ask(req);
            if (r && r->accepted) {
                answers = r->values;
                if (singleSecret && !answers.isEmpty()) {
                    m_password = answers.first();
                    m_rememberPassword = r->remember;
                    m_savedPasswordTried = true;
                }
            } else {
                m_kbdCancelled = true;
            }
        }
        for (int i = 0; i < numPrompts; ++i) {
            QByteArray a = i < answers.size() ? answers.at(i).toUtf8() : QByteArray();
            char *buf = static_cast<char *>(std::malloc(size_t(a.size()) + 1));
            if (buf) {
                std::memcpy(buf, a.constData(), size_t(a.size()));
                buf[a.size()] = '\0';
            }
            responses[i].text = buf;
            responses[i].length = buf ? static_cast<decltype(responses[i].length)>(a.size()) : 0;
            secureWipe(a);
        }
        secureWipe(answers);
    }

    bool tryKeyboardInteractive()
    {
        QDeadlineTimer deadline(QDeadlineTimer::Forever);
        for (int attempt = 0; attempt < 3 && !stopRequested() && !m_kbdCancelled; ++attempt) {
            const int rc = retry([&] {
                return libssh2_userauth_keyboard_interactive_ex(m_session, m_user.constData(), unsigned(m_user.size()),
                                                                &SshWorker::kbdCallback);
            }, deadline);
            if (rc == 0) {
                if (m_rememberPassword && !m_password.isEmpty())
                    Q_EMIT secretAccepted(m_password);
                return true;
            }
            if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED && rc != LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED)
                return false;
            Q_EMIT notice(QStringLiteral("Access denied"));
        }
        return false;
    }

    bool tryPassword()
    {
        QDeadlineTimer deadline(QDeadlineTimer::Forever);
        bool useSaved = !m_password.isEmpty() && !m_savedPasswordTried;
        for (int attempt = 0; attempt < 3 && !stopRequested(); ++attempt) {
            if (!useSaved) {
                PromptRequest req;
                req.title = QStringLiteral("Password");
                req.message = attempt == 0 ? QStringLiteral("Password for %1@%2").arg(m_opts.username, m_opts.host)
                                           : QStringLiteral("Access denied. Password for %1@%2").arg(m_opts.username, m_opts.host);
                req.fields = {PromptField{QStringLiteral("Password"), false, QString()}};
                req.offerRemember = true;
                const auto r = ask(req);
                if (!r || !r->accepted || r->values.isEmpty())
                    return false;
                m_password = r->values.first();
                m_rememberPassword = r->remember;
            }
            useSaved = false;
            QByteArray pw = m_password.toUtf8();
            const int rc = retry([&] {
                return libssh2_userauth_password_ex(m_session, m_user.constData(), unsigned(m_user.size()),
                                                    pw.constData(), unsigned(pw.size()), nullptr);
            }, deadline);
            secureWipe(pw);
            if (rc == 0) {
                if (m_rememberPassword)
                    Q_EMIT secretAccepted(m_password);
                return true;
            }
            if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED)
                return false;
        }
        return false;
    }

    // ---- shell -------------------------------------------------------------
    bool openShell()
    {
        QDeadlineTimer deadline(qint64(m_opts.connectTimeoutSeconds) * 1000);
        for (;;) {
            m_channel = libssh2_channel_open_session(m_session);
            if (m_channel)
                break;
            if (libssh2_session_last_errno(m_session) != LIBSSH2_ERROR_EAGAIN)
                return fail(QStringLiteral("Cannot open SSH channel: %1").arg(sshError(m_session)));
            if (!waitForSession(deadline))
                return fail(QStringLiteral("Timed out opening SSH channel"));
        }
        const QByteArray term = m_opts.termType.isEmpty() ? QByteArray("xterm-256color") : m_opts.termType.toLatin1();
        m_cols = m_shared->cols.load();
        m_rows = m_shared->rows.load();
        m_shared->resizePending.store(false);
        int rc = retry([&] {
            return libssh2_channel_request_pty_ex(m_channel, term.constData(), unsigned(term.size()), nullptr, 0,
                                                  m_cols, m_rows, 0, 0);
        }, deadline);
        if (rc != 0)
            return fail(QStringLiteral("Server refused PTY: %1").arg(sshError(m_session)));
        rc = retry([&] { return libssh2_channel_shell(m_channel); }, deadline);
        if (rc != 0)
            return fail(QStringLiteral("Server refused shell: %1").arg(sshError(m_session)));
        return true;
    }

    // ---- control requests --------------------------------------------------
    void takeControl(ssh::ForwardEngine &fwd, ssh::SftpEngine &sftp)
    {
        std::vector<std::pair<quint64, PortForward>> adds;
        std::vector<quint64> removes;
        std::deque<SftpRequest> reqs;
        {
            std::lock_guard<std::mutex> lock(m_control->mx);
            adds.swap(m_control->adds);
            removes.swap(m_control->removes);
            reqs.swap(m_control->sftp);
        }
        for (const auto &a : adds)
            fwd.add(a.first, a.second);
        for (quint64 id : removes)
            fwd.remove(id);
        for (const SftpRequest &r : reqs)
            sftp.submit(r);
    }

    // The worker is going away: answer whatever is still queued.
    void closeControl()
    {
        std::deque<SftpRequest> left;
        {
            std::lock_guard<std::mutex> lock(m_control->mx);
            m_control->finished = true;
            left.swap(m_control->sftp);
            m_control->adds.clear();
            m_control->removes.clear();
        }
        const QString why = m_exitReason.isEmpty() ? QStringLiteral("Not connected") : m_exitReason;
        for (const SftpRequest &r : left) {
            SftpResult res;
            res.id = r.id;
            res.op = r.op;
            res.path = r.path;
            res.localPath = r.localPath;
            res.error = why;
            Q_EMIT sftpFinished(res);
        }
        Q_EMIT forwardsChanged({});
    }

    bool sftpCancelled(quint64 id)
    {
        std::lock_guard<std::mutex> lock(m_control->mx);
        return m_control->cancelled.count(id) != 0;
    }

    // ---- shell ---------------------------------------------------------------
    // Resumable: when a libssh2 call leaves a packet half-sent, the next turn
    // starts with that same call (see SshContext.h).
    enum class ShellStep { Idle, Moved, Exit };

    ShellStep stepShell(ssh::Context &ctx, QString &exitReason)
    {
        const void *tag = &m_shellPhase;
        if (!ctx.mayRun(tag))
            return ShellStep::Idle;
        bool moved = false;
        for (;;) {
            switch (m_shellPhase) {
            case 0: {   // resize
                if (m_shared->resizePending.exchange(false)) {
                    m_cols = m_shared->cols.load();
                    m_rows = m_shared->rows.load();
                    m_resizeDirty = true;
                }
                if (m_resizeDirty) {
                    const int rc = libssh2_channel_request_pty_size(m_channel, m_cols, m_rows);
                    ctx.after(tag, rc);
                    if (rc == LIBSSH2_ERROR_EAGAIN) {
                        if (ctx.owner == tag)
                            return moved ? ShellStep::Moved : ShellStep::Idle;
                    } else {
                        m_resizeDirty = false;
                    }
                }
                m_shellPhase = 1;
                break;
            }
            case 1: {   // keyboard -> server
                takeWrites(m_pendingWrites);
                m_writeBlocked = false;
                while (!m_pendingWrites.empty()) {
                    const QByteArray &chunk = m_pendingWrites.front();
                    const ssize_t n = libssh2_channel_write(m_channel, chunk.constData() + m_pendingOffset,
                                                            size_t(chunk.size() - m_pendingOffset));
                    ctx.after(tag, n < 0 ? long(n) : 0);
                    if (n == LIBSSH2_ERROR_EAGAIN) {
                        m_writeBlocked = true;
                        if (ctx.owner == tag)
                            return moved ? ShellStep::Moved : ShellStep::Idle;
                        break;
                    }
                    if (n < 0) {
                        exitReason = QStringLiteral("Write failed: %1").arg(ssh::lastError(m_session));
                        return ShellStep::Exit;
                    }
                    moved = true;
                    m_pendingOffset += n;
                    if (m_pendingOffset >= chunk.size()) {
                        m_pendingWrites.pop_front();
                        m_pendingOffset = 0;
                    }
                }
                m_shellPhase = 2;
                m_shellStream = 0;
                break;
            }
            case 2: {   // server -> screen (paused while the UI is behind: SSH window applies back-pressure)
                if (!flowBlocked()) {
                    for (; m_shellStream < 2; ++m_shellStream) {
                        for (int guard = 0; guard < 64; ++guard) {
                            const ssize_t n = libssh2_channel_read_ex(m_channel, m_shellStream, m_readBuf.data(),
                                                                      size_t(m_readBuf.size()));
                            ctx.after(tag, n < 0 ? long(n) : 0);
                            if (n > 0) {
                                emitData(QByteArray(m_readBuf.constData(), int(n)));
                                moved = true;
                                if (flowBlocked())
                                    break;
                                continue;
                            }
                            if (n == LIBSSH2_ERROR_EAGAIN && ctx.owner == tag)
                                return moved ? ShellStep::Moved : ShellStep::Idle;
                            if (n == LIBSSH2_ERROR_EAGAIN || n == 0)
                                break;
                            exitReason = QStringLiteral("Connection lost: %1").arg(ssh::lastError(m_session));
                            return ShellStep::Exit;
                        }
                    }
                }
                m_shellPhase = 3;
                break;
            }
            default: {  // end of session?
                m_shellPhase = 0;
                if (libssh2_channel_eof(m_channel) == 1) {
                    const int code = libssh2_channel_get_exit_status(m_channel);
                    exitReason = QStringLiteral("Session closed by remote host (exit status %1)").arg(code);
                    return ShellStep::Exit;
                }
                return moved ? ShellStep::Moved : ShellStep::Idle;
            }
            }
        }
    }

    void pump()
    {
        ssh::Context ctx;
        ctx.session = m_session;
        ctx.sock = m_sock;
        m_readBuf = QByteArray(32 * 1024, Qt::Uninitialized);

        ssh::ForwardEngine fwd(
            ctx, [this](const QVector<ForwardStatus> &all) { Q_EMIT forwardsChanged(all); },
            [this](const QString &n) { Q_EMIT notice(n); });
        ssh::SftpEngine sftp(
            ctx, [this](const SftpResult &r) { Q_EMIT sftpFinished(r); },
            [this](const SftpProgress &p) { Q_EMIT sftpProgress(p); },
            [this](quint64 id) { return sftpCancelled(id); });

        for (const PortForward &f : std::as_const(m_opts.forwards)) {
            if (f.enabled && f.isValid())
                fwd.add(m_control->nextId.fetch_add(1), f);
        }

        QString exitReason;
        int nextKeepAlive = 30;
        std::vector<net::PollItem> items;
        const void *keepAliveTag = &nextKeepAlive;

        while (!stopRequested()) {
            takeControl(fwd, sftp);

            bool progress = false;
            const ShellStep shell = stepShell(ctx, exitReason);
            if (shell == ShellStep::Exit)
                break;
            progress |= shell == ShellStep::Moved;
            progress |= fwd.step();
            progress |= sftp.step();

            // Keep-alive: libssh2 drops its own EAGAIN here, which would leave
            // a half-sent packet nobody finishes. Only send into a writable,
            // otherwise idle socket.
            if (m_opts.keepAliveSeconds > 0 && !ctx.owner && !ctx.outboundBlocked()
                && (net::waitSocket(m_sock, false, true, m_waker, 0) & net::WaitSocket)) {
                if (libssh2_keepalive_send(m_session, &nextKeepAlive) != 0
                    && libssh2_session_last_errno(m_session) != LIBSSH2_ERROR_EAGAIN) {
                    exitReason = QStringLiteral("Connection lost (keep-alive failed)");
                    break;
                }
                ctx.after(keepAliveTag, 0);
            }

            if (ctx.owner && ctx.ownerStalled(120000)) {
                exitReason = QStringLiteral("Connection stalled: the server stopped accepting data");
                break;
            }

            // Data libssh2 already pulled off the socket for some channel does
            // not make the socket readable again: look for it before sleeping.
            const bool buffered = (!flowBlocked() && libssh2_poll_channel_read(m_channel, 1) > 0)
                                  || fwd.hasBufferedData() || sftp.hasBufferedData();
            int timeout = flowBlocked() ? 20 : qBound(10, nextKeepAlive * 1000, 1000);
            if (fwd.waitingOnSession() || sftp.busy() || ctx.owner || ctx.openOwner || !m_pendingWrites.empty())
                timeout = qMin(timeout, 10);
            if (progress || buffered)
                timeout = 0;

            items.clear();
            net::PollItem sshItem;
            sshItem.s = m_sock;
            sshItem.wantRead = !flowBlocked() || fwd.readsSession() || sftp.busy();
            sshItem.wantWrite = m_writeBlocked || ctx.outboundBlocked()
                                || (libssh2_session_block_directions(m_session) & LIBSSH2_SESSION_BLOCK_OUTBOUND);
            items.push_back(sshItem);
            fwd.collectPoll(items);
            const int r = net::waitMany(items, m_waker, timeout);
            if (r == net::WaitError) {
                exitReason = QStringLiteral("Socket error: %1").arg(net::lastErrorString());
                break;
            }
            if (r & net::WaitWoken)
                m_waker.drain();
        }

        m_exitReason = stopRequested() ? QStringLiteral("Disconnected") : exitReason;
        sftp.abort(m_exitReason);
        fwd.abandon();
        ctx.owner = nullptr;
        ctx.openOwner = nullptr;
        setState(TransportState::Disconnected, m_exitReason);
    }

    SshOptions m_opts;
    QString m_password;
    QString m_knownHosts;
    std::shared_ptr<SshControl> m_control;
    QString m_exitReason;
    std::deque<QByteArray> m_pendingWrites;
    qsizetype m_pendingOffset = 0;
    bool m_writeBlocked = false;
    int m_shellPhase = 0;
    int m_shellStream = 0;
    QByteArray m_readBuf;
    QByteArray m_user;
    bool m_savedPasswordTried = false;
    bool m_rememberPassword = false;
    bool m_kbdCancelled = false;
    bool m_resizeDirty = false;
    int m_cols = 80;
    int m_rows = 24;

    net::Waker m_waker;
    net::socket_t m_sock = net::kInvalidSocket;
    LIBSSH2_SESSION *m_session = nullptr;
    LIBSSH2_CHANNEL *m_channel = nullptr;
};

} // namespace

SshTransport::SshTransport(const SshOptions &opts, const QString &savedPassword,
                           const QString &knownHostsPath, QObject *parent)
    : Transport(parent), m_opts(opts), m_password(savedPassword), m_knownHosts(knownHostsPath),
      m_control(std::make_shared<SshControl>())
{
    registerSshMetaTypes();
}

SshTransport::~SshTransport()
{
    secureWipe(m_password);
}

TransportWorker *SshTransport::createWorker()
{
    auto *w = new SshWorker(m_opts, m_password, m_knownHosts, m_control);
    connect(w, &SshWorker::sftpProgress, this, &SshTransport::sftpProgress);
    connect(w, &SshWorker::sftpFinished, this, [this](const SftpResult &r) {
        {
            std::lock_guard<std::mutex> lock(m_control->mx);
            m_control->cancelled.erase(r.id);
        }
        Q_EMIT sftpFinished(r);
    });
    connect(w, &SshWorker::forwardsChanged, this, [this](const QVector<ForwardStatus> &all) {
        m_forwards = all;
        Q_EMIT forwardsChanged(all);
    });
    return w;
}

void SshTransport::failLater(const SftpRequest &req, const QString &why)
{
    SftpResult res;
    res.id = req.id;
    res.op = req.op;
    res.path = req.path;
    res.localPath = req.localPath;
    res.error = why;
    QMetaObject::invokeMethod(this, [this, res] { Q_EMIT sftpFinished(res); }, Qt::QueuedConnection);
}

quint64 SshTransport::submitSftp(SftpRequest req)
{
    req.id = m_control->nextId.fetch_add(1);
    bool finished;
    {
        std::lock_guard<std::mutex> lock(m_control->mx);
        finished = m_control->finished;
        if (!finished)
            m_control->sftp.push_back(req);
    }
    if (finished)
        failLater(req, QStringLiteral("Not connected"));
    else
        wakeWorker();
    return req.id;
}

void SshTransport::cancelSftp(quint64 id)
{
    {
        std::lock_guard<std::mutex> lock(m_control->mx);
        if (m_control->finished)
            return;
        m_control->cancelled.insert(id);
        // Not started yet: drop it from the queue.
        for (auto it = m_control->sftp.begin(); it != m_control->sftp.end(); ++it) {
            if (it->id == id) {
                SftpRequest r = *it;
                m_control->sftp.erase(it);
                m_control->cancelled.erase(id);
                SftpResult res;
                res.id = r.id;
                res.op = r.op;
                res.path = r.path;
                res.localPath = r.localPath;
                res.cancelled = true;
                res.error = QStringLiteral("Cancelled");
                QMetaObject::invokeMethod(this, [this, res] { Q_EMIT sftpFinished(res); }, Qt::QueuedConnection);
                return;
            }
        }
    }
    wakeWorker();
}

quint64 SshTransport::startForward(const PortForward &rule)
{
    const quint64 id = m_control->nextId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(m_control->mx);
        if (m_control->finished)
            return 0;
        m_control->adds.emplace_back(id, rule);
    }
    wakeWorker();
    return id;
}

void SshTransport::stopForward(quint64 id)
{
    {
        std::lock_guard<std::mutex> lock(m_control->mx);
        if (m_control->finished)
            return;
        m_control->removes.push_back(id);
    }
    wakeWorker();
}

void registerSshMetaTypes()
{
    static std::once_flag once;
    std::call_once(once, [] {
        qRegisterMetaType<SftpRequest>("termx::SftpRequest");
        qRegisterMetaType<SftpResult>("termx::SftpResult");
        qRegisterMetaType<SftpProgress>("termx::SftpProgress");
        qRegisterMetaType<PortForward>("termx::PortForward");
        qRegisterMetaType<ForwardStatus>("termx::ForwardStatus");
        qRegisterMetaType<QVector<ForwardStatus>>("QVector<termx::ForwardStatus>");
    });
}

bool SshTransport::globalInit()
{
    net::initialize();
    return libssh2_init(0) == 0;
}

void SshTransport::globalCleanup()
{
    libssh2_exit();
}

} // namespace termx

#include "SshTransport.moc"

// SPDX-License-Identifier: GPL-3.0-or-later
#include "SshTransport.h"
#include "SocketUtil.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <libssh2.h>

#include <cstdlib>
#include <cstring>

namespace termx {

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
const char *kCipherModern =
    "aes128-gcm@openssh.com,aes256-gcm@openssh.com,aes128-ctr,aes192-ctr,aes256-ctr";
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
public:
    SshWorker(SshOptions o, QString password, QString knownHosts)
        : m_opts(std::move(o)), m_password(std::move(password)), m_knownHosts(std::move(knownHosts))
    {
    }

    ~SshWorker() override
    {
        if (m_shared) {
            std::lock_guard<std::mutex> lock(m_shared->mx);
            m_shared->waker = nullptr;
        }
        cleanup();
    }

    void start() override
    {
        run();
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
        if (m_channel) {
            libssh2_channel_free(m_channel);
            m_channel = nullptr;
        }
        if (m_session) {
            // Non-blocking disconnect: a dead peer must not hang us.
            libssh2_session_set_blocking(m_session, 0);
            libssh2_session_disconnect(m_session, "Normal shutdown");
            libssh2_session_free(m_session);
            m_session = nullptr;
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
        QString err;
        m_sock = net::tcpConnect(m_opts.host, uint16_t(m_opts.port), m_opts.connectTimeoutSeconds * 1000,
                                 m_shared->stop, err);
        if (m_sock == net::kInvalidSocket) {
            fail(err);
            return;
        }
        m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
        if (!m_session) {
            fail(QStringLiteral("Cannot create SSH session"));
            return;
        }
        libssh2_session_set_blocking(m_session, 0);
        setMethodPreferences();

        QDeadlineTimer deadline(qint64(m_opts.connectTimeoutSeconds) * 1000);
        int rc = retry([&] { return libssh2_session_handshake(m_session, m_sock); }, deadline);
        if (rc != 0) {
            if (rc == LIBSSH2_ERROR_KEX_FAILURE && !m_opts.allowLegacyAlgorithms)
                fail(QStringLiteral("SSH handshake failed: no common algorithms. The device may need legacy "
                                    "algorithms (enable them in the session settings)."));
            else
                fail(QStringLiteral("SSH handshake failed: %1").arg(sshError(m_session)));
            return;
        }
        reportLegacyMethods();

        if (!verifyHostKey())
            return;
        if (!authenticate())
            return;
        if (!openShell())
            return;

        if (m_opts.keepAliveSeconds > 0)
            libssh2_keepalive_config(m_session, 0, unsigned(m_opts.keepAliveSeconds));

        setState(TransportState::Connected, QString());
        pump();
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

        PromptRequest req;
        const QString hostDisplay = m_opts.port == 22 ? m_opts.host : QStringLiteral("%1:%2").arg(m_opts.host).arg(m_opts.port);
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
            const QByteArray a = i < answers.size() ? answers.at(i).toUtf8() : QByteArray();
            char *buf = static_cast<char *>(std::malloc(size_t(a.size()) + 1));
            if (buf) {
                std::memcpy(buf, a.constData(), size_t(a.size()));
                buf[a.size()] = '\0';
            }
            responses[i].text = buf;
            responses[i].length = buf ? static_cast<decltype(responses[i].length)>(a.size()) : 0;
        }
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
            const QByteArray pw = m_password.toUtf8();
            const int rc = retry([&] {
                return libssh2_userauth_password_ex(m_session, m_user.constData(), unsigned(m_user.size()),
                                                    pw.constData(), unsigned(pw.size()), nullptr);
            }, deadline);
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

    void pump()
    {
        std::deque<QByteArray> pending;
        qsizetype pendingOffset = 0;
        QByteArray buf(32 * 1024, Qt::Uninitialized);
        QString exitReason;

        while (!stopRequested()) {
            // Resize.
            if (m_shared->resizePending.exchange(false)) {
                m_cols = m_shared->cols.load();
                m_rows = m_shared->rows.load();
                m_resizeDirty = true;
            }
            if (m_resizeDirty) {
                const int rc = libssh2_channel_request_pty_size(m_channel, m_cols, m_rows);
                if (rc != LIBSSH2_ERROR_EAGAIN)
                    m_resizeDirty = false;
            }

            // Writes.
            takeWrites(pending);
            bool writeBlocked = false;
            while (!pending.empty()) {
                const QByteArray &chunk = pending.front();
                const ssize_t n = libssh2_channel_write(m_channel, chunk.constData() + pendingOffset,
                                                        size_t(chunk.size() - pendingOffset));
                if (n == LIBSSH2_ERROR_EAGAIN) {
                    writeBlocked = true;
                    break;
                }
                if (n < 0) {
                    exitReason = QStringLiteral("Write failed: %1").arg(sshError(m_session));
                    break;
                }
                pendingOffset += n;
                if (pendingOffset >= chunk.size()) {
                    pending.pop_front();
                    pendingOffset = 0;
                }
            }
            if (!exitReason.isEmpty())
                break;

            // Reads (paused while the UI is behind -> SSH window applies back-pressure).
            bool readError = false;
            if (!flowBlocked()) {
                for (int stream = 0; stream < 2 && !readError; ++stream) {
                    for (int guard = 0; guard < 64; ++guard) {
                        const ssize_t n = libssh2_channel_read_ex(m_channel, stream, buf.data(), size_t(buf.size()));
                        if (n > 0) {
                            emitData(QByteArray(buf.constData(), int(n)));
                            continue;
                        }
                        if (n == LIBSSH2_ERROR_EAGAIN || n == 0)
                            break;
                        exitReason = QStringLiteral("Connection lost: %1").arg(sshError(m_session));
                        readError = true;
                        break;
                    }
                }
            }
            if (readError)
                break;
            if (libssh2_channel_eof(m_channel)) {
                int code = libssh2_channel_get_exit_status(m_channel);
                exitReason = QStringLiteral("Session closed by remote host (exit status %1)").arg(code);
                break;
            }

            int nextKeepAlive = 30;
            if (m_opts.keepAliveSeconds > 0) {
                if (libssh2_keepalive_send(m_session, &nextKeepAlive) != 0
                    && libssh2_session_last_errno(m_session) != LIBSSH2_ERROR_EAGAIN) {
                    exitReason = QStringLiteral("Connection lost (keep-alive failed)");
                    break;
                }
            }

            const int timeout = flowBlocked() ? 20 : qBound(10, nextKeepAlive * 1000, 1000);
            const int dir = libssh2_session_block_directions(m_session);
            const bool wantWrite = writeBlocked || (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND);
            const int r = net::waitSocket(m_sock, !flowBlocked(), wantWrite, m_waker, timeout);
            if (r == net::WaitError) {
                exitReason = QStringLiteral("Socket error: %1").arg(net::lastErrorString());
                break;
            }
            if (r & net::WaitWoken)
                m_waker.drain();
        }

        if (stopRequested())
            setState(TransportState::Disconnected, QStringLiteral("Disconnected"));
        else
            setState(TransportState::Disconnected, exitReason);
    }

    SshOptions m_opts;
    QString m_password;
    QString m_knownHosts;
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
    : Transport(parent), m_opts(opts), m_password(savedPassword), m_knownHosts(knownHostsPath)
{
}

TransportWorker *SshTransport::createWorker()
{
    return new SshWorker(m_opts, m_password, m_knownHosts);
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

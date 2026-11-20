// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end tests of SSH shell, SFTP and port forwarding against a real
// OpenSSH server started for the test (skipped when sshd is not installed).
#include "transport/SshTransport.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkProxy>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace termx;

namespace {

int freePort()
{
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    return s.serverPort();
}

QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    auto *g = QRandomGenerator::global();
    for (int i = 0; i + 4 <= n; i += 4) {
        const quint32 v = g->generate();
        memcpy(b.data() + i, &v, 4);
    }
    return b;
}

// Echo server living on the test thread.
class EchoServer : public QTcpServer {
public:
    EchoServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [s] { s->write(s->readAll()); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        listen(QHostAddress::LocalHost, 0);
    }
};

} // namespace

class TstSshIntegration : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void shellEcho();
    void sftpBasics();
    void sftpFolders();
    void sftpCancel();
    void localForward();
    void dynamicForwardSocks5();
    void dynamicForwardSocks4a();
    void remoteForward();
    void forwardErrors();
    void everythingAtOnce();
    void requestsAfterDisconnect();

private:
    bool connectTransport(const QVector<PortForward> &forwards = {});
    SftpResult sftp(SftpRequest req, int timeoutMs = 60000);
    ForwardStatus waitForward(quint64 id, int timeoutMs = 10000);
    bool echoThrough(QTcpSocket &sock, const QByteArray &payload, int timeoutMs = 30000);
    bool shellRoundTrip(const QString &marker);

    QTemporaryDir m_dir;
    QProcess m_sshd;
    int m_port = 0;
    QString m_user;
    QString m_key;
    QString m_skip;

    std::unique_ptr<SshTransport> m_t;
    QByteArray m_out;
    QMap<quint64, SftpResult> m_results;
    QVector<SftpProgress> m_progress;
    QVector<ForwardStatus> m_forwards;
};

void TstSshIntegration::initTestCase()
{
    registerTransportMetaTypes();
    registerSshMetaTypes();
    const QString sshd = QFile::exists(QStringLiteral("/usr/sbin/sshd")) ? QStringLiteral("/usr/sbin/sshd") : QString();
    const QString keygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
    if (sshd.isEmpty() || keygen.isEmpty()) {
        m_skip = QStringLiteral("OpenSSH server not installed");
        return;
    }
    QVERIFY(m_dir.isValid());
    const QString d = m_dir.path();
    for (const QString &k : {QStringLiteral("host"), QStringLiteral("client")}) {
        QProcess p;
        p.start(keygen, {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"),
                         QString(), QStringLiteral("-f"), d + QLatin1Char('/') + k});
        QVERIFY(p.waitForFinished(20000));
        QCOMPARE(p.exitCode(), 0);
    }
    QFile::copy(d + QStringLiteral("/client.pub"), d + QStringLiteral("/authorized_keys"));
    m_key = d + QStringLiteral("/client");
    QProcess who;
    who.start(QStringLiteral("id"), {QStringLiteral("-un")});
    QVERIFY(who.waitForFinished());
    m_user = QString::fromLocal8Bit(who.readAllStandardOutput()).trimmed();
    if (QFileInfo(QStringLiteral("/run")).isWritable())
        QDir().mkpath(QStringLiteral("/run/sshd"));

    m_port = freePort();
    QFile cfg(d + QStringLiteral("/sshd_config"));
    QVERIFY(cfg.open(QIODevice::WriteOnly));
    cfg.write(QStringLiteral("Port %1\nListenAddress 127.0.0.1\nHostKey %2/host\nAuthorizedKeysFile %2/authorized_keys\n"
                             "PidFile %2/sshd.pid\nUsePAM no\nStrictModes no\nAllowTcpForwarding yes\n"
                             "Subsystem sftp internal-sftp\nLogLevel ERROR\nMaxStartups 50\n")
                  .arg(m_port)
                  .arg(d)
                  .toUtf8());
    cfg.close();
    m_sshd.setProcessChannelMode(QProcess::ForwardedChannels);
    m_sshd.start(sshd, {QStringLiteral("-D"), QStringLiteral("-e"), QStringLiteral("-f"), cfg.fileName()});
    QVERIFY(m_sshd.waitForStarted());
    // Wait until it accepts connections.
    for (int i = 0; i < 50; ++i) {
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, quint16(m_port));
        if (probe.waitForConnected(200))
            return;
        if (m_sshd.state() != QProcess::Running)
            break;
        QTest::qWait(100);
    }
    m_skip = QStringLiteral("sshd did not start (needs /run/sshd when run as root)");
}

void TstSshIntegration::cleanupTestCase()
{
    if (m_sshd.state() == QProcess::Running) {
        m_sshd.terminate();
        m_sshd.waitForFinished(5000);
    }
}

void TstSshIntegration::init()
{
    if (!m_skip.isEmpty())
        QSKIP(qPrintable(m_skip));
    m_out.clear();
    m_results.clear();
    m_progress.clear();
    m_forwards.clear();
}

void TstSshIntegration::cleanup()
{
    if (m_t) {
        m_t->close();
        m_t.reset();
    }
    // The I/O thread quits through a queued call: keep the event loop running.
    QTRY_VERIFY_WITH_TIMEOUT(Transport::shutdownAll(50), 15000);
}

bool TstSshIntegration::connectTransport(const QVector<PortForward> &forwards)
{
    SshOptions o;
    o.host = QStringLiteral("127.0.0.1");
    o.port = m_port;
    o.username = m_user;
    o.keyFile = m_key;
    o.useAgent = false;
    o.hostKeyPolicy = HostKeyPolicy::AcceptNew;
    o.forwards = forwards;
    m_t = std::make_unique<SshTransport>(o, QString(), m_dir.path() + QStringLiteral("/known_hosts"));
    SshTransport *t = m_t.get();
    connect(t, &Transport::dataReceived, this, [this, t](const QByteArray &b) {
        m_out += b;
        t->acknowledge(b.size());
    });
    connect(t, &Transport::promptRequested, this, [t](const PromptRequest &r) {
        PromptReply reply;   // answer "no" to anything interactive
        t->answerPrompt(r.id, reply);
    });
    connect(t, &SshTransport::sftpFinished, this, [this](const SftpResult &r) { m_results.insert(r.id, r); });
    connect(t, &SshTransport::sftpProgress, this, [this](const SftpProgress &p) { m_progress << p; });
    connect(t, &SshTransport::forwardsChanged, this, [this](const QVector<ForwardStatus> &all) { m_forwards = all; });
    t->resize(120, 40);
    t->open();
    QElapsedTimer timer;
    timer.start();
    while (t->state() == TransportState::Connecting && timer.elapsed() < 20000)
        QTest::qWait(5);
    return t->state() == TransportState::Connected;
}

SftpResult TstSshIntegration::sftp(SftpRequest req, int timeoutMs)
{
    const quint64 id = m_t->submitSftp(req);
    QElapsedTimer t;
    t.start();
    while (!m_results.contains(id) && t.elapsed() < timeoutMs)
        QTest::qWait(5);
    SftpResult r = m_results.value(id);
    if (!m_results.contains(id))
        r.error = QStringLiteral("timeout");
    return r;
}

ForwardStatus TstSshIntegration::waitForward(quint64 id, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        for (const ForwardStatus &s : std::as_const(m_forwards))
            if (s.id == id && s.state != ForwardStatus::State::Starting)
                return s;
        QTest::qWait(5);
    }
    return ForwardStatus{};
}

bool TstSshIntegration::echoThrough(QTcpSocket &sock, const QByteArray &payload, int timeoutMs)
{
    QByteArray got;
    connect(&sock, &QTcpSocket::readyRead, &sock, [&] { got += sock.readAll(); });
    sock.write(payload);
    QElapsedTimer t;
    t.start();
    while (got.size() < payload.size() && t.elapsed() < timeoutMs && sock.state() == QAbstractSocket::ConnectedState)
        QTest::qWait(2);
    got += sock.readAll();
    sock.disconnect(&sock, nullptr, &sock, nullptr);
    return got == payload;
}

bool TstSshIntegration::shellRoundTrip(const QString &marker)
{
    // Split marker so the echo of the command line does not match.
    const int half = marker.size() / 2;
    m_t->write(QStringLiteral("echo %1''%2\n").arg(marker.left(half), marker.mid(half)).toUtf8());
    QElapsedTimer t;
    t.start();
    while (!m_out.contains(marker.toUtf8()) && t.elapsed() < 15000)
        QTest::qWait(5);
    return m_out.contains(marker.toUtf8());
}

void TstSshIntegration::shellEcho()
{
    QVERIFY(connectTransport());
    QVERIFY(shellRoundTrip(QStringLiteral("termx-hello-42")));
    // A big burst of output arrives complete and in order.
    m_out.clear();
    m_t->write("seq 1 200000 | tail -n 1; echo seq''done\n");
    QTRY_VERIFY_WITH_TIMEOUT(m_out.contains("seqdone"), 30000);
    QVERIFY(m_out.contains("200000"));
}

void TstSshIntegration::sftpBasics()
{
    QVERIFY(connectTransport());
    SftpRequest home;
    home.op = SftpOp::Home;
    SftpResult r = sftp(home);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(r.path.startsWith(QLatin1Char('/')));

    const QString remoteDir = m_dir.path() + QStringLiteral("/remote");
    SftpRequest mk;
    mk.op = SftpOp::Mkdir;
    mk.path = remoteDir;
    r = sftp(mk);
    QVERIFY2(r.ok, qPrintable(r.error));

    // Upload a file that is not a multiple of any buffer size.
    const QByteArray payload = randomBytes(5 * 1024 * 1024 + 12345);
    const QString local = m_dir.path() + QStringLiteral("/up.bin");
    {
        QFile f(local);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(payload);
    }
    SftpRequest up;
    up.op = SftpOp::Upload;
    up.localPath = local;
    up.path = remoteDir + QStringLiteral("/file one.bin");
    r = sftp(up);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.bytes, qint64(payload.size()));
    QVERIFY(!m_progress.isEmpty());
    QCOMPARE(m_progress.last().done, qint64(payload.size()));

    // Uploading again without overwrite is skipped, not an error.
    r = sftp(up);
    QVERIFY(r.ok);
    QCOMPARE(r.files, 0);
    QCOMPARE(r.skipped.size(), 1);

    SftpRequest ls;
    ls.op = SftpOp::List;
    ls.path = remoteDir;
    r = sftp(ls);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.entries.size(), 1);
    QCOMPARE(r.entries.first().name, QStringLiteral("file one.bin"));
    QCOMPARE(r.entries.first().size, qint64(payload.size()));
    QVERIFY(!r.entries.first().isDir);

    SftpRequest down;
    down.op = SftpOp::Download;
    down.path = remoteDir + QStringLiteral("/file one.bin");
    down.localPath = m_dir.path() + QStringLiteral("/down.bin");
    r = sftp(down);
    QVERIFY2(r.ok, qPrintable(r.error));
    {
        QFile f(down.localPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), payload);
    }
    QVERIFY(!QFile::exists(down.localPath + QStringLiteral(".termx-part")));

    SftpRequest mv;
    mv.op = SftpOp::Rename;
    mv.path = remoteDir + QStringLiteral("/file one.bin");
    mv.path2 = remoteDir + QStringLiteral("/renamed.bin");
    r = sftp(mv);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(QFile::exists(mv.path2));

    SftpRequest ch;
    ch.op = SftpOp::Chmod;
    ch.path = mv.path2;
    ch.mode = 0600;
    r = sftp(ch);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(QFileInfo(mv.path2).permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());

    SftpRequest missing;
    missing.op = SftpOp::List;
    missing.path = remoteDir + QStringLiteral("/nope");
    r = sftp(missing);
    QVERIFY(!r.ok);
    QVERIFY2(r.error.contains(QStringLiteral("no such")), qPrintable(r.error));

    SftpRequest rm;
    rm.op = SftpOp::Remove;
    rm.path = remoteDir;
    r = sftp(rm);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY(!QFileInfo::exists(remoteDir));

    SftpRequest root;
    root.op = SftpOp::Remove;
    root.path = QStringLiteral("/");
    r = sftp(root);
    QVERIFY(!r.ok);
}

void TstSshIntegration::sftpFolders()
{
    QVERIFY(connectTransport());
    // Local tree: a/b/c with files.
    const QString src = m_dir.path() + QStringLiteral("/tree");
    QVERIFY(QDir().mkpath(src + QStringLiteral("/a/b/c")));
    QVERIFY(QDir().mkpath(src + QStringLiteral("/empty")));
    QMap<QString, QByteArray> files{
        {QStringLiteral("top.txt"), "top"},
        {QStringLiteral("a/one.bin"), randomBytes(300000)},
        {QStringLiteral("a/b/two.bin"), randomBytes(70000)},
        {QStringLiteral("a/b/c/three.txt"), QByteArray()},
    };
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QFile f(src + QLatin1Char('/') + it.key());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(it.value());
    }
    QFile::link(src + QStringLiteral("/top.txt"), src + QStringLiteral("/link.txt"));

    SftpRequest up;
    up.op = SftpOp::Upload;
    up.localPath = src;
    up.path = m_dir.path() + QStringLiteral("/remote-tree");
    SftpResult r = sftp(up);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.files, 4);
    QCOMPARE(r.skipped.size(), 1);   // the symbolic link
    QVERIFY(QFileInfo(up.path + QStringLiteral("/empty")).isDir());

    SftpRequest down;
    down.op = SftpOp::Download;
    down.path = up.path;
    down.localPath = m_dir.path() + QStringLiteral("/back");
    r = sftp(down);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.files, 4);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QFile f(down.localPath + QLatin1Char('/') + it.key());
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
        QCOMPARE(f.readAll(), it.value());
    }
    QVERIFY(QFileInfo(down.localPath + QStringLiteral("/empty")).isDir());
    QVERIFY(m_progress.last().filesTotal == 4);

    SftpRequest rm;
    rm.op = SftpOp::Remove;
    rm.path = up.path;
    r = sftp(rm);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.files, 4);
    QVERIFY(!QFileInfo::exists(up.path));
}

void TstSshIntegration::sftpCancel()
{
    QVERIFY(connectTransport());
    const QString local = m_dir.path() + QStringLiteral("/big.bin");
    {
        QFile f(local);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray chunk = randomBytes(1024 * 1024);
        for (int i = 0; i < 400; ++i)
            f.write(chunk);
    }
    SftpRequest up;
    up.op = SftpOp::Upload;
    up.localPath = local;
    up.path = m_dir.path() + QStringLiteral("/big-remote.bin");
    const quint64 id = m_t->submitSftp(up);
    QTRY_VERIFY_WITH_TIMEOUT(!m_progress.isEmpty() && m_progress.last().done > 0, 20000);
    m_t->cancelSftp(id);
    QTRY_VERIFY_WITH_TIMEOUT(m_results.contains(id), 20000);
    const SftpResult r = m_results.value(id);
    QVERIFY(!r.ok);
    QVERIFY(r.cancelled);
    QVERIFY2(!QFile::exists(up.path), "a cancelled upload must not leave a partial file");
    // The connection is still fine.
    QVERIFY(shellRoundTrip(QStringLiteral("after-cancel-ok")));

    // Cancel a download of the same size.
    QVERIFY(QFile::copy(local, up.path));
    SftpRequest down;
    down.op = SftpOp::Download;
    down.path = up.path;
    down.localPath = m_dir.path() + QStringLiteral("/big-down.bin");
    m_progress.clear();
    const quint64 id2 = m_t->submitSftp(down);
    QTRY_VERIFY_WITH_TIMEOUT(!m_progress.isEmpty() && m_progress.last().done > 0, 20000);
    m_t->cancelSftp(id2);
    QTRY_VERIFY_WITH_TIMEOUT(m_results.contains(id2), 20000);
    QVERIFY(m_results.value(id2).cancelled || m_results.value(id2).ok);
    if (m_results.value(id2).cancelled) {
        QVERIFY(!QFile::exists(down.localPath));
        QVERIFY(!QFile::exists(down.localPath + QStringLiteral(".termx-part")));
    }
}

void TstSshIntegration::localForward()
{
    EchoServer echo;
    const int lport = freePort();
    PortForward f;
    f.type = PortForward::Type::Local;
    f.bindPort = lport;
    f.destHost = QStringLiteral("127.0.0.1");
    f.destPort = echo.serverPort();
    QVERIFY(connectTransport({f}));
    QTRY_VERIFY_WITH_TIMEOUT(!m_forwards.isEmpty(), 10000);
    QCOMPARE(m_forwards.first().state, ForwardStatus::State::Active);

    // Several connections at once, each with 2 MB.
    std::vector<std::unique_ptr<QTcpSocket>> socks;
    for (int i = 0; i < 6; ++i) {
        auto s = std::make_unique<QTcpSocket>();
        s->connectToHost(QHostAddress::LocalHost, quint16(lport));
        QVERIFY(s->waitForConnected(5000));
        socks.push_back(std::move(s));
    }
    QVector<QByteArray> payloads, got(int(socks.size()));
    for (size_t i = 0; i < socks.size(); ++i) {
        payloads << randomBytes(2 * 1024 * 1024);
        QTcpSocket *s = socks[i].get();
        connect(s, &QTcpSocket::readyRead, s, [&got, i, s] { got[int(i)] += s->readAll(); });
        s->write(payloads.last());
    }
    QTRY_VERIFY_WITH_TIMEOUT(std::equal(got.begin(), got.end(), payloads.begin(),
                                        [](const QByteArray &a, const QByteArray &b) { return a.size() == b.size(); }),
                             60000);
    QCOMPARE(got, payloads);
    QTRY_COMPARE_WITH_TIMEOUT(m_forwards.first().openConnections, 6, 5000);
    socks.clear();
    QTRY_COMPARE_WITH_TIMEOUT(m_forwards.first().openConnections, 0, 10000);
    QCOMPARE(m_forwards.first().totalConnections, quint64(6));
    QVERIFY(m_forwards.first().bytesOut >= quint64(12 * 1024 * 1024));

    // Stop the rule: the port closes.
    m_t->stopForward(m_forwards.first().id);
    QTRY_VERIFY_WITH_TIMEOUT(m_forwards.isEmpty(), 10000);
    QTcpSocket late;
    late.connectToHost(QHostAddress::LocalHost, quint16(lport));
    QVERIFY(!late.waitForConnected(1000) || !late.waitForReadyRead(500));
}

void TstSshIntegration::dynamicForwardSocks5()
{
    EchoServer echo;
    QVERIFY(connectTransport());
    PortForward d;
    d.type = PortForward::Type::Dynamic;
    d.bindPort = freePort();
    const quint64 id = m_t->startForward(d);
    QCOMPARE(waitForward(id).state, ForwardStatus::State::Active);

    QTcpSocket s;
    s.setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), quint16(d.bindPort)));
    s.connectToHost(QStringLiteral("127.0.0.1"), echo.serverPort());
    QVERIFY(s.waitForConnected(10000));
    QVERIFY(echoThrough(s, randomBytes(1024 * 1024)));

    // A destination nobody listens on: the proxy says so.
    QTcpSocket bad;
    bad.setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), quint16(d.bindPort)));
    bad.connectToHost(QStringLiteral("127.0.0.1"), quint16(freePort()));
    QVERIFY(!bad.waitForConnected(10000));
}

void TstSshIntegration::dynamicForwardSocks4a()
{
    EchoServer echo;
    QVERIFY(connectTransport());
    PortForward d;
    d.type = PortForward::Type::Dynamic;
    d.bindPort = freePort();
    const quint64 id = m_t->startForward(d);
    QCOMPARE(waitForward(id).state, ForwardStatus::State::Active);

    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, quint16(d.bindPort));
    QVERIFY(s.waitForConnected(5000));
    QByteArray req("\x04\x01", 2);
    req.append(char(echo.serverPort() >> 8)).append(char(echo.serverPort() & 0xFF));
    req.append(QByteArray("\x00\x00\x00\x01", 4)).append("user").append('\0').append("localhost").append('\0');
    req.append("early-data");   // sent before the reply: must not get lost
    s.write(req);
    QByteArray got;
    QElapsedTimer t;
    t.start();
    while (got.size() < 8 + 10 && t.elapsed() < 10000) {
        QTest::qWait(5);   // the echo server runs on this thread too
        got += s.readAll();
    }
    QVERIFY(got.size() >= 8);
    QCOMPARE(quint8(got.at(1)), quint8(0x5A));
    QCOMPARE(got.mid(8), QByteArray("early-data"));
}

void TstSshIntegration::remoteForward()
{
    EchoServer echo;
    QVERIFY(connectTransport());
    PortForward r;
    r.type = PortForward::Type::Remote;
    r.bindAddress = QStringLiteral("127.0.0.1");
    r.bindPort = freePort();
    r.destHost = QStringLiteral("127.0.0.1");
    r.destPort = echo.serverPort();
    const quint64 id = m_t->startForward(r);
    const ForwardStatus st = waitForward(id);
    QCOMPARE(st.state, ForwardStatus::State::Active);
    QCOMPARE(st.boundPort, r.bindPort);

    for (int round = 0; round < 3; ++round) {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, quint16(r.bindPort));
        QVERIFY(s.waitForConnected(5000));
        QVERIFY(echoThrough(s, randomBytes(512 * 1024)));
    }

    // Port 0: the server picks one and we learn which.
    PortForward any = r;
    any.bindPort = 0;
    const quint64 id2 = m_t->startForward(any);
    const ForwardStatus st2 = waitForward(id2);
    QCOMPARE(st2.state, ForwardStatus::State::Active);
    QVERIFY(st2.boundPort > 0);
    QTcpSocket s2;
    s2.connectToHost(QHostAddress::LocalHost, quint16(st2.boundPort));
    QVERIFY(s2.waitForConnected(5000));
    QVERIFY(echoThrough(s2, "ping"));

    // Destination down: the connection is closed, the rule stays up.
    PortForward dead = r;
    dead.bindPort = freePort();
    dead.destPort = freePort();
    const quint64 id3 = m_t->startForward(dead);
    QCOMPARE(waitForward(id3).state, ForwardStatus::State::Active);
    QTcpSocket s3;
    s3.connectToHost(QHostAddress::LocalHost, quint16(dead.bindPort));
    QVERIFY(s3.waitForConnected(5000));
    QTRY_VERIFY_WITH_TIMEOUT(s3.state() == QAbstractSocket::UnconnectedState, 20000);

    m_t->stopForward(id);
    QTRY_VERIFY_WITH_TIMEOUT(std::none_of(m_forwards.begin(), m_forwards.end(), [&](const ForwardStatus &x) { return x.id == id; }), 10000);
    QVERIFY(shellRoundTrip(QStringLiteral("remote-done")));
}

void TstSshIntegration::forwardErrors()
{
    QVERIFY(connectTransport());
    // Port already taken here.
    QTcpServer busy;
    QVERIFY(busy.listen(QHostAddress::LocalHost, 0));
    PortForward l;
    l.type = PortForward::Type::Local;
    l.bindPort = busy.serverPort();
    l.destHost = QStringLiteral("127.0.0.1");
    l.destPort = 9;
    const quint64 id = m_t->startForward(l);
    const ForwardStatus st = waitForward(id);
    QCOMPARE(st.state, ForwardStatus::State::Failed);
    QVERIFY(!st.error.isEmpty());

    // Local forward to a closed port: the client is disconnected, rule stays up.
    PortForward l2;
    l2.type = PortForward::Type::Local;
    l2.bindPort = freePort();
    l2.destHost = QStringLiteral("127.0.0.1");
    l2.destPort = freePort();
    const quint64 id2 = m_t->startForward(l2);
    QCOMPARE(waitForward(id2).state, ForwardStatus::State::Active);
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, quint16(l2.bindPort));
    QVERIFY(s.waitForConnected(5000));
    QTRY_VERIFY_WITH_TIMEOUT(s.state() == QAbstractSocket::UnconnectedState, 20000);
    QVERIFY(shellRoundTrip(QStringLiteral("errors-ok")));
}

// Shell output, SFTP transfers and tunnels at the same time over one
// connection: exercises the libssh2 "same call after EAGAIN" rule.
void TstSshIntegration::everythingAtOnce()
{
    EchoServer echo;
    PortForward f;
    f.type = PortForward::Type::Local;
    f.bindPort = freePort();
    f.destHost = QStringLiteral("127.0.0.1");
    f.destPort = echo.serverPort();
    QVERIFY(connectTransport({f}));
    QTRY_VERIFY_WITH_TIMEOUT(!m_forwards.isEmpty() && m_forwards.first().state == ForwardStatus::State::Active, 10000);

    const QByteArray payload = randomBytes(24 * 1024 * 1024);
    const QString local = m_dir.path() + QStringLiteral("/mix.bin");
    {
        QFile file(local);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(payload);
    }
    SftpRequest up;
    up.op = SftpOp::Upload;
    up.localPath = local;
    up.path = m_dir.path() + QStringLiteral("/mix-remote.bin");
    up.overwrite = true;
    const quint64 upId = m_t->submitSftp(up);

    std::vector<std::unique_ptr<QTcpSocket>> socks;
    QVector<QByteArray> sent, got(4);
    for (int i = 0; i < 4; ++i) {
        auto s = std::make_unique<QTcpSocket>();
        s->connectToHost(QHostAddress::LocalHost, quint16(f.bindPort));
        QVERIFY(s->waitForConnected(5000));
        QTcpSocket *raw = s.get();
        connect(raw, &QTcpSocket::readyRead, raw, [&got, i, raw] { got[i] += raw->readAll(); });
        sent << randomBytes(3 * 1024 * 1024);
        raw->write(sent.last());
        socks.push_back(std::move(s));
    }
    m_out.clear();
    m_t->write("seq 1 100000 | md5sum; echo mix''done\n");

    QTRY_VERIFY_WITH_TIMEOUT(m_results.contains(upId), 120000);
    QVERIFY2(m_results.value(upId).ok, qPrintable(m_results.value(upId).error));
    QTRY_VERIFY_WITH_TIMEOUT(m_out.contains("mixdone"), 60000);
    QTRY_VERIFY_WITH_TIMEOUT(std::equal(got.begin(), got.end(), sent.begin(),
                                        [](const QByteArray &a, const QByteArray &b) { return a.size() >= b.size(); }),
                             120000);
    QCOMPARE(got, sent);

    // The uploaded file is intact; download it while the shell keeps talking.
    SftpRequest down;
    down.op = SftpOp::Download;
    down.path = up.path;
    down.localPath = m_dir.path() + QStringLiteral("/mix-back.bin");
    down.overwrite = true;
    const quint64 downId = m_t->submitSftp(down);
    QVERIFY(shellRoundTrip(QStringLiteral("during-download")));
    QTRY_VERIFY_WITH_TIMEOUT(m_results.contains(downId), 120000);
    QVERIFY2(m_results.value(downId).ok, qPrintable(m_results.value(downId).error));
    QFile back(down.localPath);
    QVERIFY(back.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(back.readAll(), QCryptographicHash::Sha256),
             QCryptographicHash::hash(payload, QCryptographicHash::Sha256));
}

void TstSshIntegration::requestsAfterDisconnect()
{
    QVERIFY(connectTransport());
    SftpRequest big;
    big.op = SftpOp::List;
    big.path = QStringLiteral("/");
    const quint64 queued = m_t->submitSftp(big);
    m_t->close();
    QTRY_VERIFY_WITH_TIMEOUT(m_results.contains(queued), 10000);
    // After the worker is gone every request still gets an answer.
    QTRY_VERIFY_WITH_TIMEOUT(Transport::shutdownAll(100), 10000);
    const quint64 late = m_t->submitSftp(big);
    QTRY_VERIFY_WITH_TIMEOUT(m_results.contains(late), 5000);
    QVERIFY(!m_results.value(late).ok);
    QCOMPARE(m_t->startForward(PortForward{}), quint64(0));
}

QTEST_MAIN(TstSshIntegration)
#include "tst_sshintegration.moc"

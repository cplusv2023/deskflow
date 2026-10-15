// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "CoreClipboard.h"
#ifndef DESKFLOW_CLIPBOARD_FILES
namespace deskflow::clipboard {
struct PeerLink::Impl
{
};
PeerLink::PeerLink(std::unique_ptr<Impl> impl) : m_impl(std::move(impl))
{
}
PeerLink::~PeerLink() = default;
unsigned short PeerLink::port() const
{
  return 0;
}
std::unique_ptr<PeerLink> PeerLink::listen(IStream *)
{
  return {};
}
std::unique_ptr<PeerLink> PeerLink::connect(IStream *, unsigned short)
{
  return {};
}
} // namespace deskflow::clipboard
#else
#include "LocalFileRpc.h"
#include "common/Settings.h"
#include "deskflow/ipc/CoreIpc.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QSslCertificate>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <future>
#include <map>
#include <openssl/crypto.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace deskflow::clipboard {
namespace {
constexpr qsizetype frameLimit = 1024 * 1024;
constexpr qsizetype queueLimit = 2 * 1024 * 1024 + 64 * 1024;
constexpr qsizetype formatLimit = 16 * 1024 * 1024;
constexpr qsizetype chunkLimit = 256 * 1024;
std::atomic_bool reserved{false}, active{false};
std::mutex legacyClipboardMutex;
void setActive(bool value)
{
  const std::lock_guard lock(legacyClipboardMutex);
  active = value;
}
QString backendName()
{
#ifdef Q_OS_MACOS
  return "AppKit / macFUSE FSKit";
#else
  if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland")
    return "GNOME Wayland via XWayland / FUSE3";
  return "X11 / FUSE3";
#endif
}
const QString origin = QUuid::createUuid().toString(QUuid::WithoutBraces);
std::string str(const QString &s)
{
  return s.toUtf8().toStdString();
}
QString qs(const std::string &s)
{
  return QString::fromUtf8(s);
}
std::uint64_t integer(const std::string &s)
{
  std::uint64_t n = 0;
  auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (s.empty() || ec != std::errc{} || p != s.data() + s.size())
    throw std::runtime_error("invalid integer");
  return n;
}
void status(const QString &s)
{
  qInfo().noquote() << "clipboard:" << s;
  ipcSendToClient("clipboardStatus", s);
}
QString helper(const QString &name)
{
  return QCoreApplication::applicationDirPath() + '/' + name;
}
// Bounded parser shared by the network connection and local FUSE adapters.
bool takeMessage(QByteArray &buffer, RpcMessage &out)
{
  if (buffer.size() < 4)
    return false;
  auto size = qFromBigEndian<quint32>(buffer.constData());
  if (size < 8 || size > frameLimit)
    throw std::runtime_error("invalid frame size");
  if (buffer.size() < size + 4)
    return false;
  out = decodeRpcBody({reinterpret_cast<const uint8_t *>(buffer.constData() + 4), size});
  buffer.remove(0, size + 4);
  return true;
}
bool writeMessage(QIODevice *device, const RpcMessage &msg)
{
  const auto data = encodeRpcFrame(msg);
  auto queued = device->bytesToWrite();
  if (auto *tls = qobject_cast<QSslSocket *>(device))
    queued += tls->encryptedBytesToWrite();
  if (queued + static_cast<qint64>(data.size()) > queueLimit)
    return false;
  return device->write(reinterpret_cast<const char *>(data.data()), data.size()) == static_cast<qint64>(data.size());
}
class Listener : public QTcpServer
{
public:
  using QTcpServer::QTcpServer;
  std::function<void(qintptr)> accept;
  void incomingConnection(qintptr fd) override
  {
    accept(fd);
  }
};

class Session : public QObject
{
public:
  Session(ClipboardBinding binding, bool server, QString certificate)
      : m_binding(std::move(binding)),
        m_server(server),
        m_certificate(std::move(certificate)),
        m_dir(QDir::tempPath() + "/dfcb-XXXXXX")
  {
    m_clock.start();
    // Never recursively remove a path which may still be a mounted filesystem.
    m_dir.setAutoRemove(false);
    LocalFileSource::Options opts;
    auto key = QByteArray(reinterpret_cast<const char *>(m_binding.secret.data()), 32);
    opts.sessionEpoch =
        str(QString::fromLatin1(
            QCryptographicHash::hash(key + (server ? "server" : "client"), QCryptographicHash::Sha256).toHex()
        ));
    opts.maxEntriesPerOffer = 1024;
    opts.maxManifestBytes = 512 * 1024;
    opts.maxOffers = 16;
    m_source = std::make_unique<LocalFileSource>(opts);
    m_tick.setInterval(500);
    connect(&m_tick, &QTimer::timeout, this, [this] { tick(); });
    m_tick.start();
    m_deadline = now() + 15000;
  }
  ~Session() override
  {
    shutdown();
  }
  unsigned short start(unsigned short port)
  {
    if (!m_dir.isValid())
      throw std::runtime_error("cannot create private runtime directory");
    ::chmod(m_dir.path().toUtf8().constData(), 0700);
    if (m_server) {
      m_listener = new Listener(this);
      m_listener->setMaxPendingConnections(1);
      m_listener->accept = [this](qintptr fd) {
        if (m_socket) {
          ::close(static_cast<int>(fd));
          return;
        }
        setupSocket(fd);
      };
      // Ephemeral port is advertised only on the authenticated, accepted main connection.
      if (!m_listener->listen(QHostAddress::Any, 0))
        throw std::runtime_error("cannot listen for clipboard TLS");
      return m_listener->serverPort();
    }
    setupSocket(-1);
    if (m_stopped)
      return 0;
    m_socket->connectToHostEncrypted(qs(m_binding.peerAddress), port);
    return port;
  }
  void shutdown()
  {
    if (m_stopped)
      return;
    m_stopped = true;
    m_tick.stop();
    if (m_listener)
      m_listener->close();
    if (m_socket)
      m_socket->abort();
    // Closing bridges wakes FUSE requests before waiting for helpers to exit.
    for (auto &[id, mount] : m_mounts) {
      if (mount->socket)
        mount->socket->abort();
      mount->listener->close();
    }
    m_source->disconnect();
    const auto processes = findChildren<QProcess *>();
    for (auto *p : processes)
      if (p->state() != QProcess::NotRunning)
        p->terminate();
    for (auto *p : processes) {
      if (p->state() != QProcess::NotRunning && !p->waitForFinished(1500)) {
        p->kill();
        p->waitForFinished(1000);
      }
    }
    // Unmount only our known paths. rmdir cannot traverse or delete a live mount.
    for (auto &[id, mount] : m_mounts) {
      QProcess unmount;
#ifdef Q_OS_MACOS
      unmount.start("/sbin/umount", {mount->path});
#else
      unmount.start("fusermount3", {"-u", mount->path});
#endif
      if (!unmount.waitForFinished(1500)) {
        unmount.kill();
        unmount.waitForFinished(500);
      }
      if (::rmdir(mount->path.toUtf8().constData()) != 0)
        qWarning().noquote() << "clipboard: mount directory retained; inspect/unmount" << mount->path;
      QFile::remove(mount->socketPath);
    }
    // Only bounded format files and socket names generated by this process are removed.
    for (const auto &path : m_formatFiles)
      QFile::remove(path);
    ::rmdir(m_dir.path().toUtf8().constData());
    setActive(false);
    status("Disconnected; legacy clipboard restored");
  }

private:
  struct Mount
  {
    OfferInfo offer;
    QString path, socketPath;
    QLocalServer *listener = nullptr;
    QPointer<QLocalSocket> socket;
    QProcess *process = nullptr;
    QByteArray input, output;
    std::string pendingOp, pendingId;
    std::uint64_t lastId = 0, token = 0, revision = 0;
    qint64 deadline = 0, startupDeadline = 0;
    bool ready = false;
  };
  qint64 now() const
  {
    return m_clock.elapsed();
  }
  void fail(const QString &reason)
  {
    if (m_stopped)
      return;
    shutdown();
    status("Unavailable: " + reason);
    QThread::currentThread()->quit();
  }
  void networkFailure(const QString &reason)
  {
    if (m_stopped)
      return;
    if (m_server && !m_bound) {
      // An unauthenticated connection must not consume the main session's binding.
      auto *rejected = m_socket;
      m_socket = nullptr;
      if (rejected) {
        rejected->disconnect(this);
        rejected->abort();
        rejected->deleteLater();
      }
      m_input.clear();
      status("Waiting for authenticated clipboard connection (rejected an unbound connection)");
      return;
    }
    fail(reason);
  }
  bool send(RpcMessage message)
  {
    if (m_stopped || !m_socket || !m_socket->isEncrypted())
      return false;
    try {
      if (writeMessage(m_socket, message))
        return true;
    } catch (const std::exception &) {
    }
    fail("data queue or frame limit exceeded");
    return false;
  }
  bool fingerprintMatches() const
  {
    const auto digest = m_socket->peerCertificate().digest(QCryptographicHash::Sha256);
    return digest.size() == 32 && CRYPTO_memcmp(digest.constData(), m_binding.peerFingerprint.data(), 32) == 0;
  }
  void setupSocket(qintptr fd)
  {
    m_socket = new QSslSocket(this);
    m_socket->setReadBufferSize(frameLimit + 4);
    QFile pem(m_certificate);
    if (!pem.open(QIODevice::ReadOnly)) {
      fail("cannot read TLS certificate");
      return;
    }
    const auto bytes = pem.readAll();
    QSslKey key(bytes, QSsl::Rsa);
    if (key.isNull())
      key = QSslKey(bytes, QSsl::Ec);
    m_socket->setLocalCertificate(QSslCertificate(bytes));
    m_socket->setPrivateKey(key);
    m_socket->setProtocol(QSsl::TlsV1_2OrLater);
    m_socket->setPeerVerifyMode(QSslSocket::VerifyPeer);
    connect(m_socket, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
      // The main connection already verified this exact certificate. No global trust exception.
      if (fingerprintMatches())
        m_socket->ignoreSslErrors(errors);
    });
    connect(m_socket, &QSslSocket::encrypted, this, [this] {
      if (!fingerprintMatches()) {
        networkFailure("data channel peer differs from main connection");
        return;
      }
      send(
          {{"bind", "1", QByteArray(reinterpret_cast<const char *>(m_binding.secret.data()), 32).toHex().toStdString()},
           {}}
      );
    });
    connect(m_socket, &QSslSocket::readyRead, this, [this] { drainInput(); });
    connect(m_socket, &QSslSocket::disconnected, this, [this] { networkFailure("data connection closed"); });
    connect(m_socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
      networkFailure("TLS connection failed");
    });
    if (fd >= 0) {
      if (!m_socket->setSocketDescriptor(fd)) {
        ::close(static_cast<int>(fd));
        fail("socket setup failed");
        return;
      }
      m_socket->startServerEncryption();
    }
  }
  void drainInput()
  {
    try {
      // After the TLS handshake, wait for a native baseline before processing
      // selections. Keep data in the bounded socket/frame buffers meanwhile.
      while (!m_stopped && m_socket && (!m_enabled || m_nativeReady)) {
        RpcMessage msg;
        if (takeMessage(m_input, msg)) {
          handle(std::move(msg));
        } else if (m_socket->bytesAvailable()) {
          m_input += m_socket->read(frameLimit + 4 - m_input.size());
        } else {
          break;
        }
      }
    } catch (const std::exception &e) {
      networkFailure("invalid peer message: " + QString::fromUtf8(e.what()));
    }
  }
  void handle(RpcMessage msg)
  {
    auto &f = msg.fields;
    if (f.empty())
      throw std::runtime_error("empty message");
    const auto op = f[0];
    if (!m_bound) {
      const auto expected = QByteArray(reinterpret_cast<const char *>(m_binding.secret.data()), 32).toHex();
      if (f.size() != 3 || op != "bind" || f[1] != "1" || !msg.bytes.empty() ||
          f[2].size() != static_cast<size_t>(expected.size()) ||
          CRYPTO_memcmp(f[2].data(), expected.constData(), expected.size()) != 0)
        throw std::runtime_error("main-session binding rejected");
      m_bound = true;
      OPENSSL_cleanse(m_binding.secret.data(), m_binding.secret.size());
      if (m_listener)
        m_listener->close();
      send({{"ready"}, {}});
      return;
    }
    if (!m_enabled) {
      if (f != std::vector<std::string>{"ready"} || !msg.bytes.empty())
        throw std::runtime_error("expected ready");
      m_enabled = true;
      setActive(true);
      m_deadline = now() + 15000;
      startWatcher();
      return;
    }
    if (op == "intent" && m_server && f.size() == 2 && msg.bytes.empty()) {
      auto token = integer(f[1]);
      if (!token || token <= m_remoteToken)
        throw std::runtime_error("replayed intent");
      m_remoteToken = token;
      select("client", token);
    } else if (op == "select" && !m_server && f.size() == 4 && msg.bytes.empty()) {
      auto revision = integer(f[1]), token = integer(f[3]);
      if (revision <= m_revision || (f[2] != "server" && f[2] != "client"))
        throw std::runtime_error("invalid selection");
      if (m_pendingPublisher) {
        m_pendingPublisher->kill();
        m_pendingPublisher = nullptr;
      }
      m_revision = revision;
      m_selectedSource = f[2];
      m_selectedToken = token;
      if (f[2] == "server") {
        m_outgoing.clear();
        m_outToken = 0;
        m_outDeadline = 0;
      }
      m_incoming.clear();
      m_incomingMime.clear();
      if (f[2] == "client")
        m_acknowledgedToken = token;
    } else if (op == "files" && f.size() > 2 && msg.bytes.empty()) {
      const auto token = integer(f[1]);
      if (!remoteSelected(token))
        return;
      f.erase(f.begin(), f.begin() + 2);
      mountOffer(parseOfferMessage(msg), token);
    } else if (op == "request" && f.size() >= 4) {
      const auto channel = f[1];
      if (channel.size() > 64 || !msg.bytes.empty())
        throw std::runtime_error("invalid request");
      f.erase(f.begin(), f.begin() + 2);
      if (!m_remoteRequests.contains(channel) && m_remoteRequests.size() >= 8)
        throw std::runtime_error("remote bridge budget exceeded");
      const auto requestId = integer(f[1]);
      if (requestId <= m_remoteRequests[channel])
        throw std::runtime_error("replayed file request");
      m_remoteRequests[channel] = requestId;
      // File IO is on this dedicated thread, never the keyboard event loop or socket multiplexer.
      auto reply = dispatchFileRequest(*m_source, msg);
      qDebug().noquote() << "clipboard RPC" << qs(f[0]) << "requestId=" << qs(f[1]) << "status=" << qs(reply.fields[2])
                         << "returnedBytes=" << reply.bytes.size();
      if (f[0] == "read" && reply.fields.size() == 13) {
        const auto &r = reply.fields;
        qDebug().noquote() << "clipboard range epoch=" << qs(r[3]) << "offerId=" << qs(r[4]) << "fileId=" << qs(r[5])
                           << "version=" << qs(r[6]) << "leaseId=" << qs(r[7]) << "handleId=" << qs(r[8])
                           << "requestId=" << qs(r[9]) << "offset=" << qs(r[10]) << "length=" << qs(r[11])
                           << "eof=" << qs(r[12]);
      }
      if (reply.fields.size() > 2 && reply.fields[2] != "OK")
        status("File request: " + qs(reply.fields[2]));
      reply.fields.insert(reply.fields.begin(), {"reply", channel});
      send(std::move(reply));
    } else if (op == "reply" && f.size() >= 5) {
      auto found = m_mounts.find(f[1]);
      if (found == m_mounts.end())
        throw std::runtime_error("unknown bridge");
      auto &mount = *found->second;
      f.erase(f.begin(), f.begin() + 2);
      if (!mount.socket || f[0] != mount.pendingOp || f[1] != mount.pendingId)
        throw std::runtime_error("uncorrelated response");
      mount.pendingOp.clear();
      mount.pendingId.clear();
      mount.deadline = 0;
      if (!writeMessage(mount.socket, msg))
        throw std::runtime_error("local queue exhausted");
    } else if (op == "format-start" && f.size() == 4 && msg.bytes.empty()) {
      auto token = integer(f[1]), size = integer(f[3]);
      if (!remoteSelected(token))
        return;
      if (!validMime(f[2]) || size > formatLimit || m_incoming.contains(f[2]) || !m_incomingMime.empty())
        throw std::runtime_error("invalid format start");
      qsizetype total = size;
      for (auto &[mime, data] : m_incoming)
        total += data.size();
      if (total > formatLimit)
        throw std::runtime_error("format budget exceeded");
      m_incomingMime = f[2];
      m_incomingSize = size;
      m_incoming[f[2]] = {};
      send({{"format-ack", f[1], f[2], "0"}, {}});
    } else if (op == "format-chunk" && f.size() == 4) {
      if (!remoteSelected(integer(f[1])))
        return;
      if (f[2] != m_incomingMime || msg.bytes.size() > chunkLimit)
        throw std::runtime_error("invalid format chunk");
      auto &data = m_incoming.at(f[2]);
      if (integer(f[3]) != static_cast<uint64_t>(data.size()) || data.size() + msg.bytes.size() > m_incomingSize)
        throw std::runtime_error("format offset mismatch");
      data.append(reinterpret_cast<const char *>(msg.bytes.data()), msg.bytes.size());
      send({{"format-ack", f[1], f[2], std::to_string(data.size())}, {}});
      if (data.size() == m_incomingSize)
        m_incomingMime.clear();
    } else if (op == "format-ack" && f.size() == 4 && msg.bytes.empty()) {
      if (integer(f[1]) == m_outToken)
        advanceFormats(f[2], integer(f[3]));
    } else if (op == "formats-end" && f.size() == 2 && msg.bytes.empty()) {
      if (remoteSelected(integer(f[1])) && m_incomingMime.empty() && !m_incoming.empty())
        publishFormats(integer(f[1]));
    } else
      throw std::runtime_error("unsupported operation");
  }
  static bool validMime(const std::string &mime)
  {
    return mime == "text/plain" || mime == "text/html" || mime == "image/png";
  }
  bool remoteSelected(uint64_t token) const
  {
    return !m_stopped && token == m_selectedToken && m_selectedSource == (m_server ? "client" : "server") &&
           (m_server || m_acknowledgedToken == m_localToken);
  }
  void select(const std::string &source, uint64_t token)
  {
    if (m_pendingPublisher) {
      m_pendingPublisher->kill();
      m_pendingPublisher = nullptr;
    }
    ++m_revision;
    m_selectedSource = source;
    m_selectedToken = token;
    if (source == "client") {
      m_outgoing.clear();
      m_outToken = 0;
      m_outDeadline = 0;
    }
    m_incoming.clear();
    m_incomingMime.clear();
    send({{"select", std::to_string(m_revision), source, std::to_string(token)}, {}});
  }
  QProcess *process(const QString &name, const QStringList &args)
  {
    auto *p = new QProcess(this);
    // Arch's signal thread blocks termination signals in core threads. exec
    // preserves that mask, so helpers must start unblocked for QProcess teardown
    // and ParentWatch to reach their SIGTERM/FUSE handlers.
    p->setChildProcessModifier([] {
      sigset_t mask;
      sigemptyset(&mask);
      sigprocmask(SIG_SETMASK, &mask, nullptr);
    });
    p->setProgram(helper(name));
    p->setArguments(args + QStringList{"--parent-pid", QString::number(QCoreApplication::applicationPid())});
    connect(p, &QProcess::readyReadStandardError, this, [p] { p->readAllStandardError(); });
    return p;
  }
  void startWatcher()
  {
    QStringList args{"watch", "--origin", origin};
#ifndef Q_OS_MACOS
    args << "--capture" << "--timeout-ms" << "10000";
#endif
    if (!m_server)
      args << "--skip-initial";
    m_watcher = process("deskflow-clipboard-probe", args);
    connect(m_watcher, &QProcess::readyReadStandardOutput, this, [this] {
      m_watchInput += m_watcher->readAllStandardOutput();
      // Small formats are captured as one bounded native generation. The local
      // JSONL pipe carries base64, while the network retains 256 KiB frames.
      if (m_watchInput.size() > 2 * formatLimit) {
        fail("native clipboard snapshot too large");
        return;
      }
      while (m_watchInput.contains('\n')) {
        auto pos = m_watchInput.indexOf('\n');
        auto object = QJsonDocument::fromJson(m_watchInput.left(pos)).object();
        m_watchInput.remove(0, pos + 1);
        const auto event = object.value("event").toString();
        if (event == "watch_initialized") {
          m_nativeReady = true;
          status("Ready: " + backendName() + "; detectable files, text, HTML, PNG");
          continue;
        }
        if (event == "selfChange" || event == "self_offer_ignored") {
          if (object.contains("changeCount"))
            m_native = object;
          continue;
        }
        if (event == "owner_changed") {
          m_native = object;
          continue;
        }
        if (event == "announcement_skipped") {
          status("Native clipboard capture: " + object.value("status").toString() + "; copy again to retry");
          continue;
        }
        if (event != "clipboardChanged" && event != "clipboard_snapshot")
          continue;
        m_native = object;
        observed(object);
      }
      if (m_nativeReady)
        drainInput();
    });
    connect(m_watcher, &QProcess::finished, this, [this] {
      if (!m_stopped)
        fail("native clipboard watcher stopped");
    });
    connect(m_watcher, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
      if (!m_stopped)
        fail("native clipboard helper unavailable");
    });
    m_watcher->start();
  }
  void observed(const QJsonObject &object)
  {
    if (m_pendingPublisher) {
      m_pendingPublisher->kill();
      m_pendingPublisher = nullptr;
    }
    ++m_localToken;
    m_outgoing.clear();
    m_outMime.clear();
    m_outToken = 0;
    if (m_server)
      select("server", m_localToken);
    else
      send({{"intent", std::to_string(m_localToken)}, {}});
    // Cancel obsolete native reads before starting the next snapshot.
    if (m_reader) {
      m_reader->disconnect(this);
      m_reader->kill();
      m_reader->deleteLater();
      m_reader = nullptr;
    }
    m_capture.clear();
    m_captureTargets.clear();
#ifndef Q_OS_MACOS
    if (object.contains("files")) {
      std::vector<std::filesystem::path> paths;
      for (const auto &path : object.value("files").toArray())
        paths.emplace_back(str(path.toString()));
      announceFiles(paths, m_localToken);
      return;
    }
    qsizetype total = 0;
    for (const auto &format : object.value("formats").toArray()) {
      const auto item = format.toObject();
      const auto mime = str(item.value("mime").toString());
      const auto decoded = QByteArray::fromBase64Encoding(
          item.value("bytesBase64").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors
      );
      if (!validMime(mime) || !decoded || m_capture.contains(mime) || decoded.decoded.size() > formatLimit - total) {
        fail("invalid native clipboard snapshot");
        return;
      }
      total += decoded.decoded.size();
      m_capture.emplace(mime, decoded.decoded);
    }
    m_outgoing = std::move(m_capture);
    m_outToken = m_localToken;
    nextFormat();
#else
    QStringList types;
    for (const auto &item : object.value("items").toArray())
      for (const auto &type : item.toObject().value("types").toArray())
        types << type.toString();
    if (types.contains("public.file-url")) {
      captureFiles(m_localToken);
      return;
    }
    if (types.contains("public.utf8-plain-text") || types.contains("public.utf16-external-plain-text") ||
        types.contains("public.rtf"))
      m_captureTargets << "text/plain";
    if (types.contains("public.html") || types.contains("public.rtf"))
      m_captureTargets << "text/html";
    if (types.contains("public.png") || types.contains("public.tiff"))
      m_captureTargets << "image/png";
    captureNext(m_localToken);
#endif
  }
  void
  readNative(QStringList args, uint64_t token, std::function<void(QByteArray)> done, std::function<void()> skipped = {})
  {
    args << publicationGuard();
#ifdef Q_OS_MACOS
    args << "--origin" << origin;
#endif
    auto *p = process("deskflow-clipboard-probe", args);
    m_reader = p;
    auto bytes = std::make_shared<QByteArray>();
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p, bytes] {
      bytes->append(p->readAllStandardOutput());
      if (bytes->size() > formatLimit) {
        p->kill();
        bytes->clear();
      }
    });
    connect(p, &QProcess::finished, this, [this, p, bytes, token, done, skipped](int code, QProcess::ExitStatus exit) {
      if (m_reader == p)
        m_reader = nullptr;
      if (!m_stopped && token == m_localToken && code == 0 && exit == QProcess::NormalExit)
        done(std::move(*bytes));
      else if (!m_stopped && token == m_localToken) {
        status("Native clipboard capture failed; copy again to retry");
        if (skipped)
          skipped();
      }
      p->deleteLater();
    });
    QTimer::singleShot(10000, p, [p] {
      if (p->state() != QProcess::NotRunning)
        p->kill();
    });
    p->start();
  }
  void captureFiles(uint64_t token)
  {
#ifdef Q_OS_MACOS
    QStringList args{"read", "--target", "text/uri-list"};
#else
    QStringList args{"capture-files"};
#endif
    readNative(args, token, [this, token](QByteArray bytes) {
      std::vector<std::filesystem::path> paths;
#ifdef Q_OS_MACOS
      for (auto line : bytes.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
          continue;
        QUrl url = QUrl::fromEncoded(line, QUrl::StrictMode);
        if (!url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost"))
          return;
        paths.emplace_back(str(url.toLocalFile()));
      }
#else
      auto doc = QJsonDocument::fromJson(bytes);
      if (!doc.isArray()) return;
      for (const auto &path : doc.array()) paths.emplace_back(str(path.toString()));
#endif
      announceFiles(paths, token);
    });
  }
  void announceFiles(const std::vector<std::filesystem::path> &paths, uint64_t token)
  {
    // A transient native owner notification must never re-export our own projection.
    for (const auto &path : paths) {
      const auto localPath = qs(path.lexically_normal().string());
      if (localPath == m_dir.path() || localPath.startsWith(m_dir.path() + '/'))
        return;
    }
    // No file body is read: only fixed, bounded metadata and source identity.
    const auto result = m_source->createOffer(paths);
    if (result.status != Status::Ok) {
      status("File capture: " + qs(std::string(statusName(result.status))));
      return;
    }
    auto msg = offerMessage(result.offer);
    msg.fields.insert(msg.fields.begin(), {"files", std::to_string(token)});
    send(std::move(msg));
    status("File references announced; content is read only when pasted");
  }
  void captureNext(uint64_t token)
  {
    if (m_captureTargets.isEmpty()) {
      m_outgoing = std::move(m_capture);
      m_outToken = token;
      nextFormat();
      return;
    }
    auto mime = m_captureTargets.takeFirst();
    QString target = mime;
#ifndef Q_OS_MACOS
    if (mime == "text/plain") {
      const auto targets = m_native.value("targets").toArray();
      if (targets.contains("UTF8_STRING"))
        target = "UTF8_STRING";
      else if (targets.contains("text/plain;charset=utf-8"))
        target = "text/plain;charset=utf-8";
    }
#endif
    readNative(
        {"read", "--target", target}, token,
        [this, token, mime](QByteArray data) {
          qsizetype total = data.size();
          for (const auto &[type, bytes] : m_capture)
            total += bytes.size();
          if (total > formatLimit) {
            status("Clipboard representations exceed 16 MiB");
            return;
          }
          m_capture[str(mime)] = std::move(data);
          captureNext(token);
        },
        [this, token] { captureNext(token); }
    );
  }
  void nextFormat()
  {
    if (m_outgoing.empty()) {
      if (m_outToken)
        send({{"formats-end", std::to_string(m_outToken)}, {}});
      m_outMime.clear();
      m_outDeadline = 0;
      return;
    }
    m_outMime = m_outgoing.begin()->first;
    m_outOffset = 0;
    m_outDeadline = now() + 10000;
    send(
        {{"format-start", std::to_string(m_outToken), m_outMime, std::to_string(m_outgoing.begin()->second.size())}, {}}
    );
  }
  void advanceFormats(const std::string &mime, uint64_t offset)
  {
    if (mime != m_outMime || m_outgoing.empty() || offset != m_outOffset)
      throw std::runtime_error("invalid format acknowledgement");
    const auto &data = m_outgoing.at(mime);
    if (offset == static_cast<uint64_t>(data.size())) {
      // Empty formats need one empty chunk so the receiver observes their completion.
      if (data.isEmpty() && !m_emptySent) {
        m_emptySent = true;
        send({{"format-chunk", std::to_string(m_outToken), mime, "0"}, {}});
        return;
      }
      m_emptySent = false;
      m_outgoing.erase(mime);
      nextFormat();
      return;
    }
    auto count = std::min<uint64_t>(chunkLimit, data.size() - offset);
    RpcMessage chunk{{"format-chunk", std::to_string(m_outToken), mime, std::to_string(offset)}, {}};
    chunk.bytes.assign(data.constData() + offset, data.constData() + offset + count);
    m_outOffset += count;
    m_outDeadline = now() + 10000;
    send(std::move(chunk));
  }
  QStringList publicationGuard() const
  {
#ifdef Q_OS_MACOS
    return {"--expected-change-count", QString::number(m_native.value("changeCount").toInteger(-1))};
#else
    return {
        "--expected-owner", QString::number(m_native.value("owner").toInteger()), "--expected-timestamp",
        QString::number(m_native.value("selectionTimestamp").toInteger())
    };
#endif
  }
  void publish(QStringList args, uint64_t token, QStringList files = {})
  {
    if (!remoteSelected(token)) {
      for (const auto &path : files) {
        QFile::remove(path);
        m_formatFiles.removeAll(path);
      }
      return;
    }
    // Keep the old owner alive until the guarded replacement actually claims ownership.
    if (m_pendingPublisher) {
      m_pendingPublisher->kill();
      m_pendingPublisher = nullptr;
    }
    args << "--origin" << origin;
    args << publicationGuard();
    auto *p = process("deskflow-clipboard-probe", args);
    m_pendingPublisher = p;
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] {
      const auto bytes = p->readAllStandardOutput();
      if (bytes.contains("ready") || bytes.contains("published")) {
        if (m_publisher && m_publisher != p)
          m_publisher->terminate();
        m_publisher = p;
        if (m_pendingPublisher == p)
          m_pendingPublisher = nullptr;
        status("Remote clipboard available for paste");
      }
    });
    connect(p, &QProcess::finished, this, [this, p, files](int code, QProcess::ExitStatus) {
      for (const auto &path : files) {
        QFile::remove(path);
        m_formatFiles.removeAll(path);
      }
      if (m_pendingPublisher == p)
        m_pendingPublisher = nullptr;
      if (m_publisher == p)
        m_publisher = nullptr;
      if (code && !m_stopped)
        status("Clipboard publication failed or a newer local copy won");
      p->deleteLater();
    });
    p->start();
  }
  void publishFormats(uint64_t token)
  {
#ifdef Q_OS_MACOS
    QStringList args{"publish-data"};
#else
    QStringList args{"publish-formats"};
#endif
    // At most an active and a replacement representation set are retained.
    if (m_pendingPublisher) {
      status("Clipboard publisher busy; copy again to retry");
      m_incoming.clear();
      return;
    }
    QStringList files;
    for (auto &[mime, bytes] : m_incoming) {
      auto path = m_dir.path() + '/' + QUuid::createUuid().toString(QUuid::WithoutBraces);
      m_formatFiles << path;
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size()) {
        fail("cannot stage bounded clipboard representation");
        return;
      }
      file.close();
      files << path;
#ifdef Q_OS_MACOS
      args << "--target" << qs(mime) << "--input" << path;
#else
      const auto nativeMime = mime == "text/plain" ? QString("text/plain;charset=utf-8") : qs(mime);
      args << "--format" << nativeMime + '=' + path;
#endif
    }
    m_incoming.clear();
    publish(args, token, files);
  }
  void mountOffer(OfferInfo offer, uint64_t token)
  {
    if (m_mounts.size() >= 8) {
      status("Retained mount limit (8); reconnect after finishing older copies");
      return;
    }
    if (offer.files.size() > 1024 || offer.roots.empty() || offer.roots.size() > 64)
      throw std::runtime_error("manifest limit");
    for (const auto &file : offer.files)
      if (str(qs(file.name)) != file.name || str(qs(file.relativePath)) != file.relativePath)
        throw std::runtime_error("manifest names must be UTF-8");
    auto mount = std::make_unique<Mount>();
    mount->offer = std::move(offer);
    mount->token = token;
    mount->revision = m_revision;
    auto id = str(QUuid::createUuid().toString(QUuid::WithoutBraces));
    mount->path = m_dir.path() + '/' + qs(id);
    mount->socketPath = m_dir.path() + '/' + QString::number(m_mounts.size()) + ".sock";
    if (!QDir().mkdir(mount->path)) {
      fail("cannot create mountpoint");
      return;
    }
    mount->listener = new QLocalServer(this);
    mount->listener->setSocketOptions(QLocalServer::UserAccessOption);
    if (!mount->listener->listen(mount->socketPath)) {
      fail("cannot create private FUSE bridge");
      return;
    }
    auto *raw = mount.get();
    connect(raw->listener, &QLocalServer::newConnection, this, [this, raw, id] {
      auto *socket = raw->listener->nextPendingConnection();
      if (raw->socket) {
        socket->abort();
        socket->deleteLater();
        return;
      }
      raw->socket = socket;
      socket->setReadBufferSize(frameLimit + 4);
      connect(socket, &QLocalSocket::readyRead, this, [this, raw, id, socket] {
        try {
          raw->input += socket->read(frameLimit + 4 - raw->input.size());
          RpcMessage msg;
          while (takeMessage(raw->input, msg)) {
            auto &f = msg.fields;
            if (f.size() < 2 || !msg.bytes.empty() || !raw->pendingId.empty())
              throw std::runtime_error("invalid local request");
            auto requestId = integer(f[1]);
            if (requestId <= raw->lastId)
              throw std::runtime_error("replayed local request");
            raw->lastId = requestId;
            if (f[0] == "hello" && f.size() == 2) {
              auto response = offerMessage(raw->offer);
              response.fields.insert(response.fields.begin(), {"hello", f[1], "OK"});
              if (!writeMessage(socket, response))
                throw std::runtime_error("local queue exhausted");
            } else {
              raw->pendingOp = f[0];
              raw->pendingId = f[1];
              raw->deadline = now() + 10000;
              f.insert(f.begin(), {"request", id});
              send(std::move(msg));
            }
          }
        } catch (const std::exception &) {
          fail("FUSE bridge protocol error");
        }
      });
    });
    raw->process = process(
        "deskflow-file-prototype", {"--socket", raw->socketPath, "--mount", raw->path, "--log", "/dev/null",
                                    "--detectable", "--timeout-ms", "10000"}
    );
    raw->startupDeadline = now() + 15000;
    connect(raw->process, &QProcess::readyReadStandardOutput, this, [this, raw] {
      raw->output += raw->process->readAllStandardOutput();
      if (raw->output.size() > 4096) {
        fail("invalid mount readiness");
        return;
      }
      if (raw->ready || !raw->output.contains("DESKFLOW_FUSE_READY\n"))
        return;
      raw->ready = true;
      raw->deadline = 0;
      if (!remoteSelected(raw->token) || raw->revision != m_revision)
        return;
      QStringList args{"publish"};
      // RpcFileSource validated the full tree before signalling readiness.
      for (const auto &root : raw->offer.roots) {
        auto it = std::find_if(raw->offer.files.begin(), raw->offer.files.end(), [&](const auto &file) {
          return file.fileId == root;
        });
        if (it == raw->offer.files.end() || it->name.empty() || it->name == "." || it->name == ".." ||
            it->name.find('/') != std::string::npos || it->name.find('\0') != std::string::npos) {
          fail("unsafe root name");
          return;
        }
        args << raw->path + '/' + qs(it->name) + (it->kind == FileKind::Directory ? "/" : "");
      }
      publish(args, raw->token);
    });
    connect(raw->process, &QProcess::finished, this, [this, raw](int code, QProcess::ExitStatus) {
      if (!m_stopped)
        status(code ? "FUSE mount failed; check macFUSE FSKit / FUSE3 installation" : "File mount closed");
      if (raw->socket)
        raw->socket->abort();
    });
    m_mounts.emplace(id, std::move(mount));
    raw->process->start();
  }
  void tick()
  {
    if (m_stopped)
      return;
    if (!m_enabled && now() > m_deadline) {
      fail("capability or TLS binding timed out");
      return;
    }
    if (m_enabled && !m_nativeReady && now() > m_deadline) {
      fail("native clipboard initialization timed out");
      return;
    }
    if (m_outDeadline && now() > m_outDeadline) {
      m_outgoing.clear();
      m_outMime.clear();
      m_outDeadline = 0;
      status("Format transfer timed out; copy again to retry");
    }
    for (auto &[id, mount] : m_mounts)
      if ((mount->deadline && now() > mount->deadline) || (!mount->ready && now() > mount->startupDeadline)) {
        fail("file request or mount timed out");
        return;
      }
    m_source->collectExpired();
  }
  ClipboardBinding m_binding;
  bool m_server, m_bound = false, m_enabled = false, m_nativeReady = false, m_stopped = false, m_emptySent = false;
  QString m_certificate;
  QTemporaryDir m_dir;
  std::unique_ptr<LocalFileSource> m_source;
  Listener *m_listener = nullptr;
  QSslSocket *m_socket = nullptr;
  QTimer m_tick;
  QElapsedTimer m_clock;
  QByteArray m_input, m_watchInput;
  qint64 m_deadline = 0, m_outDeadline = 0;
  QProcess *m_watcher = nullptr;
  QPointer<QProcess> m_reader, m_publisher, m_pendingPublisher;
  QJsonObject m_native;
  uint64_t m_localToken = 0, m_remoteToken = 0, m_acknowledgedToken = 0, m_selectedToken = 0, m_revision = 0;
  std::string m_selectedSource;
  QStringList m_captureTargets, m_formatFiles;
  std::map<std::string, QByteArray> m_capture, m_outgoing, m_incoming;
  std::string m_outMime, m_incomingMime;
  uint64_t m_outToken = 0, m_outOffset = 0, m_incomingSize = 0;
  std::map<std::string, std::unique_ptr<Mount>> m_mounts;
  std::map<std::string, uint64_t> m_remoteRequests;
};

bool eligible()
{
  if (!Settings::value(Settings::Core::FileClipboard).toBool())
    return false;
  if (!Settings::value(Settings::Server::EnableClipboard).toBool()) {
    status("Disabled by clipboard sharing setting");
    return false;
  }
#ifndef Q_OS_MACOS
  if (qEnvironmentVariableIsEmpty("DISPLAY")) {
    status("Unavailable: X11 / XWayland DISPLAY is required");
    return false;
  }
  const bool wayland =
      !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland";
  if (wayland && !qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':').contains("GNOME", Qt::CaseInsensitive)) {
    status("Unavailable: Wayland clipboard bridge currently supports GNOME with XWayland");
    return false;
  }
#endif
  if (!QFileInfo(helper("deskflow-clipboard-probe")).isExecutable() ||
      !QFileInfo(helper("deskflow-file-prototype")).isExecutable()) {
    status("Unavailable: bundled clipboard helpers are missing");
    return false;
  }
  return true;
}
} // namespace
struct PeerLink::Impl : QThread
{
  ClipboardBinding binding;
  bool server;
  unsigned short remotePort = 0, localPort = 0;
  QString certificate;
  std::promise<unsigned short> started;
  Impl(ClipboardBinding b, bool s, unsigned short p)
      : binding(std::move(b)),
        server(s),
        remotePort(p),
        certificate(Settings::value(Settings::Security::Certificate).toString())
  {
  }
  void run() override
  {
    try {
      Session session(binding, server, certificate);
      localPort = session.start(remotePort);
      started.set_value(localPort);
      if (localPort)
        exec();
      session.shutdown();
    } catch (const std::exception &e) {
      status("Unavailable: " + QString::fromUtf8(e.what()));
      try {
        started.set_value(0);
      } catch (...) {
      }
    }
    setActive(false);
  }
  ~Impl() override
  {
    quit();
    wait();
    reserved = false;
    OPENSSL_cleanse(binding.secret.data(), binding.secret.size());
  }
};
bool coreClipboardActive()
{
  return active;
}
LegacyClipboardGuard::LegacyClipboardGuard() : m_lock(legacyClipboardMutex)
{
}
LegacyClipboardGuard::operator bool() const
{
  return !active;
}
PeerLink::PeerLink(std::unique_ptr<Impl> impl) : m_impl(std::move(impl))
{
}
PeerLink::~PeerLink() = default;
unsigned short PeerLink::port() const
{
  return m_impl->localPort;
}
std::unique_ptr<PeerLink> PeerLink::listen(IStream *stream)
{
  if (!eligible())
    return {};
  auto binding = stream->clipboardBinding();
  if (!binding) {
    status("Unavailable: mutual TLS authentication required");
    return {};
  }
  if (reserved.exchange(true)) {
    status("Unavailable: only one clipboard peer is supported");
    return {};
  }
  auto impl = std::make_unique<Impl>(*binding, true, 0);
  auto future = impl->started.get_future();
  impl->start();
  if (!future.get())
    return {};
  return std::unique_ptr<PeerLink>(new PeerLink(std::move(impl)));
}
std::unique_ptr<PeerLink> PeerLink::connect(IStream *stream, unsigned short port)
{
  if (!eligible())
    return {};
  auto binding = stream->clipboardBinding();
  if (!binding) {
    status("Unavailable: authenticated TLS required");
    return {};
  }
  if (reserved.exchange(true))
    return {};
  auto impl = std::make_unique<Impl>(*binding, false, port);
  auto future = impl->started.get_future();
  impl->start();
  if (!future.get())
    return {};
  return std::unique_ptr<PeerLink>(new PeerLink(std::move(impl)));
}
} // namespace deskflow::clipboard
#endif

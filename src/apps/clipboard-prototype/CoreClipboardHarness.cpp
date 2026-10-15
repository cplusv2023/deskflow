// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception

// Test-only host for the actual core runtime. Synthetic main-session bindings
// let two isolated desktops exercise TLS and helpers without keyboard capture.
// This executable is never installed or used by deskflow-core.
#include "clipboard/CoreClipboard.h"
#include "common/Settings.h"
#include "deskflow/ipc/CoreIpc.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslCertificate>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <iostream>
#include <mutex>

namespace {
volatile std::sig_atomic_t stopping = 0;
std::mutex outputMutex;
void emitJson(const QJsonObject &object)
{
  const std::lock_guard lock(outputMutex);
  std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << std::endl;
}
class BoundStream final : public deskflow::IStream
{
public:
  std::optional<deskflow::ClipboardBinding> binding;
  std::optional<deskflow::ClipboardBinding> clipboardBinding() override
  {
    return binding;
  }
  void close() override
  {
  }
  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }
  void write(const void *, uint32_t) override
  {
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return nullptr;
  }
  bool isReady() const override
  {
    return true;
  }
  uint32_t getSize() const override
  {
    return 0;
  }
};
} // namespace

void ipcSendToClient(const QString &command, const QString &args)
{
  emitJson({{"event", command}, {"value", args}});
}

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() != 6 || qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME") ||
      qEnvironmentVariableIsEmpty("XDG_STATE_HOME"))
    return 2;
  // certificate, peer certificate, test binding hex, listen/connect, port.
  QFile pem(args[2]);
  if (!pem.open(QIODevice::ReadOnly))
    return 2;
  const auto digest = QSslCertificate(pem.readAll()).digest(QCryptographicHash::Sha256);
  const auto secret = QByteArray::fromHex(args[3].toLatin1());
  if (secret.size() != 32 || digest.size() != 32 || (args[4] != "listen" && args[4] != "connect"))
    return 2;
  Settings::setValue(Settings::Security::Certificate, args[1]);
  Settings::setValue(Settings::Core::FileClipboard, true);
  Settings::setValue(Settings::Server::EnableClipboard, true);
  BoundStream stream;
  stream.binding.emplace();
  std::copy(secret.begin(), secret.end(), stream.binding->secret.begin());
  std::copy(digest.begin(), digest.end(), stream.binding->peerFingerprint.begin());
  stream.binding->peerAddress = "127.0.0.1";
  // Emulate a Portal read that started before the TLS handoff. Ready must
  // wait for it; subsequent legacy operations must be refused until teardown.
  auto initialGuard = std::make_unique<deskflow::clipboard::LegacyClipboardGuard>();
  auto peer = args[4] == "listen" ? deskflow::clipboard::PeerLink::listen(&stream)
                                  : deskflow::clipboard::PeerLink::connect(&stream, args[5].toUShort());
  emitJson({{"event", "started"}, {"port", peer ? peer->port() : 0}});
  if (!peer)
    return 3;
  std::signal(SIGTERM, [](int) { stopping = 1; });
  std::signal(SIGINT, [](int) { stopping = 1; });
  QTimer::singleShot(300, &app, [&] {
    if (deskflow::clipboard::coreClipboardActive())
      std::abort();
    emitJson({{"event", "legacyGuardReleased"}});
    initialGuard.reset();
  });
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &app, [&] {
    if (initialGuard)
      return;
    const deskflow::clipboard::LegacyClipboardGuard guard;
    if (bool(guard) == deskflow::clipboard::coreClipboardActive())
      std::abort();
    if (stopping)
      app.quit();
  });
  timer.start(50);
  QTimer::singleShot(90000, &app, &QCoreApplication::quit);
  app.exec();
  peer.reset();
  const deskflow::clipboard::LegacyClipboardGuard guard;
  emitJson({{"event", "stopped"}, {"legacyAvailable", bool(guard)}});
}

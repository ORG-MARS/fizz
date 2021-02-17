/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <fizz/crypto/aead/AESGCM128.h>
#include <fizz/crypto/aead/OpenSSLEVPCipher.h>
#include <fizz/extensions/delegatedcred/DelegatedCredentialCertManager.h>
#include <fizz/extensions/delegatedcred/SelfDelegatedCredential.h>
#ifdef FIZZ_TOOL_ENABLE_BROTLI
#include <fizz/protocol/BrotliCertificateCompressor.h>
#endif
#include <fizz/protocol/DefaultCertificateVerifier.h>
#include <fizz/protocol/ZlibCertificateCompressor.h>
#ifdef FIZZ_TOOL_ENABLE_ZSTD
#include <fizz/protocol/ZstdCertificateCompressor.h>
#endif
#include <fizz/protocol/test/Utilities.h>
#include <fizz/server/AsyncFizzServer.h>
#include <fizz/server/SlidingBloomReplayCache.h>
#include <fizz/server/TicketTypes.h>
#include <fizz/tool/FizzCommandCommon.h>
#include <fizz/util/KeyLogWriter.h>
#include <fizz/util/Parse.h>

#include <folly/Format.h>
#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/AsyncServerSocket.h>

#include <fstream>
#include <string>
#include <vector>

using namespace fizz::server;
using namespace folly;

namespace fizz {
namespace tool {
namespace {

void printUsage() {
  // clang-format off
  std::cerr
    << "Usage: server args\n"
    << "\n"
    << "Supported arguments:\n"
    << " -accept port             (set port to accept connections on. Default: 8443)\n"
    << " -ciphers c1,c2:c3;...    (Lists of ciphers in preference order, separated by colons. Default:\n"
    << "                           TLS_AES_128_GCM_SHA256,TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256)\n"
    << " -cert cert               (PEM format server certificate. Default: none, generates a self-signed cert)\n"
    << " -key key                 (PEM format private key for server certificate. Default: none)\n"
    << " -pass password           (private key password. Default: none)\n"
    << " -requestcert             (request an optional client certificate from clients. Default: false)\n"
    << " -requirecert             (require a client certificate from clients. Default: false)\n"
    << " -capath directory        (path to a directory of hashed formed CA certs used for verification.\n"
    << "                           The directory should contain one certificate or CRL per file in PEM format,\n"
    << "                           with a file name of the form hash.N for a certificate, or hash.rN for a CRL.\n"
    << "                           Refer to https://www.openssl.org/docs/man1.1.1/man1/rehash.html for how to generate such files.)\n"
    << " -cafile file             (path to a bundle file of CA certs used for verification; can be used with or without -capath.)\n"
    << " -keylog file             (dump TLS secrets to a NSS key log file; for debugging purpose only)\n"
    << " -early                   (enables sending early data during resumption. Default: false)\n"
    << " -early_max maxBytes      (sets the maximum amount allowed in early data. Default: UINT32_MAX)\n"
    << " -alpn alpn1:...          (comma-separated list of ALPNs to support. Default: none)\n"
    << " -certcompression a1:...  (enables certificate compression support for given algorithms. Default: None)\n"
    << " -fallback                (enables falling back to OpenSSL for pre-1.3 connections. Default: false)\n"
    << " -loop                    (don't exit after client disconnect. Default: false)\n"
    << " -quiet                   (hide informational logging. Default: false)\n"
    << " -v verbosity             (set verbose log level for VLOG macros. Default: 0)\n"
    << " -vmodule m1=N,...        (set per-module verbose log level for VLOG macros. Default: none)\n"
    << " -http                    (run a crude HTTP server that returns stats for GET requests. Default: false)\n"
    << " -delegatedcred cred      (use a delegated credential. If set, -cert and -key must also be set. Default: none)\n"
    << " -ech                     (use default values to simulate the sending of an encrypted client hello.)\n"
    << " -echconfigs file         (path to read ECH configs to use when decrypting an encrypted client hello.)\n"
    << "                          (If more than 1 ECH config is provided, the first config will be used.)\n"
    << "                          (The ech configs should be in JSON format: {echconfigs: [${your ECH config here with all the fields..}]})\n"
    << "                          (See FizzCommandCommonTest for an example.)\n"
    << "                          (Note ECH is implicitly enabled if this and a private key are provided.)\n"
    << " -echprivatekey key       (path to read the private key used in the ECH decryption.)\n"
    << "                          (This MUST correspond to the public key set in the ECH config.)\n"
    << "                          (If this option is specified, a corresponding ECH config must be set.)\n"
    << "                          (For OpenSSL key exchanges, please use the PEM format for the private key.)\n"
    << "                          (For the X25519 key exchange, please specify the private key in hex on the first line, "
    << "                          (and the public key in hex on the second line.)\n"
#ifdef FIZZ_TOOL_ENABLE_IO_URING
    << " -io_uring                (use io_uring for I/O. Default: false)\n"
    << " -io_uring_capacity N     (backend capacity for io_uring. Default: 128)\n"
    << " -io_uring_max_submit N   (maximum submit size for io_uring. Default: 64)\n"
    << " -io_uring_max_get N      (maximum get size for io_uring. Default: no limit)\n"
    << " -io_uring_register_fds   (use registered fds with io_uring. Default: false)\n"
    << " -io_uring_async_recv     (use async recv for io_uring. Default: false)\n"
#endif
  ;
  // clang-format on
}

class FizzServerAcceptor : AsyncServerSocket::AcceptCallback {
 public:
  explicit FizzServerAcceptor(
      uint16_t port,
      std::shared_ptr<FizzServerContext> serverCtx,
      bool loop,
      EventBase* evb,
      std::shared_ptr<SSLContext> sslCtx,
      bool registerEventCallback);

  void connectionAccepted(
      folly::NetworkSocket fdNetworkSocket,
      const SocketAddress& clientAddr) noexcept override;

  void acceptError(const std::exception& ex) noexcept override;
  void done();
  void setHttpEnabled(bool enabled) {
    http_ = enabled;
  }
  void setKeyLogWriter(std::unique_ptr<KeyLogWriter> keyLogWriter) {
    keyLogger_ = std::move(keyLogWriter);
  }
  void writeKeyLog(
      const fizz::Random& clientRandom,
      KeyLogWriter::Label label,
      const folly::ByteRange& secret) {
    if (keyLogger_) {
      keyLogger_->write(clientRandom, label, secret);
    }
  }

 private:
  bool loop_{false};
  EventBase* evb_{nullptr};
  std::shared_ptr<FizzServerContext> ctx_;
  std::shared_ptr<SSLContext> sslCtx_;
  AsyncServerSocket::UniquePtr socket_;
  std::unique_ptr<AsyncFizzServer::HandshakeCallback> cb_;
  std::unique_ptr<TerminalInputHandler> inputHandler_;
  bool http_{false};
  std::unique_ptr<KeyLogWriter> keyLogger_;
  bool registerEventCallback_{false};
};

class FizzExampleServer : public AsyncFizzServer::HandshakeCallback,
                          public AsyncSSLSocket::HandshakeCB,
                          public AsyncTransportWrapper::ReadCallback,
                          public InputHandlerCallback,
                          public SecretCollector {
 public:
  explicit FizzExampleServer(
      std::shared_ptr<AsyncFizzServer> transport,
      FizzServerAcceptor* acceptor,
      std::shared_ptr<SSLContext> sslCtx)
      : transport_(transport), acceptor_(acceptor), sslCtx_(sslCtx) {}
  void fizzHandshakeSuccess(AsyncFizzServer* server) noexcept override {
    server->setReadCB(this);
    connected_ = true;
    printHandshakeSuccess();
  }

  void fizzHandshakeError(
      AsyncFizzServer* /*server*/,
      exception_wrapper ex) noexcept override {
    LOG(ERROR) << "Handshake error: " << ex.what();
    finish();
  }

  void fizzHandshakeAttemptFallback(
      std::unique_ptr<IOBuf> clientHello) override {
    CHECK(transport_);
    LOG(INFO) << "Fallback attempt";
    auto socket = transport_->getUnderlyingTransport<AsyncSocket>();
    auto evb = socket->getEventBase();
    auto fd = socket->detachNetworkSocket().toFd();
    transport_.reset();
    sslSocket_ = AsyncSSLSocket::UniquePtr(
        new AsyncSSLSocket(sslCtx_, evb, folly::NetworkSocket::fromFd(fd)));
    sslSocket_->setPreReceivedData(std::move(clientHello));
    sslSocket_->sslAccept(this);
  }

  void handshakeSuc(AsyncSSLSocket* sock) noexcept override {
    LOG(INFO) << "Fallback SSL Handshake success";
    sock->setReadCB(this);
    connected_ = true;
    printFallbackSuccess();
  }

  void handshakeErr(
      AsyncSSLSocket* /*sock*/,
      const AsyncSocketException& ex) noexcept override {
    LOG(ERROR) << "Fallback SSL Handshake error: " << ex.what();
    finish();
  }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    *bufReturn = readBuf_.data();
    *lenReturn = readBuf_.size();
  }

  void readDataAvailable(size_t len) noexcept override {
    std::cout << std::string(readBuf_.data(), len);
  }

  bool isBufferMovable() noexcept override {
    return true;
  }

  void readBufferAvailable(std::unique_ptr<IOBuf> buf) noexcept override {
    std::cout << StringPiece(buf->coalesce()).str();
  }

  void readEOF() noexcept override {
    LOG(INFO) << "EOF";
    finish();
  }

  void readErr(const AsyncSocketException& ex) noexcept override {
    LOG(ERROR) << "Read error: " << ex.what();
    finish();
  }

  bool connected() const override {
    return connected_;
  }

  void write(std::unique_ptr<IOBuf> msg) override {
    if (transport_) {
      transport_->writeChain(nullptr, std::move(msg));
    } else if (sslSocket_) {
      sslSocket_->writeChain(nullptr, std::move(msg));
    }
  }

  void close() override {
    finish();
  }

 protected:
  std::vector<std::string> handshakeSuccessLog() {
    auto& state = transport_->getState();
    auto serverCert = state.serverCert();
    auto clientCert = state.clientCert();

    if (clientEarlyTrafficSecret_) {
      acceptor_->writeKeyLog(
          *state.clientRandom(),
          KeyLogWriter::Label::CLIENT_EARLY_TRAFFIC_SECRET,
          folly::range(*clientEarlyTrafficSecret_));
    }
    if (clientHandshakeTrafficSecret_) {
      acceptor_->writeKeyLog(
          *state.clientRandom(),
          KeyLogWriter::Label::CLIENT_HANDSHAKE_TRAFFIC_SECRET,
          folly::range(*clientHandshakeTrafficSecret_));
    }
    if (serverHandshakeTrafficSecret_) {
      acceptor_->writeKeyLog(
          *state.clientRandom(),
          KeyLogWriter::Label::SERVER_HANDSHAKE_TRAFFIC_SECRET,
          folly::range(*serverHandshakeTrafficSecret_));
    }
    if (exporterMasterSecret_) {
      acceptor_->writeKeyLog(
          *state.clientRandom(),
          KeyLogWriter::Label::EXPORTER_SECRET,
          folly::range(*exporterMasterSecret_));
    }
    if (clientAppTrafficSecret_) {
      acceptor_->writeKeyLog(
          *state.clientRandom(),
          KeyLogWriter::Label::CLIENT_TRAFFIC_SECRET_0,
          folly::range(*clientAppTrafficSecret_));
    }
    if (serverAppTrafficSecret_) {
      acceptor_->writeKeyLog(
          *state.clientRandom(),
          KeyLogWriter::Label::SERVER_TRAFFIC_SECRET_0,
          folly::range(*serverAppTrafficSecret_));
    }

    return {
        folly::to<std::string>("  TLS Version: ", toString(*state.version())),
        folly::to<std::string>("  Cipher Suite:  ", toString(*state.cipher())),
        folly::to<std::string>(
            "  Named Group: ",
            (state.group() ? toString(*state.group()) : "(none)")),
        folly::to<std::string>(
            "  Signature Scheme: ",
            (state.sigScheme() ? toString(*state.sigScheme()) : "(none)")),
        folly::to<std::string>("  PSK: ", toString(*state.pskType())),
        folly::to<std::string>(
            "  PSK Mode: ",
            (state.pskMode() ? toString(*state.pskMode()) : "(none)")),
        folly::to<std::string>(
            "  Key Exchange Type: ", toString(*state.keyExchangeType())),
        folly::to<std::string>("  Early: ", toString(*state.earlyDataType())),
        folly::to<std::string>(
            "  Server identity: ",
            (serverCert ? serverCert->getIdentity() : "(none)")),
        folly::to<std::string>(
            "  Client Identity: ",
            (clientCert ? clientCert->getIdentity() : "(none)")),
        folly::to<std::string>(
            "  Server Certificate Compression: ",
            (state.serverCertCompAlgo() ? toString(*state.serverCertCompAlgo())
                                        : "(none)")),
        folly::to<std::string>("  ALPN: ", state.alpn().value_or("(none)")),
        folly::to<std::string>(
            "  Client Random: ", folly::hexlify(*state.clientRandom())),
        folly::to<std::string>("  Secrets:"),
        folly::to<std::string>(
            "    External PSK Binder: ", secretStr(externalPskBinder_)),
        folly::to<std::string>(
            "    Resumption PSK Binder: ", secretStr(resumptionPskBinder_)),
        folly::to<std::string>(
            "    Early Exporter: ", secretStr(earlyExporterSecret_)),
        folly::to<std::string>(
            "    Early Client Data: ", secretStr(clientEarlyTrafficSecret_)),
        folly::to<std::string>(
            "    Client Handshake: ", secretStr(clientHandshakeTrafficSecret_)),
        folly::to<std::string>(
            "    Server Handshake: ", secretStr(serverHandshakeTrafficSecret_)),
        folly::to<std::string>(
            "    Exporter Master: ", secretStr(exporterMasterSecret_)),
        folly::to<std::string>(
            "    Resumption Master: ", secretStr(resumptionMasterSecret_)),
        folly::to<std::string>(
            "    Client Traffic: ", secretStr(clientAppTrafficSecret_)),
        folly::to<std::string>(
            "    Server Traffic: ", secretStr(serverAppTrafficSecret_)),
        folly::to<std::string>(
            "",
            state.context()->getECHDecrypter()
                ? "Encrypted client hello (ECH) is successful."
                : "")};
  }

  std::vector<std::string> fallbackSuccessLog() {
    auto serverCert = sslSocket_->getSelfCertificate();
    auto clientCert = sslSocket_->getPeerCertificate();
    auto ssl = sslSocket_->getSSL();
    return {
        folly::to<std::string>("  TLS Version: ", SSL_get_version(ssl)),
        folly::to<std::string>(
            "  Cipher:  ", sslSocket_->getNegotiatedCipherName()),
        folly::to<std::string>(
            "  Signature Algorithm: ", sslSocket_->getSSLCertSigAlgName()),
        folly::to<std::string>(
            "  Server identity: ",
            (serverCert ? serverCert->getIdentity() : "(none)")),
        folly::to<std::string>(
            "  Client Identity: ",
            (clientCert ? clientCert->getIdentity() : "(none)"))};
  }

  void printHandshakeSuccess() {
    LOG(INFO) << "Fizz handshake succeeded.";
    for (const auto& line : handshakeSuccessLog()) {
      LOG(INFO) << line;
    }
  }

  void printFallbackSuccess() {
    LOG(INFO) << "Fallback handshake succeeded.";
    for (const auto& line : fallbackSuccessLog()) {
      LOG(INFO) << line;
    }
  }

  void finish() {
    if (transport_ || sslSocket_) {
      auto transport = std::move(transport_);
      sslSocket_ = nullptr;
      // Forcibly clean up connection
      if (transport) {
        transport->closeNow();
      }
      acceptor_->done();
    }
  }

  std::shared_ptr<AsyncFizzServer> transport_;
  AsyncSSLSocket::UniquePtr sslSocket_;
  FizzServerAcceptor* acceptor_;
  std::shared_ptr<SSLContext> sslCtx_;
  std::array<char, 8192> readBuf_;
  bool connected_{false};
};

class FizzHTTPServer : public FizzExampleServer {
 public:
  explicit FizzHTTPServer(
      std::shared_ptr<AsyncFizzServer> transport,
      FizzServerAcceptor* acceptor,
      std::shared_ptr<SSLContext> sslCtx)
      : FizzExampleServer(transport, acceptor, sslCtx) {}

  // HTTP server doesn't send user input.
  void write(std::unique_ptr<IOBuf> /*msg*/) override {}
  void readDataAvailable(size_t len) noexcept override {
    readBufferAvailable(IOBuf::copyBuffer(readBuf_.data(), len));
  }

  void readBufferAvailable(std::unique_ptr<IOBuf> buf) noexcept override {
    if (!requestBuf_) {
      requestBuf_ = std::move(buf);
    } else {
      requestBuf_->prependChain(std::move(buf));
    }

    if (requestBuf_->computeChainDataLength() >= 5) {
      auto coalesced = requestBuf_->coalesce();
      if (strncmp(
              reinterpret_cast<const char*>(coalesced.data()), "GET /", 5) ==
          0) {
        auto response = IOBuf::create(0);
        folly::io::Appender appender(response.get(), 10);
        std::string responseBody =
            transport_ ? respondHandshakeSuccess() : respondFallbackSuccess();
        format(
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: {}\r\n\r\n"
            "{}",
            responseBody.length(),
            responseBody)(appender);
        if (transport_) {
          transport_->writeChain(nullptr, std::move(response));
          transport_->close();
        } else {
          sslSocket_->writeChain(nullptr, std::move(response));
          sslSocket_->close();
        }
      } else {
        LOG(WARNING) << "Got non-GET request: " << StringPiece(coalesced);
      }
    }
  }

 private:
  std::string respondHandshakeSuccess() {
    const std::string headerStr = "Fizz HTTP Server\n\n";
    std::string response;
    join("\n", handshakeSuccessLog(), response);
    return headerStr + response;
  }

  std::string respondFallbackSuccess() {
    const std::string headerStr = "Fizz HTTP Server (Fallback)\n\n";
    std::string response;
    join("\n", fallbackSuccessLog(), response);
    return headerStr + response;
  }

  std::unique_ptr<IOBuf> requestBuf_;
};

FizzServerAcceptor::FizzServerAcceptor(
    uint16_t port,
    std::shared_ptr<FizzServerContext> serverCtx,
    bool loop,
    EventBase* evb,
    std::shared_ptr<SSLContext> sslCtx,
    bool registerEventCallback)
    : loop_(loop),
      evb_(evb),
      ctx_(serverCtx),
      sslCtx_(sslCtx),
      registerEventCallback_(registerEventCallback) {
  socket_ = AsyncServerSocket::UniquePtr(new AsyncServerSocket(evb_));
  socket_->bind(port);
  socket_->listen(100);
  socket_->addAcceptCallback(this, evb_);
  socket_->startAccepting();
  LOG(INFO) << "Started listening on " << socket_->getAddress();
}

void FizzServerAcceptor::connectionAccepted(
    folly::NetworkSocket fdNetworkSocket,
    const SocketAddress& clientAddr) noexcept {
  int fd = fdNetworkSocket.toFd();

  LOG(INFO) << "Connection accepted from " << clientAddr;
  auto sock = new AsyncSocket(evb_, folly::NetworkSocket::fromFd(fd));
  AsyncFizzBase::TransportOptions transportOpts;
  transportOpts.registerEventCallback = registerEventCallback_;
  std::shared_ptr<AsyncFizzServer> transport =
      AsyncFizzServer::UniquePtr(new AsyncFizzServer(
          AsyncSocket::UniquePtr(sock),
          ctx_,
          nullptr,
          std::move(transportOpts)));
  socket_->pauseAccepting();
  auto serverCb = http_
      ? std::make_unique<FizzHTTPServer>(transport, this, sslCtx_)
      : std::make_unique<FizzExampleServer>(transport, this, sslCtx_);
  if (!http_) {
    inputHandler_ =
        std::make_unique<TerminalInputHandler>(evb_, serverCb.get());
  }
  transport->setSecretCallback(serverCb.get());
  cb_ = std::move(serverCb);
  transport->accept(cb_.get());
}

void FizzServerAcceptor::acceptError(const std::exception& ex) noexcept {
  LOG(ERROR) << "Failed to accept connection: " << ex.what();
  if (!loop_) {
    evb_->terminateLoopSoon();
  }
}

void FizzServerAcceptor::done() {
  cb_.reset();
  inputHandler_.reset();
  if (loop_) {
    socket_->startAccepting();
  } else {
    socket_.reset();
  }
}

std::unique_ptr<KeyExchange> createKeyExchange(
    hpke::KEMId kemId,
    std::string echPrivateKeyFile) {
  auto getPrivateKey = [&echPrivateKeyFile]() {
    std::string keyData;
    folly::ssl::EvpPkeyUniquePtr privKey;
    if (readFile(echPrivateKeyFile.c_str(), keyData)) {
      privKey = CertUtils::readPrivateKeyFromBuffer(keyData);
    }
    return privKey;
  };

  switch (kemId) {
    case hpke::KEMId::secp256r1: {
      auto kex = std::make_unique<OpenSSLECKeyExchange<P256>>();
      kex->setPrivateKey(getPrivateKey());
      return kex;
    }
    case hpke::KEMId::secp384r1: {
      auto kex = std::make_unique<OpenSSLECKeyExchange<P384>>();
      kex->setPrivateKey(getPrivateKey());
      return kex;
    }
    case hpke::KEMId::secp521r1: {
      auto kex = std::make_unique<OpenSSLECKeyExchange<P521>>();
      kex->setPrivateKey(getPrivateKey());
      return kex;
    }
    case hpke::KEMId::x25519: {
      auto kex = std::make_unique<X25519KeyExchange>();
      std::string keyData;
      std::ifstream infile(echPrivateKeyFile);

      // Assume the first line is the private key in hex, the second line is the
      // public key in hex.
      std::string privKeyStr, pubKeyStr;
      infile >> privKeyStr;
      infile >> pubKeyStr;

      kex->setKeyPair(
          folly::IOBuf::copyBuffer(folly::unhexlify(privKeyStr)),
          folly::IOBuf::copyBuffer(folly::unhexlify(pubKeyStr)));

      return kex;
    }
    default: {
      // We don't support other key exchanges right now.
      break;
    }
  }
  return nullptr;
}

std::shared_ptr<ech::Decrypter> setupDecrypterFromInputs(
    std::string echConfigsFile,
    std::string echPrivateKeyFile) {
  // Get the ECH config that corresponds to the client setup.
  auto echConfigsJson = readECHConfigsJson(echConfigsFile);
  if (!echConfigsJson.has_value()) {
    LOG(ERROR) << "Unable to load ECH configs from json file";
    return nullptr;
  }
  auto gotECHConfigs = parseECHConfigs(echConfigsJson.value());
  if (!gotECHConfigs.has_value()) {
    LOG(ERROR)
        << "Unable to parse JSON file and make ECH config."
        << "Ensure the format matches what is expected."
        << "Rough example of format: {echconfigs: [${your ECH config here with all the fields..}]}"
        << "See FizzCommandCommonTest for a more concrete example.";
    return nullptr;
  }

  auto decrypter = std::make_shared<ech::ECHConfigManager>();

  // If more that 1 ECH config is provided, we use the first one.
  ech::ECHConfig gotConfig = gotECHConfigs.value()[0];
  auto kemId =
      getKEMId((*echConfigsJson)["echconfigs"][0]["kem_id"].asString());

  // Create a key exchange and set the private key
  auto kexWithPrivateKey = createKeyExchange(kemId, echPrivateKeyFile);
  if (!kexWithPrivateKey) {
    LOG(ERROR)
        << "Unable to create a key exchange and set a private key for it.";
    return nullptr;
  }

  // Configure ECH decrpyter to be used server side.
  decrypter->addDecryptionConfig(
      ech::DecrypterParams{gotConfig, std::move(kexWithPrivateKey)});

  return decrypter;
}

std::shared_ptr<ech::Decrypter> setupDefaultDecrypter() {
  auto defaultPrivateKey = folly::IOBuf::copyBuffer(folly::unhexlify(
      "8c490e5b0c7dbe0c6d2192484d2b7a0423b3b4544f2481095a99dbf238fb350f"));
  auto defaultPublicKey = folly::IOBuf::copyBuffer(folly::unhexlify(
      "8a07563949fac6232936ed6f36c4fa735930ecdeaef6734e314aeac35a56fd0a"));

  ech::ECHConfig chosenConfig = getDefaultECHConfigs()[0];
  auto kex = std::make_unique<X25519KeyExchange>();
  kex->setKeyPair(std::move(defaultPrivateKey), std::move(defaultPublicKey));

  // Configure ECH decrpyter to be used server side.
  auto decrypter = std::make_shared<ech::ECHConfigManager>();
  decrypter->addDecryptionConfig(
      ech::DecrypterParams{chosenConfig, std::move(kex)});

  return decrypter;
}

} // namespace

int fizzServerCommand(const std::vector<std::string>& args) {
  uint16_t port = 8443;
  std::string certPath;
  std::string keyPath;
  std::string keyPass;
  ClientAuthMode clientAuthMode = ClientAuthMode::None;
  std::string caPath;
  std::string caFile;
  std::string keyLogFile;
  bool early = false;
  std::vector<std::string> alpns;
  folly::Optional<std::vector<CertificateCompressionAlgorithm>> compAlgos;
  bool loop = false;
  bool fallback = false;
  bool http = false;
  uint32_t earlyDataSize = std::numeric_limits<uint32_t>::max();
  std::vector<std::vector<CipherSuite>> ciphers {
    {CipherSuite::TLS_AES_128_GCM_SHA256, CipherSuite::TLS_AES_256_GCM_SHA384},
#if FOLLY_OPENSSL_HAS_CHACHA
    {
      CipherSuite::TLS_CHACHA20_POLY1305_SHA256
    }
#endif
  };
  std::string credPath;
  bool ech = false;
  std::string echConfigsFile;
  std::string echPrivateKeyFile;
  bool uring = false;
  bool uringAsync = false;
  bool uringRegisterFds = false;
  int32_t uringCapacity = 128;
  int32_t uringMaxSubmit = 64;
  int32_t uringMaxGet = -1;

  // clang-format off
  FizzArgHandlerMap handlers = {
    {"-accept", {true, [&port](const std::string& arg) {
        port = portFromString(arg, true);
    }}},
    {"-ciphers", {true, [&ciphers](const std::string& arg) {
        ciphers.clear();
        std::vector<std::string> list;
        folly::split(":", arg, list);
        for (const auto& item : list) {
          try {
            ciphers.push_back(splitParse<CipherSuite>(item, ","));
          }
          catch (const std::exception& e) {
            LOG(ERROR) << "Error parsing cipher suites: " << e.what();
            throw;
          }
        }
    }}},
    {"-cert", {true, [&certPath](const std::string& arg) { certPath = arg; }}},
    {"-key", {true, [&keyPath](const std::string& arg) { keyPath = arg; }}},
    {"-pass", {true, [&keyPass](const std::string& arg) { keyPass = arg; }}},
    {"-requestcert", {false, [&clientAuthMode](const std::string&) {
      clientAuthMode = ClientAuthMode::Optional;
    }}},
    {"-requirecert", {false, [&clientAuthMode](const std::string&) {
      clientAuthMode = ClientAuthMode::Required;
    }}},
    {"-capath", {true, [&caPath](const std::string& arg) { caPath = arg; }}},
    {"-cafile", {true, [&caFile](const std::string& arg) { caFile = arg; }}},
    {"-keylog", {true,[&keyLogFile](const std::string& arg) {
      keyLogFile = arg;
    }}},
    {"-early", {false, [&early](const std::string&) { early = true; }}},
    {"-alpn", {true, [&alpns](const std::string& arg) {
        alpns.clear();
        folly::split(":", arg, alpns);
    }}},
    {"-certcompression", {true, [&compAlgos](const std::string& arg) {
        try {
          compAlgos = splitParse<CertificateCompressionAlgorithm>(arg);
        } catch (const std::exception& e) {
          LOG(ERROR) << "Error parsing certificate compression algorithms: " << e.what();
          throw;
        }
    }}},
    {"-loop", {false, [&loop](const std::string&) { loop = true; }}},
    {"-quiet", {false, [](const std::string&) {
        FLAGS_minloglevel = google::GLOG_ERROR;
    }}},
    {"-fallback", {false, [&fallback](const std::string&) {
        fallback = true;
    }}},
    {"-http", {false, [&http](const std::string&) { http = true; }}},
    {"-early_max", {true, [&earlyDataSize](const std::string& arg) {
        earlyDataSize = folly::to<uint32_t>(arg);
    }}},
    {"-delegatedcred", {true, [&credPath](const std::string& arg) {
        credPath = arg;
    }}},
    {"-ech", {false, [&ech](const std::string&) {
        ech = true;
    }}},
    {"-echconfigs", {true, [&echConfigsFile](const std::string& arg) {
        echConfigsFile = arg;
    }}},
    {"-echprivatekey", {true, [&echPrivateKeyFile](const std::string& arg) {
        echPrivateKeyFile = arg;
    }}}
#ifdef FIZZ_TOOL_ENABLE_IO_URING
    ,{"-io_uring", {false, [&uring](const std::string&) { uring = true; }}},
    {"-io_uring_async_recv", {false, [&uringAsync](const std::string&) {
        uringAsync = true;
    }}},
    {"-io_uring_register_fds", {false, [&uringRegisterFds](const std::string&) {
        uringRegisterFds = true;
    }}},
    {"-io_uring_capacity", {true, [&uringCapacity](const std::string& arg) {
        uringCapacity = folly::to<int32_t>(arg);
    }}},
    {"-io_uring_max_get", {true, [&uringMaxGet](const std::string& arg) {
        uringMaxGet = folly::to<int32_t>(arg);
    }}},
    {"-io_uring_max_submit", {true, [&uringMaxSubmit](const std::string& arg) {
        uringMaxSubmit = folly::to<int32_t>(arg);
    }}}
#endif
  };
  // clang-format on

  try {
    if (parseArguments(args, handlers, printUsage)) {
      // Parsing failed, return
      return 1;
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "Error: " << e.what();
    return 1;
  }

  // Sanity check input.
  if (certPath.empty() != keyPath.empty()) {
    LOG(ERROR) << "-cert and -key are both required when specified";
    return 1;
  }

  if (!credPath.empty() && (certPath.empty() || keyPath.empty())) {
    LOG(ERROR)
        << "-cert and -key are both required when delegated credentials are in use";
    return 1;
  }

  EventBase evb(folly::EventBase::Options().setBackendFactory([uring,
                                                               uringAsync,
                                                               uringRegisterFds,
                                                               uringCapacity,
                                                               uringMaxSubmit,
                                                               uringMaxGet] {
    return setupBackend(
        uring,
        uringAsync,
        uringRegisterFds,
        uringCapacity,
        uringMaxSubmit,
        uringMaxGet);
  }));
  std::shared_ptr<const CertificateVerifier> verifier;

  if (clientAuthMode != ClientAuthMode::None) {
    // Initialize CA store first, if given.
    folly::ssl::X509StoreUniquePtr storePtr;
    if (!caPath.empty() || !caFile.empty()) {
      storePtr.reset(X509_STORE_new());
      auto caFilePtr = caFile.empty() ? nullptr : caFile.c_str();
      auto caPathPtr = caPath.empty() ? nullptr : caPath.c_str();

      if (X509_STORE_load_locations(storePtr.get(), caFilePtr, caPathPtr) ==
          0) {
        LOG(ERROR) << "Failed to load CA certificates";
        return 1;
      }
    }

    verifier = std::make_shared<const DefaultCertificateVerifier>(
        VerificationContext::Server, std::move(storePtr));
  }

  auto serverContext = std::make_shared<FizzServerContext>();

  if (ech) {
    // Use ECH  default values.
    serverContext->setECHDecrypter(setupDefaultDecrypter());
  }

  if ((echConfigsFile.empty() && !echPrivateKeyFile.empty()) ||
      (!echConfigsFile.empty() && echPrivateKeyFile.empty())) {
    LOG(ERROR)
        << "Must provide both an ECH configs file (\"-echconfigs [config file]\") and an ECH private key (\"-echprivatekey [key file]\") or neither.";
    return 1;
  }

  // ECH is implicitly enabled if ECH configs and a private key are provided.
  // Note that if there are ECH configs provided, there must be an associated
  // key file.
  if (!echConfigsFile.empty()) {
    // Setup ECH decrypting tools based on user provided ECH configs and private
    // key.
    auto decrypter =
        setupDecrypterFromInputs(echConfigsFile, echPrivateKeyFile);
    if (!decrypter) {
      LOG(ERROR) << "Unable to setup decrypter.";
      return 1;
    }
    serverContext->setECHDecrypter(decrypter);
  }

  serverContext->setSupportedCiphers(std::move(ciphers));
  serverContext->setClientAuthMode(clientAuthMode);
  serverContext->setClientCertVerifier(verifier);

  auto ticketCipher = std::make_shared<
      Aead128GCMTicketCipher<TicketCodec<CertificateStorage::X509>>>(
      std::make_shared<OpenSSLFactory>(), std::make_shared<CertManager>());
  auto ticketSeed = RandomGenerator<32>().generateRandom();
  ticketCipher->setTicketSecrets({{range(ticketSeed)}});
  serverContext->setTicketCipher(ticketCipher);

  // Store a vector of compressors and algorithms for which there are
  // compressors.
  std::unique_ptr<CertManager> certManager =
      std::make_unique<fizz::extensions::DelegatedCredentialCertManager>();
  std::vector<std::shared_ptr<CertificateCompressor>> compressors;
  std::vector<CertificateCompressionAlgorithm> finalAlgos;
  if (compAlgos) {
    for (const auto& algo : *compAlgos) {
      switch (algo) {
        case CertificateCompressionAlgorithm::zlib:
          compressors.push_back(std::make_shared<ZlibCertificateCompressor>(9));
          finalAlgos.push_back(algo);
          break;
#ifdef FIZZ_TOOL_ENABLE_BROTLI
        case CertificateCompressionAlgorithm::brotli:
          compressors.push_back(
              std::make_shared<BrotliCertificateCompressor>());
          finalAlgos.push_back(algo);
          break;
#endif
#ifdef FIZZ_TOOL_ENABLE_ZSTD
        case CertificateCompressionAlgorithm::zstd:
          compressors.push_back(
              std::make_shared<ZstdCertificateCompressor>(19));
          finalAlgos.push_back(algo);
          break;
#endif
        default:
          LOG(WARNING) << "Don't know what compressor to use for "
                       << toString(algo) << ", ignoring.";
          break;
      }
    }
  }
  serverContext->setSupportedCompressionAlgorithms(finalAlgos);

  if (!certPath.empty()) {
    std::string certData;
    std::string keyData;
    if (!readFile(certPath.c_str(), certData)) {
      LOG(ERROR) << "Failed to read certificate";
      return 1;
    } else if (!readFile(keyPath.c_str(), keyData)) {
      LOG(ERROR) << "Failed to read private key";
      return 1;
    }
    std::unique_ptr<SelfCert> cert;
    if (credPath.empty()) {
      if (!keyPass.empty()) {
        cert = CertUtils::makeSelfCert(certData, keyData, keyPass, compressors);
      } else {
        cert = CertUtils::makeSelfCert(certData, keyData, compressors);
      }
    } else {
      std::string credData;
      if (!readFile(credPath.c_str(), credData)) {
        LOG(ERROR) << "Failed to read credential";
        return 1;
      }
      std::vector<Extension> credVec;
      credVec.emplace_back(Extension{
          ExtensionType::delegated_credential,
          folly::IOBuf::copyBuffer(std::move(credData))});

      auto certs = folly::ssl::OpenSSLCertUtils::readCertsFromBuffer(
          folly::StringPiece(certData));

      folly::ssl::EvpPkeyUniquePtr credPrivKey =
          CertUtils::readPrivateKeyFromBuffer(
              keyData, keyPass.empty() ? nullptr : &keyPass[0]);

      try {
        auto cred = getExtension<fizz::extensions::DelegatedCredential>(
            std::move(credVec));
        switch (CertUtils::getKeyType(credPrivKey)) {
          case KeyType::RSA:
            cert = std::make_unique<
                fizz::extensions::SelfDelegatedCredentialImpl<KeyType::RSA>>(
                std::move(certs),
                std::move(credPrivKey),
                std::move(*cred),
                compressors);
            break;
          case KeyType::P256:
            cert = std::make_unique<
                fizz::extensions::SelfDelegatedCredentialImpl<KeyType::P256>>(
                std::move(certs),
                std::move(credPrivKey),
                std::move(*cred),
                compressors);
            break;
          case KeyType::P384:
            cert = std::make_unique<
                fizz::extensions::SelfDelegatedCredentialImpl<KeyType::P384>>(
                std::move(certs),
                std::move(credPrivKey),
                std::move(*cred),
                compressors);
            break;
          case KeyType::P521:
            cert = std::make_unique<
                fizz::extensions::SelfDelegatedCredentialImpl<KeyType::P521>>(
                std::move(certs),
                std::move(credPrivKey),
                std::move(*cred),
                compressors);
            break;
          case KeyType::ED25519:
            cert =
                std::make_unique<fizz::extensions::SelfDelegatedCredentialImpl<
                    KeyType::ED25519>>(
                    std::move(certs),
                    std::move(credPrivKey),
                    std::move(*cred),
                    compressors);
            break;
        }
      } catch (const std::exception& e) {
        LOG(ERROR) << "Credential parsing failed: " << e.what();
        return 1;
      }
    }
    certManager->addCert(std::move(cert), true);
  } else {
    auto certData = fizz::test::createCert("fizz-self-signed", false, nullptr);
    std::vector<folly::ssl::X509UniquePtr> certChain;
    certChain.push_back(std::move(certData.cert));
    auto cert = std::make_unique<SelfCertImpl<KeyType::P256>>(
        std::move(certData.key), std::move(certChain), compressors);
    certManager->addCert(std::move(cert), true);
  }
  serverContext->setCertManager(std::move(certManager));

  if (early) {
    serverContext->setEarlyDataSettings(
        true,
        {std::chrono::seconds(-10), std::chrono::seconds(10)},
        std::make_shared<SlidingBloomReplayCache>(240, 140000, 0.0005, &evb));
    serverContext->setMaxEarlyDataSize(earlyDataSize);
  }

  std::shared_ptr<SSLContext> sslContext;
  if (fallback) {
    if (certPath.empty()) {
      LOG(ERROR) << "Fallback mode requires explicit certificates";
      return 1;
    }
    sslContext = std::make_shared<SSLContext>();
    sslContext->loadCertKeyPairFromFiles(certPath.c_str(), keyPath.c_str());
    SSL_CTX_set_ecdh_auto(sslContext->getSSLCtx(), 1);
  }
  serverContext->setVersionFallbackEnabled(fallback);

  if (!alpns.empty()) {
    serverContext->setSupportedAlpns(std::move(alpns));
  }

  serverContext->setSupportedVersions(
      {ProtocolVersion::tls_1_3, ProtocolVersion::tls_1_3_28});
  FizzServerAcceptor acceptor(
      port, serverContext, loop, &evb, sslContext, uringAsync);
  if (!keyLogFile.empty()) {
    acceptor.setKeyLogWriter(std::make_unique<KeyLogWriter>(keyLogFile));
  }
  acceptor.setHttpEnabled(http);
  evb.loop();
  return 0;
}

} // namespace tool
} // namespace fizz

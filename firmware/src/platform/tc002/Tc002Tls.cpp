// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/Tc002Tls.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#include "bearssl.h"

namespace stipple {
namespace platform {
namespace tc002 {

/// The BearSSL anchor array. Out of the header on purpose — see Tls.
struct Tls::AnchorList {
    std::vector<br_x509_trust_anchor> items;
};

namespace {

/// Days from 0000-01-01 to 1970-01-01, which is what BearSSL counts from.
constexpr std::uint32_t kDaysToEpoch = 719528;

/// A clock reading earlier than this has not been set.
///
/// 2020-01-01. The device boots at 1970 and stays there until SNTP answers,
/// and validating a certificate against 1970 rejects every certificate ever
/// issued — which would read as "the internet is broken" rather than as
/// "this clock is wrong".
constexpr std::int64_t kPlausibleEpoch = 1577836800;

bool fileExists(const char* path) noexcept {
    struct stat info;
    return ::stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

/// Everything one connection needs, in one allocation.
///
/// BearSSL allocates nothing itself, so the buffer lives here. Mono rather
/// than bidi: this sends a request and then reads a reply, never both at
/// once, and the bidirectional buffer is most of another 16 KB.
struct Session {
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    br_sslio_context io;
    int fd;
    unsigned char buffer[BR_SSL_BUFSIZE_MONO];
};

int sockRead(void* context, unsigned char* data, std::size_t length) {
    const int fd = *static_cast<int*>(context);
    for (;;) {
        const ssize_t got = ::recv(fd, data, length, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        // The socket carries SO_RCVTIMEO, so a server that stops talking
        // surfaces here as an error rather than as a thread parked for ever.
        return static_cast<int>(got);
    }
}

int sockWrite(void* context, const unsigned char* data, std::size_t length) {
    const int fd = *static_cast<int*>(context);
    for (;;) {
        const ssize_t wrote = ::send(fd, data, length, MSG_NOSIGNAL);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        return static_cast<int>(wrote);
    }
}

/// BearSSL's error code, in words short enough for the panel.
///
/// Only the ones somebody can act on are named; the rest keep their number,
/// which is still something to look up. The OpenSSL attempt taught this the
/// hard way — every failure arrived as the same sentence, and the sentence
/// was wrong.
std::string describe(int error) {
    switch (error) {
        case BR_ERR_X509_EXPIRED:         return "certificate expired";
        case BR_ERR_X509_NOT_TRUSTED:     return "issuer not trusted";
        case BR_ERR_X509_BAD_SERVER_NAME: return "wrong host on cert";
        case BR_ERR_X509_UNSUPPORTED:     return "unsupported cert";
        case BR_ERR_X509_WEAK_PUBLIC_KEY: return "weak key on cert";
        case BR_ERR_BAD_VERSION:          return "server too old for tls12";
        case BR_ERR_UNSUPPORTED_VERSION:  return "server wants newer tls";
        case BR_ERR_BAD_CIPHER_SUITE:     return "no shared cipher";
        case BR_ERR_NO_CLIENT_AUTH:       return "server wants client cert";
        case BR_ERR_IO:                   return "connection cut";
        default: break;
    }
    return "tls error " + std::to_string(error);
}

/// Seed the PRNG from the kernel.
///
/// Explicit rather than trusting BearSSL's build-time detection of a system
/// seeder: whether it finds one depends on how the vendored tree was
/// configured, and a TLS client quietly running on a predictable PRNG is
/// worse than one that refuses to run at all.
bool seed(br_ssl_engine_context* engine) {
    unsigned char entropy[32];
    std::FILE* source = std::fopen("/dev/urandom", "rb");
    if (source == nullptr) {
        return false;
    }
    const std::size_t got = std::fread(entropy, 1, sizeof(entropy), source);
    std::fclose(source);
    if (got != sizeof(entropy)) {
        return false;
    }
    br_ssl_engine_inject_entropy(engine, entropy, got);
    return true;
}

/// Collects bytes streamed by BearSSL's decoders.
void collect(void* context, const void* buffer, std::size_t length) {
    auto* out = static_cast<std::vector<unsigned char>*>(context);
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    out->insert(out->end(), bytes, bytes + length);
}

}  // namespace

Tls::Tls() : anchors_(new AnchorList()) {
    const char* bundle = kCaBundlePath;
    if (!fileExists(bundle)) {
        bundle = kCaBundleFallback;
    }
    if (!fileExists(bundle)) {
        problem_ = "no ca bundle";
        return;
    }
    if (!loadBundle(bundle)) {
        return;
    }
    if (anchors_->items.empty()) {
        problem_ = "ca bundle has no roots";
    }
}

Tls::~Tls() = default;

Tls& Tls::instance() {
    // Function-local static: initialised once, and the standard makes that
    // thread-safe, which matters because the first caller is a fetch worker.
    static Tls one;
    return one;
}

int Tls::anchorCount() const noexcept {
    return static_cast<int>(anchors_->items.size());
}

bool Tls::loadBundle(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        problem_ = "cannot read ca bundle";
        return false;
    }

    std::string pem;
    char chunk[8192];
    std::size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (pem.size() + got > kMaxBundleBytes) {
            std::fclose(file);
            problem_ = "ca bundle too large";
            return false;
        }
        pem.append(chunk, got);
    }
    std::fclose(file);

    br_pem_decoder_context decoder;
    br_pem_decoder_init(&decoder);

    std::vector<unsigned char> der;
    bool wantThisObject = false;
    br_pem_decoder_setdest(&decoder, collect, &der);

    std::size_t at = 0;
    while (at < pem.size()) {
        const std::size_t taken =
            br_pem_decoder_push(&decoder, pem.data() + at, pem.size() - at);
        at += taken;

        const int event = br_pem_decoder_event(&decoder);
        if (event == BR_PEM_BEGIN_OBJ) {
            // A bundle may hold more than certificates. Anything else is
            // skipped rather than fed to the X.509 decoder.
            const char* name = br_pem_decoder_name(&decoder);
            wantThisObject = name != nullptr &&
                             (std::strcmp(name, "CERTIFICATE") == 0 ||
                              std::strcmp(name, "X509 CERTIFICATE") == 0 ||
                              std::strcmp(name, "TRUSTED CERTIFICATE") == 0);
            der.clear();
        } else if (event == BR_PEM_END_OBJ) {
            if (wantThisObject && !der.empty()) {
                addAnchor(der.data(), der.size());
            }
            der.clear();
            wantThisObject = false;
        } else if (event == BR_PEM_ERROR) {
            // One malformed object does not condemn the bundle: the decoder
            // resynchronises at the next BEGIN line, and losing one root is
            // better than losing every fetch.
            der.clear();
            wantThisObject = false;
        } else if (taken == 0) {
            break;  // no progress and no event to read; nothing more to do
        }
    }
    return true;
}

bool Tls::addAnchor(const unsigned char* der, std::size_t length) {
    std::unique_ptr<Anchor> anchor(new (std::nothrow) Anchor());
    if (anchor == nullptr) {
        return false;
    }

    // br_x509_decoder streams the *subject* DN to this callback, which is
    // exactly what a chain's issuer field gets matched against.
    br_x509_decoder_context decoder;
    br_x509_decoder_init(&decoder, collect, &anchor->dn);
    br_x509_decoder_push(&decoder, der, length);

    if (br_x509_decoder_last_error(&decoder) != 0) {
        return false;
    }
    // A root that is not a CA cannot sign the chain about to be checked.
    if (!br_x509_decoder_isCA(&decoder)) {
        return false;
    }

    const br_x509_pkey* key = br_x509_decoder_get_pkey(&decoder);
    if (key == nullptr || anchor->dn.empty()) {
        return false;
    }

    br_x509_trust_anchor item;
    std::memset(&item, 0, sizeof(item));
    item.flags = BR_X509_TA_CA;
    item.pkey.key_type = key->key_type;

    if (key->key_type == BR_KEYTYPE_RSA) {
        anchor->keyA.assign(key->key.rsa.n, key->key.rsa.n + key->key.rsa.nlen);
        anchor->keyB.assign(key->key.rsa.e, key->key.rsa.e + key->key.rsa.elen);
        item.pkey.key.rsa.n = anchor->keyA.data();
        item.pkey.key.rsa.nlen = anchor->keyA.size();
        item.pkey.key.rsa.e = anchor->keyB.data();
        item.pkey.key.rsa.elen = anchor->keyB.size();
    } else if (key->key_type == BR_KEYTYPE_EC) {
        anchor->keyA.assign(key->key.ec.q, key->key.ec.q + key->key.ec.qlen);
        item.pkey.key.ec.curve = key->key.ec.curve;
        item.pkey.key.ec.q = anchor->keyA.data();
        item.pkey.key.ec.qlen = anchor->keyA.size();
    } else {
        return false;
    }

    item.dn.data = anchor->dn.data();
    item.dn.len = anchor->dn.size();

    // Stored before the anchor is published, so the array never names memory
    // that is not yet owned.
    storage_.push_back(std::move(anchor));
    anchors_->items.push_back(item);
    return true;
}

void* Tls::connect(int fd, const std::string& host, std::string& problem) {
    if (anchors_->items.empty()) {
        problem = problem_.empty() ? std::string("no tls") : problem_;
        return nullptr;
    }

    // The clock, before anything else. BearSSL checks notBefore/notAfter
    // against the time it is given, and a device that has not heard from SNTP
    // yet is at 1970 — which rejects every certificate ever issued and reads
    // as the network being broken.
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (now < kPlausibleEpoch) {
        problem = "clock not set";
        return nullptr;
    }

    auto* session = new (std::nothrow) Session();
    if (session == nullptr) {
        problem = "out of memory for tls";
        return nullptr;
    }
    session->fd = fd;

    br_ssl_client_init_full(&session->client, &session->x509,
                            anchors_->items.data(), anchors_->items.size());

    br_x509_minimal_set_time(
        &session->x509,
        static_cast<std::uint32_t>(now / 86400) + kDaysToEpoch,
        static_cast<std::uint32_t>(now % 86400));

    // TLS 1.2 only. It is BearSSL 0.6's ceiling as well as our floor, and
    // everything below it is deprecated wherever an API is hosted — so this
    // refuses nothing that would have worked, and declines to negotiate down
    // if a server asks.
    br_ssl_engine_set_versions(&session->client.eng, BR_TLS12, BR_TLS12);

    if (!seed(&session->client.eng)) {
        delete session;
        problem = "no entropy for tls";
        return nullptr;
    }

    br_ssl_engine_set_buffer(&session->client.eng, session->buffer,
                             sizeof(session->buffer), 0);

    // The host name does three jobs from this one argument: SNI on the wire,
    // the name the certificate is checked against, and the session key. One
    // place, so it cannot be set for one and forgotten for another — which is
    // precisely the mistake that is easy to make with OpenSSL.
    if (br_ssl_client_reset(&session->client, host.c_str(), 0) != 1) {
        delete session;
        problem = "tls setup failed";
        return nullptr;
    }

    br_sslio_init(&session->io, &session->client.eng, sockRead, &session->fd,
                  sockWrite, &session->fd);

    // BearSSL runs the handshake lazily, on the first I/O. Forcing it here
    // means a failure is reported as itself rather than surfacing later as a
    // write error with the reason already lost.
    if (br_sslio_flush(&session->io) != 0) {
        problem = describe(br_ssl_engine_last_error(&session->client.eng));
        delete session;
        return nullptr;
    }

    return session;
}

int Tls::read(void* handle, void* buffer, int length) {
    if (handle == nullptr || length <= 0) {
        return -1;
    }
    auto* session = static_cast<Session*>(handle);

    const int got =
        br_sslio_read(&session->io, buffer, static_cast<std::size_t>(length));
    if (got >= 0) {
        return got;
    }
    // BearSSL signals a clean close and a broken connection the same way, and
    // the caller needs them apart: one ends a body that had no Content-Length,
    // the other is a failure worth reporting.
    return br_ssl_engine_last_error(&session->client.eng) == BR_ERR_OK ? 0 : -1;
}

int Tls::write(void* handle, const void* buffer, int length) {
    if (handle == nullptr || length <= 0) {
        return -1;
    }
    auto* session = static_cast<Session*>(handle);

    if (br_sslio_write_all(&session->io, buffer,
                           static_cast<std::size_t>(length)) != 0) {
        return -1;
    }
    if (br_sslio_flush(&session->io) != 0) {
        return -1;
    }
    return length;
}

void Tls::close(void* handle) {
    if (handle == nullptr) {
        return;
    }
    auto* session = static_cast<Session*>(handle);
    // Best effort. The socket is about to close anyway, and waiting on the
    // peer's close_notify would be waiting on a server with no reason to
    // hurry.
    br_sslio_close(&session->io);
    delete session;
}

}  // namespace tc002
}  // namespace platform
}  // namespace stipple

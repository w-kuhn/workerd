// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sockets.h"
#include "system-streams.h"


namespace workerd::api {


bool isValidAddress(kj::StringPtr address) {
  // This function performs some basic length and characters checks, it does not guarantee that
  // the specified address is a valid domain. It should only be used to reject malicious
  // addresses.
  if (address.size() > (255 + 6) || address.size() == 0) {
    // RFC1035 states that maximum domain name length is 255 octets. But we also add 6 to support
    // port numbers in the address.
    //
    // IP addresses are always shorter, so we take the max domain length instead.
    return false;
  }

  for (int i = 0; i < address.size(); i++) {
    switch (address[i]) {
      case '-':
      case '.':
      case ':':
      case '_':
      case '[': case ']': // For IPv6.
        break;
      default:
        if ((address[i] >= 'a' && address[i] <= 'z') ||
            (address[i] >= 'A' && address[i] <= 'Z') ||
            (address[i] >= '0' && address[i] <= '9')) {
          break;
        }
        return false;
    }
  }
  return true;
}

jsg::Ref<Socket> connectImplNoOutputLock(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, kj::String address) {

  JSG_REQUIRE(isValidAddress(address), TypeError,
      "Specified address is empty string, contains unsupported characters or is too long.");

  auto& ioContext = IoContext::current();

  auto jsRequest = Request::constructor(js, kj::str(address), nullptr);
  kj::Own<WorkerInterface> client = fetcher->getClient(
      ioContext, jsRequest->serializeCfBlobJson(js), "connect"_kj);

  // Set up the connection.
  auto headers = kj::heap<kj::HttpHeaders>(ioContext.getHeaderTable());
  auto httpClient = kj::newHttpClient(*client);
  auto request = httpClient->connect(address, *headers);

  // Initialise the readable/writable streams with the readable/writable sides of an AsyncIoStream.
  auto sysStreams = newSystemMultiStream(kj::mv(request.connection), ioContext);
  auto readable = jsg::alloc<ReadableStream>(ioContext, kj::mv(sysStreams.readable));
  auto writable = jsg::alloc<WritableStream>(ioContext, kj::mv(sysStreams.writable));

  auto closeFulfiller = kj::heap<jsg::PromiseResolverPair<void>>(
      jsg::newPromiseAndResolver<void>(ioContext.getCurrentLock().getIsolate()));
  closeFulfiller->promise.markAsHandled();

  auto result = jsg::alloc<Socket>(
      js, kj::mv(readable), kj::mv(writable), kj::mv(closeFulfiller));
  // `handleProxyStatus` needs an initialised refcount to use `JSG_THIS`, hence it cannot be
  // called in Socket's constructor.
  result->handleProxyStatus(js, kj::mv(request.status));
  return result;
}

jsg::Ref<Socket> connectImpl(
    jsg::Lock& js, kj::Maybe<jsg::Ref<Fetcher>> fetcher, kj::String address,
    CompatibilityFlags::Reader featureFlags) {
  // `connect()` should be hidden when the feature flag is off, so we shouldn't even get here.
  KJ_ASSERT(featureFlags.getTcpSocketsSupport());

  jsg::Ref<Fetcher> actualFetcher = nullptr;
  KJ_IF_MAYBE(f, fetcher) {
    actualFetcher = kj::mv(*f);
  } else {
    actualFetcher = jsg::alloc<Fetcher>(
        IoContext::NULL_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
  }
  return connectImplNoOutputLock(js, kj::mv(actualFetcher), kj::mv(address));
}

jsg::Promise<void> Socket::close(jsg::Lock& js) {
  // Forcibly close the readable/writable streams.
  auto cancelPromise = readable->getController().cancel(js, nullptr);
  auto abortPromise = writable->getController().abort(js, nullptr);
  // The below is effectively `Promise.all(cancelPromise, abortPromise)`
  return cancelPromise.then(js, [abortPromise = kj::mv(abortPromise), this](jsg::Lock& js) mutable {
    return abortPromise.then(js, [this](jsg::Lock& js) {
      resolveFulfiller(js, nullptr);
      return js.resolvedPromise();
    });
  }, [this](jsg::Lock& js, jsg::Value err) { return errorHandler(js, kj::mv(err)); });
}

void Socket::handleProxyStatus(
    jsg::Lock& js, kj::Promise<kj::HttpClient::ConnectRequest::Status> status) {
  auto& context = IoContext::current();
  auto result = context.awaitIo(js, kj::mv(status),
      [this, self = JSG_THIS](jsg::Lock& js, kj::HttpClient::ConnectRequest::Status&& status) -> void {
    if (status.statusCode < 200 || status.statusCode >= 300) {
      // If the status indicates an unsucessful connection we need to reject the `closeFulfiller`
      // with an exception. This will reject the socket's `closed` promise.
      auto exc = kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
        kj::str(JSG_EXCEPTION(Error) ": proxy request failed"));
      resolveFulfiller(js, exc);
    }
  });
  result.markAsHandled();
}

}  // namespace workerd::api

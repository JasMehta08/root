// @(#)root/net:$Id$
// Author: Jakob Blomer

/*************************************************************************
 * Copyright (C) 1995-2025, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_RCurlConnection
#define ROOT_RCurlConnection

#include <ROOT/RError.hxx>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace ROOT {
namespace Internal {

enum class EHTTPCredentialsType {
   kNone,
   kS3
};

struct RS3Credentials final {
   std::string fAccessKey;
   std::string fSecretKey;
   std::string fRegion;
};

struct RHTTPCredentials {
   EHTTPCredentialsType fType = EHTTPCredentialsType::kNone;
   std::variant<RS3Credentials> fData;
};

/// Encapsulates a curl easy handle and provides an interface to send HTTP HEAD and (multi-)range queries.
class RCurlConnection {
public:
   /// Return value for both HEAD and GET requests. In case of errors, provides the reason for the failure as code
   /// and as message.
   struct RStatus {
      enum EStatusCode {
         kSuccess = 0,
         kTooManyRanges, ///< should not get to the user; number of request ranges is automatically reduced as needed
         kNotFound,
         kForbidden,
         kIOError,
         /// The server reported a condition that may pass by itself: HTTP 429 or 5xx. Retried automatically.
         kTransientHTTP,
         /// The request did not complete at the network level (timeout, reset connection, ...). Retried
         /// automatically. Kept apart from kTransientHTTP so that "the server is busy" and "the network is
         /// down" can be told apart without parsing fStatusMsg.
         kTransportError,
         kUnknown
      };

      EStatusCode fStatusCode = kUnknown;
      std::string fStatusMsg;
      /// HTTP response code, or 0 if the request failed before one was received
      long fHttpCode = 0;
      /// CURLcode of the transfer, 0 (CURLE_OK) if curl itself succeeded. An int rather than a CURLcode
      /// because libcurl is linked PRIVATE, so dependents get no curl/curl.h -- as with fHandle below.
      int fCurlCode = 0;
      /// Value of the Retry-After response header in seconds, or -1 if it was absent or unparseable
      long fRetryAfterSec = -1;

      RStatus() = default;
      explicit RStatus(EStatusCode code) : fStatusCode(code) {}

      explicit operator bool() const { return fStatusCode == kSuccess; }
      /// True for failures that are worth repeating; a permanent error such as 403 or 404 is not.
      bool IsRetryable() const { return fStatusCode == kTransientHTTP || fStatusCode == kTransportError; }
   };

private:
   std::unique_ptr<RHTTPCredentials> fCredentials;
   void *fHandle = nullptr; ///< the CURL easy handle corresponding to this connection
   /// If set to zero, automatically adjust: try with all given ranges and as long as the number of ranges is too large,
   /// half it. If set to zero and automatic reduction of the number of requests is necessary, the number of requests
   /// that works will be saved for further requests with this object.
   std::size_t fMaxNRangesPerReqest = 0;
   std::string fEscapedUrl;              ///< The URL provided in the constructor escaped according to standard rules
   std::unique_ptr<char[]> fErrorBuffer; ///< For use by libcurl
   /// Total number of attempts for a request, including the first one. Set to 1 to disable retrying.
   unsigned int fMaxRetryAttempts = 3;
   /// Delay before the first retry; subsequent retries back off exponentially from it. Zero retries
   /// immediately, which is what the tests use.
   unsigned int fRetryBaseDelayMs = 100;

   void SetupErrorBuffer();
   void SetOptions();
   void ResetHandle();
   void Perform(RStatus &status);
   /// Sleep before attempt number `attempt` (one-based, so never called with 0) of a request to `what`,
   /// backing off exponentially with jitter unless the server asked for a specific delay.
   void WaitBeforeRetry(unsigned int attempt, const RStatus &status, const char *what) const;

public:
   /// Returned by SendHeadReq() if the HTTP response contains no content-length header
   static constexpr std::uint64_t kUnknownSize = static_cast<std::uint64_t>(-1);

   /// Caller-provided byte-range of the remote resource together with a pointer to a buffer.
   struct RUserRange {
      unsigned char *fDestination = nullptr;
      std::uint64_t fOffset = 0;
      std::size_t fLength = 0;
      /// Usually equal to fLength for a successful call unless range goes out of the size of the remote resource
      std::size_t fNBytesRecv = 0;

      bool operator<(const RUserRange &other) const { return fOffset < other.fOffset; }
   };

   explicit RCurlConnection(const std::string &url);
   ~RCurlConnection();
   RCurlConnection(const RCurlConnection &other) = delete;
   RCurlConnection &operator=(const RCurlConnection &other) = delete;
   RCurlConnection(RCurlConnection &&other);
   RCurlConnection &operator=(RCurlConnection &&other);

   /// Used for testing
   static int GetCurlVersion();

   void SetCredentialsFromEnvironment();
   void SetCredentials(const RS3Credentials &credentials);
   void ClearCredentials();
   EHTTPCredentialsType GetCredentialsType() const;

   /// Retargets this connection to `url`, reusing the underlying handle so curl can keep the connection
   /// to the same host alive across requests. Call before a Send*Req to address a different object.
   /// Returns an error if the URL cannot be set on the handle.
   RResult<void> SetUrl(const std::string &url);
   /// Checks if the resource exists and if it does, return the value of the content-length header as size
   RStatus SendHeadReq(std::uint64_t &remoteSize);
   /// Reads the given ranges from the remote resource. The ranges can be in any order and also overlapping. They
   /// will be transformed in optimized HTTP ranges for a multi-range request. Ranges past the resource size are
   /// valid (but won't receive any data). No limit on the number of ranges; if fMaxNRangesPerReqest is zero,
   /// a valid batching of requests into multiple multi-range requests takes place automatically.
   /// The fNBytesRecv member of the ranges is only well-defined on success.
   RStatus SendRangesReq(std::size_t N, RUserRange *ranges);
   /// Uploads data to the URL using an HTTP PUT request.
   RStatus SendPutReq(const unsigned char *data, std::size_t length);

   const std::string &GetEscapedUrl() const { return fEscapedUrl; }

   void SetMaxNRangesPerRequest(std::size_t val) { fMaxNRangesPerReqest = val; }
   std::size_t GetMaxNRangesPerRequest() const { return fMaxNRangesPerReqest; }

   /// Total attempts per request, including the first. One disables retrying.
   void SetMaxRetryAttempts(unsigned int val) { fMaxRetryAttempts = std::max(1u, val); }
   unsigned int GetMaxRetryAttempts() const { return fMaxRetryAttempts; }

   /// Delay before the first retry, in milliseconds. Zero makes retries immediate.
   void SetRetryBaseDelayMs(unsigned int val) { fRetryBaseDelayMs = val; }
   unsigned int GetRetryBaseDelayMs() const { return fRetryBaseDelayMs; }
};

} // namespace Internal
} // namespace ROOT

#endif

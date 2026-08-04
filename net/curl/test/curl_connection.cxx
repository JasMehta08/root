#include "gtest/gtest.h"

#include "ROOT/RCurlConnection.hxx"

#include "TServerSocket.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

/// Return a lower-cased copy of the input string.
static std::string ToLower(std::string s)
{
   std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
   return s;
}

/// Accept a PUT request: read headers + body, optionally respond to Expect: 100-continue, send 200 OK.
static void TaskRecvPut(TServerSocket *serverSocket, std::string *requestHeaders, std::string *requestBody)
{
   requestHeaders->clear();
   requestBody->clear();
   auto sock = serverSocket->Accept();

   const char *eof = "\r\n\r\n";
   const std::size_t eofLen = strlen(eof);
   std::size_t nextInEof = 0;
   char c;
   while (sock->RecvRaw(&c, 1)) {
      requestHeaders->push_back(c);
      if (c == eof[nextInEof]) {
         if (++nextInEof == eofLen)
            break;
      } else {
         nextInEof = 0;
      }
   }

   // If the client sent Expect: 100-continue, respond with HTTP 100 before reading the body
   std::string headersLower = ToLower(*requestHeaders);
   if (headersLower.find("expect: 100-continue") != std::string::npos) {
      const char *continueResponse = "HTTP/1.1 100 Continue\r\n\r\n";
      sock->SendRaw(continueResponse, strlen(continueResponse));
   }

   // Parse content-length (case-insensitive)
   std::size_t contentLength = 0;
   auto pos = headersLower.find("content-length: ");
   if (pos != std::string::npos) {
      auto valStart = pos + strlen("content-length: ");
      auto valEnd = headersLower.find("\r\n", valStart);
      contentLength = std::stoul(headersLower.substr(valStart, valEnd - valStart));
   }

   if (contentLength > 0) {
      requestBody->resize(contentLength);
      sock->RecvRaw(&(*requestBody)[0], contentLength);
   }

   const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
   sock->SendRaw(response, strlen(response));
   sock->Close();
}

static void TaskRecv(TServerSocket *serverSocket, std::string *request)
{
   request->clear();
   auto sock = serverSocket->Accept();

   const char *eof = "\r\n\r\n";
   const std::size_t eofLen = strlen(eof);
   std::size_t nextInEof = 0;
   char c;
   while (sock->RecvRaw(&c, 1)) {
      request->push_back(c);
      if (c == eof[nextInEof]) {
         if (++nextInEof == eofLen)
            break;
      } else {
         nextInEof = 0;
      }
   }

   sock->Close();
}

TEST(RCurlConnection, Cred)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::string url =
      std::string("http://") + sock.GetLocalInetAddress().GetHostAddress() + ":" + std::to_string(sock.GetLocalPort());

   std::string request;
   std::thread threadRecv(TaskRecv, &sock, &request);

   ROOT::Internal::RCurlConnection conn(url);
   EXPECT_EQ(ROOT::Internal::EHTTPCredentialsType::kNone, conn.GetCredentialsType());
   // This test inspects the request headers, not the outcome of the transfer: TaskRecv closes the socket
   // without replying, which curl reports as a transport error. That is retryable by default, and the
   // retry would then hang waiting on a mock that only serves one connection.
   conn.SetMaxRetryAttempts(1);

   std::uint64_t remoteSize;
   conn.SendHeadReq(remoteSize);

   threadRecv.join();
   EXPECT_EQ(std::string::npos, request.find("\r\nAuthorization: "));

   conn.SetCredentials(ROOT::Internal::RS3Credentials{"a", "b", ""});
   EXPECT_EQ(ROOT::Internal::EHTTPCredentialsType::kS3, conn.GetCredentialsType());
   threadRecv = std::thread(TaskRecv, &sock, &request);
   conn.SendHeadReq(remoteSize);
   threadRecv.join();
   EXPECT_NE(std::string::npos, request.find("\r\nAuthorization: "));

   conn.ClearCredentials();
   EXPECT_EQ(ROOT::Internal::EHTTPCredentialsType::kNone, conn.GetCredentialsType());
   threadRecv = std::thread(TaskRecv, &sock, &request);
   conn.SendHeadReq(remoteSize);
   threadRecv.join();
   EXPECT_EQ(std::string::npos, request.find("\r\nAuthorization: "));
}

TEST(RCurlConnection, Put)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::string url =
      std::string("http://") + sock.GetLocalInetAddress().GetHostAddress() + ":" + std::to_string(sock.GetLocalPort());

   const unsigned char payload[] = "Hello, S3!";
   const std::size_t payloadLen = sizeof(payload) - 1; // exclude null terminator

   std::string headers;
   std::string body;
   std::thread threadRecv(TaskRecvPut, &sock, &headers, &body);

   ROOT::Internal::RCurlConnection conn(url);
   auto status = conn.SendPutReq(payload, payloadLen);

   threadRecv.join();
   EXPECT_TRUE(static_cast<bool>(status));
   EXPECT_EQ(0u, headers.find("PUT "));

   // Normalize headers to lower-case for case-insensitive matching
   std::string headersLower = ToLower(headers);
   auto clPos = headersLower.find("content-length: " + std::to_string(payloadLen));
   ASSERT_NE(std::string::npos, clPos) << "content-length header not found in request";

   EXPECT_EQ(std::string(reinterpret_cast<const char *>(payload), payloadLen), body);
}

TEST(RCurlConnection, SetUrlThenPut)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::string baseUrl =
      std::string("http://") + sock.GetLocalInetAddress().GetHostAddress() + ":" + std::to_string(sock.GetLocalPort());

   const unsigned char payload[] = "object body";
   const std::size_t payloadLen = sizeof(payload) - 1; // exclude null terminator

   std::string headers;
   std::string body;
   std::thread threadRecv(TaskRecvPut, &sock, &headers, &body);

   // The connection is created with the base URL; SetUrl retargets it to a per-request URL (the
   // mechanism that lets one connection be reused across many objects on the same host).
   ROOT::Internal::RCurlConnection conn(baseUrl);
   auto urlStatus = conn.SetUrl(baseUrl + "/myobject/42");
   ASSERT_TRUE(static_cast<bool>(urlStatus));
   auto status = conn.SendPutReq(payload, payloadLen);

   threadRecv.join();
   EXPECT_TRUE(static_cast<bool>(status));
   EXPECT_EQ(0u, headers.find("PUT /myobject/42 ")) << headers.substr(0, 40);
   EXPECT_EQ(std::string(reinterpret_cast<const char *>(payload), payloadLen), body);
}

/// GET (range read) after PUT on the same handle — verifies that WRITEFUNCTION is set correctly
/// in SendRangesReq after a PUT cleared it.
TEST(RCurlConnection, GetAfterPut)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::string url =
      std::string("http://") + sock.GetLocalInetAddress().GetHostAddress() + ":" + std::to_string(sock.GetLocalPort());

   // First: do a PUT
   const unsigned char putPayload[] = "put-data";
   const std::size_t putPayloadLen = sizeof(putPayload) - 1;

   std::string putHeaders;
   std::string putBody;
   std::thread threadRecvPut(TaskRecvPut, &sock, &putHeaders, &putBody);

   ROOT::Internal::RCurlConnection conn(url);
   auto putStatus = conn.SendPutReq(putPayload, putPayloadLen);

   threadRecvPut.join();
   EXPECT_TRUE(static_cast<bool>(putStatus));
   EXPECT_EQ(0u, putHeaders.find("PUT "));

   // Second: do a GET (SendRangesReq) on the same handle.
   // The server sends a plain 200 response with the body "response-from-get".
   const std::string expectedBody = "response-from-get";
   std::string getHeaders;
   auto taskRecvGet = [&](TServerSocket *serverSocket) {
      getHeaders.clear();
      auto s = serverSocket->Accept();

      const char *eof = "\r\n\r\n";
      const std::size_t eofLen = strlen(eof);
      std::size_t nextInEof = 0;
      char c;
      while (s->RecvRaw(&c, 1)) {
         getHeaders.push_back(c);
         if (c == eof[nextInEof]) {
            if (++nextInEof == eofLen)
               break;
         } else {
            nextInEof = 0;
         }
      }

      std::string response =
         "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(expectedBody.size()) + "\r\n\r\n" + expectedBody;
      s->SendRaw(response.data(), response.size());
      s->Close();
   };
   std::thread threadRecvGet(taskRecvGet, &sock);

   std::vector<unsigned char> readBuf(expectedBody.size(), 0);
   ROOT::Internal::RCurlConnection::RUserRange range;
   range.fDestination = readBuf.data();
   range.fOffset = 0;
   range.fLength = expectedBody.size();
   auto getStatus = conn.SendRangesReq(1, &range);

   threadRecvGet.join();
   EXPECT_TRUE(static_cast<bool>(getStatus));
   EXPECT_EQ(0u, getHeaders.find("GET "));
   EXPECT_EQ(expectedBody.size(), range.fNBytesRecv);
   std::string received(reinterpret_cast<char *>(readBuf.data()), range.fNBytesRecv);
   EXPECT_EQ(expectedBody, received);
}

/// PUT with a payload larger than libcurl's internal Expect: 100-continue threshold (1 MB since curl 7.69).
/// Verifies that the server-side 100 Continue handshake works and all bytes arrive correctly.
TEST(RCurlConnection, PutLargeExpect100)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::string url =
      std::string("http://") + sock.GetLocalInetAddress().GetHostAddress() + ":" + std::to_string(sock.GetLocalPort());

   // 2 MB payload with a known repeating pattern
   const std::size_t payloadLen = 2 * 1024 * 1024;
   std::vector<unsigned char> payload(payloadLen);
   for (std::size_t i = 0; i < payloadLen; ++i)
      payload[i] = static_cast<unsigned char>(i & 0xFF);

   std::string headers;
   std::string body;
   std::thread threadRecv(TaskRecvPut, &sock, &headers, &body);

   ROOT::Internal::RCurlConnection conn(url);
   auto status = conn.SendPutReq(payload.data(), payloadLen);

   threadRecv.join();
   EXPECT_TRUE(static_cast<bool>(status));
   EXPECT_EQ(0u, headers.find("PUT "));

   std::string headersLower = ToLower(headers);
   EXPECT_NE(std::string::npos, headersLower.find("expect: 100-continue"))
      << "large upload should include Expect: 100-continue header";
   EXPECT_NE(std::string::npos, headersLower.find("content-length: " + std::to_string(payloadLen)));
   ASSERT_EQ(payloadLen, body.size());
   EXPECT_EQ(0, memcmp(body.data(), payload.data(), payloadLen));
}

// ==================== Retry tests ====================

// These drive the retry logic by scripting what the server replies with on each successive connection.
// The mock closes the socket after every reply, so curl opens a new connection per attempt and the number
// of accepted connections is exactly the number of attempts made.

namespace {

/// One scripted reply. `fHeaders` holds the header lines after the status line, each terminated by CRLF;
/// composing them by hand keeps the tests explicit about what the server sends (in particular, a HEAD
/// reply advertises a Content-Length without a body).
struct RScriptedReply {
   int fCode = 200;
   std::string fHeaders;
   std::string fBody;
   /// Close the connection without replying at all, which curl reports as a transport-level failure
   bool fHangUp = false;
};

/// Serve one scripted reply per accepted connection, counting the requests that arrive and recording
/// their bodies so that a retried upload can be checked for completeness.
void TaskServeScript(TServerSocket *serverSocket, const std::vector<RScriptedReply> *script, std::atomic<int> *reqCount,
                     std::vector<std::string> *bodies)
{
   for (const auto &reply : *script) {
      auto sock = serverSocket->Accept();
      if (!sock || sock == reinterpret_cast<TSocket *>(-1))
         return;

      std::string headers;
      const char *eof = "\r\n\r\n";
      const std::size_t eofLen = strlen(eof);
      std::size_t nextInEof = 0;
      char c;
      while (sock->RecvRaw(&c, 1)) {
         headers.push_back(c);
         if (c == eof[nextInEof]) {
            if (++nextInEof == eofLen)
               break;
         } else {
            nextInEof = 0;
         }
      }
      ++(*reqCount);

      const std::string headersLower = ToLower(headers);
      if (headersLower.find("expect: 100-continue") != std::string::npos) {
         const char *continueResponse = "HTTP/1.1 100 Continue\r\n\r\n";
         sock->SendRaw(continueResponse, strlen(continueResponse));
      }

      std::string body;
      if (auto pos = headersLower.find("content-length: "); pos != std::string::npos) {
         auto valStart = pos + strlen("content-length: ");
         auto valEnd = headersLower.find("\r\n", valStart);
         const auto contentLength = std::stoul(headersLower.substr(valStart, valEnd - valStart));
         if (contentLength > 0) {
            body.resize(contentLength);
            sock->RecvRaw(&body[0], contentLength);
         }
      }
      if (bodies)
         bodies->push_back(body);

      if (!reply.fHangUp) {
         const std::string response = "HTTP/1.1 " + std::to_string(reply.fCode) + " Scripted\r\n" + reply.fHeaders +
                                      "Connection: close\r\n\r\n" + reply.fBody;
         sock->SendRaw(response.data(), response.size());
      }
      sock->Close();
   }
}

/// The URL addressing an already-bound loopback server socket. Takes a non-const reference because
/// TServerSocket's accessors are not const-qualified.
std::string LocalUrl(TServerSocket &sock)
{
   return std::string("http://") + sock.GetLocalInetAddress().GetHostAddress() + ":" +
          std::to_string(sock.GetLocalPort());
}

} // anonymous namespace

TEST(RCurlConnectionRetry, HeadRetriedOn503)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::vector<RScriptedReply> script = {{503, "Content-Length: 0\r\n", "", false},
                                               {200, "Content-Length: 42\r\n", "", false}};
   std::atomic<int> reqCount{0};
   std::thread server(TaskServeScript, &sock, &script, &reqCount, nullptr);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetRetryBaseDelayMs(0);

   std::uint64_t remoteSize = 0;
   auto status = conn.SendHeadReq(remoteSize);
   server.join();

   EXPECT_TRUE(static_cast<bool>(status)) << status.fStatusMsg;
   EXPECT_EQ(42u, remoteSize);
   EXPECT_EQ(2, reqCount.load()) << "the 503 should have been retried exactly once";
}

TEST(RCurlConnectionRetry, ForbiddenNotRetried)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::vector<RScriptedReply> script = {{403, "Content-Length: 0\r\n", "", false}};
   std::atomic<int> reqCount{0};
   std::thread server(TaskServeScript, &sock, &script, &reqCount, nullptr);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetRetryBaseDelayMs(0);

   std::uint64_t remoteSize = 0;
   auto status = conn.SendHeadReq(remoteSize);
   server.join();

   // Waiting cannot turn a rejected request into an accepted one, so it must fail on the first attempt.
   EXPECT_EQ(ROOT::Internal::RCurlConnection::RStatus::kForbidden, status.fStatusCode);
   EXPECT_FALSE(status.IsRetryable());
   EXPECT_EQ(1, reqCount.load());
}

TEST(RCurlConnectionRetry, GivesUpAfterMaxAttempts)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::vector<RScriptedReply> script(3, {503, "Content-Length: 0\r\n", "", false});
   std::atomic<int> reqCount{0};
   std::thread server(TaskServeScript, &sock, &script, &reqCount, nullptr);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetRetryBaseDelayMs(0);
   conn.SetMaxRetryAttempts(3);

   std::uint64_t remoteSize = 0;
   auto status = conn.SendHeadReq(remoteSize);
   server.join();

   EXPECT_EQ(ROOT::Internal::RCurlConnection::RStatus::kTransientHTTP, status.fStatusCode);
   EXPECT_EQ(503, status.fHttpCode);
   EXPECT_EQ(3, reqCount.load()) << "three attempts in total, i.e. two retries";
}

TEST(RCurlConnectionRetry, TransportFailureRetried)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   // The first connection is accepted and then dropped without a reply, which is what a connection reset
   // in the middle of a request looks like to curl.
   const std::vector<RScriptedReply> script = {{0, "", "", true}, {200, "Content-Length: 5\r\n", "", false}};
   std::atomic<int> reqCount{0};
   std::thread server(TaskServeScript, &sock, &script, &reqCount, nullptr);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetRetryBaseDelayMs(0);

   std::uint64_t remoteSize = 0;
   auto status = conn.SendHeadReq(remoteSize);
   server.join();

   EXPECT_TRUE(static_cast<bool>(status)) << status.fStatusMsg;
   EXPECT_EQ(5u, remoteSize);
   EXPECT_EQ(2, reqCount.load());
}

TEST(RCurlConnectionRetry, RangeRequestRetriedWithCorrectData)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::string content = "AAAAABBBBBCCCCC"; // 15 bytes
   const std::vector<RScriptedReply> script = {
      {503, "Content-Length: 0\r\n", "", false},
      {200, "Content-Length: " + std::to_string(content.size()) + "\r\n", content, false}};
   std::atomic<int> reqCount{0};
   std::thread server(TaskServeScript, &sock, &script, &reqCount, nullptr);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetRetryBaseDelayMs(0);

   unsigned char bufFirst[5] = {};
   unsigned char bufLast[5] = {};
   ROOT::Internal::RCurlConnection::RUserRange ranges[2];
   ranges[0].fDestination = bufFirst;
   ranges[0].fOffset = 0;
   ranges[0].fLength = 5;
   ranges[1].fDestination = bufLast;
   ranges[1].fOffset = 10;
   ranges[1].fLength = 5;

   auto status = conn.SendRangesReq(2, ranges);
   server.join();

   // The point of the test: the retry has to start from a clean transfer state. If it resumed from where
   // the failed attempt left off, the bytes would land at the wrong offsets instead of failing outright.
   EXPECT_TRUE(static_cast<bool>(status)) << status.fStatusMsg;
   EXPECT_EQ(2, reqCount.load());
   EXPECT_EQ(5u, ranges[0].fNBytesRecv);
   EXPECT_EQ(5u, ranges[1].fNBytesRecv);
   EXPECT_EQ("AAAAA", std::string(reinterpret_cast<const char *>(bufFirst), 5));
   EXPECT_EQ("CCCCC", std::string(reinterpret_cast<const char *>(bufLast), 5));
}

TEST(RCurlConnectionRetry, PutResendsWholeBody)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::vector<RScriptedReply> script = {{503, "Content-Length: 0\r\n", "", false},
                                               {200, "Content-Length: 0\r\n", "", false}};
   std::atomic<int> reqCount{0};
   std::vector<std::string> bodies;
   std::thread server(TaskServeScript, &sock, &script, &reqCount, &bodies);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetRetryBaseDelayMs(0);

   const unsigned char payload[] = "Hello, retry!";
   const std::size_t payloadLen = sizeof(payload) - 1;
   auto status = conn.SendPutReq(payload, payloadLen);
   server.join();

   EXPECT_TRUE(static_cast<bool>(status)) << status.fStatusMsg;
   EXPECT_EQ(2, reqCount.load());
   // The retried upload must send the payload again in full, not the remainder of the failed attempt.
   ASSERT_EQ(2u, bodies.size());
   const std::string expected(reinterpret_cast<const char *>(payload), payloadLen);
   EXPECT_EQ(expected, bodies[0]);
   EXPECT_EQ(expected, bodies[1]);
}

TEST(RCurlConnectionRetry, TooManyRangesNotRetried)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::vector<RScriptedReply> script = {{400, "Content-Length: 0\r\n", "", false}};
   std::atomic<int> reqCount{0};
   std::thread server(TaskServeScript, &sock, &script, &reqCount, nullptr);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetRetryBaseDelayMs(0);

   unsigned char buffer[8] = {};
   ROOT::Internal::RCurlConnection::RUserRange range;
   range.fDestination = buffer;
   range.fOffset = 0;
   range.fLength = sizeof(buffer);

   auto status = conn.SendRangesReq(1, &range);
   server.join();

   // A 400 means "too many ranges in this request", which SendRangesReq answers by halving the batch
   // size rather than by waiting. Repeating the identical request would not help, so it must not be
   // retried; with a single range there is nothing left to halve either.
   EXPECT_EQ(ROOT::Internal::RCurlConnection::RStatus::kTooManyRanges, status.fStatusCode);
   EXPECT_FALSE(status.IsRetryable());
   EXPECT_EQ(1, reqCount.load());
}

TEST(RCurlConnectionRetry, StatusCarriesDiagnostics)
{
   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::vector<RScriptedReply> script = {{503, "Content-Length: 0\r\n", "", false}};
   std::atomic<int> reqCount{0};
   std::thread server(TaskServeScript, &sock, &script, &reqCount, nullptr);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetMaxRetryAttempts(1); // no retry, so the single scripted reply is the final answer

   std::uint64_t remoteSize = 0;
   auto status = conn.SendHeadReq(remoteSize);
   server.join();

   // curl itself succeeded; it is the HTTP layer that failed, and the code has to survive into the status
   // so that an error message can name it.
   EXPECT_EQ(503, status.fHttpCode);
   EXPECT_EQ(0, status.fCurlCode);
   EXPECT_EQ(1, reqCount.load());
}

TEST(RCurlConnectionRetry, RetryAfterHeaderIsHonoured)
{
   // curl_easy_header() arrived in libcurl 7.83; below that the header is not read and the retry falls
   // back to exponential backoff, which this test cannot distinguish from ignoring it.
   if (ROOT::Internal::RCurlConnection::GetCurlVersion() < 0x078300)
      GTEST_SKIP() << "libcurl too old to read the Retry-After header";

   TServerSocket sock(0, false, TServerSocket::kDefaultBacklog, -1, ESocketBindOption::kInaddrLoopback);
   const std::vector<RScriptedReply> script = {{503, "Content-Length: 0\r\nRetry-After: 1\r\n", "", false},
                                               {200, "Content-Length: 7\r\n", "", false}};
   std::atomic<int> reqCount{0};
   std::thread server(TaskServeScript, &sock, &script, &reqCount, nullptr);

   ROOT::Internal::RCurlConnection conn(LocalUrl(sock));
   conn.SetRetryBaseDelayMs(0); // without Retry-After this would retry immediately

   const auto start = std::chrono::steady_clock::now();
   std::uint64_t remoteSize = 0;
   auto status = conn.SendHeadReq(remoteSize);
   const auto elapsed = std::chrono::steady_clock::now() - start;
   server.join();

   EXPECT_TRUE(static_cast<bool>(status)) << status.fStatusMsg;
   EXPECT_EQ(2, reqCount.load());
   // The base delay is zero, so any appreciable wait can only come from the server's Retry-After.
   EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 900);
}

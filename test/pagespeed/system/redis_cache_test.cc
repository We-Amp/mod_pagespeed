/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Unit-test the redis interface.

#include "pagespeed/system/redis_cache.h"

#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "apr_network_io.h"  // NOLINT
#include "base/logging.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/posix_timer.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/kernel/util/simple_stats.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/cache/cache_test_base.h"
#include "test/pagespeed/kernel/thread/worker_test_base.h"
#include "test/pagespeed/system/tcp_connection_for_testing.h"
#include "test/pagespeed/system/tcp_server_thread_for_testing.h"

namespace net_instaweb {

namespace {
static const int kReconnectionDelayMs = 10;
static const int kTimeoutUs = 100 * Timer::kMsUs;
// Operation timeout for the caches that talk to the real Redis server from
// the environment. A round trip to it (INFO in RedisCacheTest.GetStatus,
// say) can exceed the 100 ms kTimeoutUs on a loaded host or in a sanitizer
// build; the tight value is only needed by the scripted timeout tests.
static const int kRealServerTimeoutUs = 2 * Timer::kSecondUs;
static const int kDatabaseIndex[] = {0, 1};
static const int kTTLSec = -1;
static const char kSomeKey[] = "SomeKey";
static const char kSomeValue[] = "SomeValue";
}  // namespace

using testing::HasSubstr;

// Answers each request it reads with the next scripted reply, in order,
// then closes the connection. The first request is the SELECT sent on
// connect.
class RedisScriptedServerThread : public TcpServerThreadForTesting {
 public:
  RedisScriptedServerThread(apr_port_t listen_port, ThreadSystem* thread_system,
                            std::vector<GoogleString> replies)
      : TcpServerThreadForTesting(listen_port, "redis_scripted_server",
                                  thread_system),
        replies_(std::move(replies)) {}

  ~RedisScriptedServerThread() override { ShutDown(); }

 private:
  void HandleClientConnection(apr_socket_t* sock) override {
    for (const GoogleString& reply : replies_) {
      char buf[4096];
      apr_size_t size = sizeof(buf);
      if (apr_socket_recv(sock, buf, &size) != APR_SUCCESS) {
        break;
      }
      for (size_t sent = 0; sent < reply.size();) {
        apr_size_t chunk = reply.size() - sent;
        if (apr_socket_send(sock, reply.data() + sent, &chunk) != APR_SUCCESS) {
          break;
        }
        sent += chunk;
      }
    }
    apr_socket_close(sock);
  }

  const std::vector<GoogleString> replies_;
};

// TODO(yeputons): refactor this class with MemcachedCacheTest, see details in
// memcached_cache_test.cc
class RedisCacheTest : public CacheTestBase {
 protected:
  RedisCacheTest()
      : thread_system_(Platform::CreateThreadSystem()),
        statistics_(thread_system_.get()),
        timer_(new NullMutex, 0),
        redis_port_env_(0),
        redis_host_env_("localhost") {
    RedisCache::InitStats(&statistics_);
  }

  bool PrepareRedisOrSkip() {
    const char* portString = getenv("REDIS_PORT");
    int port;
    if (portString == nullptr || !StringToInt(portString, &port)) {
      LOG(ERROR) << "RedisCache tests are skipped because env var "
                 << "$REDIS_PORT is not set to an integer. Set that "
                 << "to the port number where redis is running to "
                 << "enable the tests. See install/run_program_with_redis.sh";
      return false;
    }

    redis_port_env_ = port;
    const char* hostString = getenv("REDIS_HOST");
    if (hostString != nullptr) {
      redis_host_env_ = hostString;
    }

    {
      TcpConnectionForTesting conn;
      CHECK(conn.Connect(redis_host_env_.c_str(), port))
          << "Cannot connect to Redis server";
      conn.Send("FLUSHALL\r\n");
      CHECK_EQ("+OK\r\n", conn.ReadLineCrLf());
    }
    return true;
  }

  void InitRedisWithCustomDatabaseIndex(const int database_index) {
    cache_.emplace_back(new RedisCache(
        redis_host_env_, redis_port_env_, thread_system_.get(), &handler_,
        &timer_, kReconnectionDelayMs, kRealServerTimeoutUs, &statistics_,
        database_index, kTTLSec));
    cache_.back()->StartUp();
  }

  void InitRedisWithCustomServer() {
    cache_.emplace_back(
        new RedisCache("localhost", custom_server_port_, thread_system_.get(),
                       &handler_, &timer_, kReconnectionDelayMs, kTimeoutUs,
                       &statistics_, kDatabaseIndex[0], kTTLSec));
  }

  void InitRedisWithUnreachableServer() {
    // Try to connect to some definitely unreachable host.
    // 192.0.2.0/24 is reserved for documentation purposes in RFC5737 and no
    // machine should ever be routable in that subnet.
    cache_.emplace_back(new RedisCache("192.0.2.1", 12345, thread_system_.get(),
                                       &handler_, &timer_, kReconnectionDelayMs,
                                       kTimeoutUs, &statistics_,
                                       kDatabaseIndex[0], kTTLSec));
  }

  static void SetUpTestCase() {
    apr_initialize();
    TcpServerThreadForTesting::PickListenPortOnce(&custom_server_port_);
    CHECK_NE(custom_server_port_, 0);
  }

  template <class ServerThread>
  bool StartCustomServer() {
    WaitForCustomServerShutdown();
    custom_server_ = std::make_unique<ServerThread>(custom_server_port_,
                                                    thread_system_.get());
    if (!custom_server_->Start()) {
      return false;
    }
    // Wait while server starts and check its port
    return custom_server_->GetListeningPort() == custom_server_port_;
  }

  void WaitForCustomServerShutdown() { custom_server_.reset(); }

  bool StartScriptedServer(std::vector<GoogleString> replies) {
    WaitForCustomServerShutdown();
    custom_server_ = std::make_unique<RedisScriptedServerThread>(
        custom_server_port_, thread_system_.get(), std::move(replies));
    if (!custom_server_->Start()) {
      return false;
    }
    return custom_server_->GetListeningPort() == custom_server_port_;
  }

  // Sends one GET to a scripted server that redirects it to a refusing
  // port and then answers the slot-mapping query with `cluster_slots`.
  void GetRedirectedThenMapped(const GoogleString& cluster_slots) {
    InitRedisWithCustomServer();
    ASSERT_TRUE(StartScriptedServer(
        {"+OK\r\n", "-MOVED 1 127.0.0.1:1\r\n", cluster_slots}));
    cache_[0]->StartUp();
    CheckNotFound(kSomeKey);
    WaitForCustomServerShutdown();
  }

  size_t ClusterMappingCount() {
    ScopedMutex lock(cache_[0]->cluster_map_lock_.get());
    return cache_[0]->cluster_mappings_.size();
  }

  size_t ConnectionCount() {
    ScopedMutex lock(cache_[0]->connections_lock_.get());
    return cache_[0]->connections_.size();
  }

  static size_t MaxClusterConnections() {
    return RedisCache::kMaxClusterConnections;
  }

  static void TearDownTestCase() { apr_terminate(); }

  CacheInterface* Cache() override { return cache_[0].get(); }

  ThreadSynchronizer* GetThreadSynchronizer() {
    return cache_[0]->GetThreadSynchronizerForTesting();
  }

  std::vector<std::unique_ptr<RedisCache>> cache_;
  std::unique_ptr<ThreadSystem> thread_system_;
  SimpleStats statistics_;
  MockTimer timer_;
  GoogleMessageHandler handler_;

  std::unique_ptr<TcpServerThreadForTesting> custom_server_;
  static apr_port_t custom_server_port_;
  int redis_port_env_;
  GoogleString redis_host_env_;
};

apr_port_t RedisCacheTest::custom_server_port_ = 0;

// Simple flow of putting in an item, getting it, deleting it.
TEST_F(RedisCacheTest, PutGetDelete) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);

  CheckPut("Name", "Value");
  CheckGet("Name", "Value");
  CheckNotFound("Another Name");

  CheckPut("Name", "NewValue");
  CheckGet("Name", "NewValue");

  CheckDelete("Name");
  CheckNotFound("Name");

  // We're not running against redis cluster, so we don't expect to ever be
  // redirected, and we should never ask for cluster slots.
  EXPECT_EQ(0, cache_[0]->Redirections());
  EXPECT_EQ(0, cache_[0]->ClusterSlotsFetches());
}

// Make sure curly braces in keys aren't treated specially.
TEST_F(RedisCacheTest, CurlyBraces) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);

  CheckPut("{1}NameA", "Value1A");
  CheckPut("{2}NameB", "Value2B");
  CheckPut("{2}NameC", "Value2C");

  CheckGet("{1}NameA", "Value1A");
  CheckGet("{2}NameB", "Value2B");
  CheckGet("{2}NameC", "Value2C");
}

// And spaces
TEST_F(RedisCacheTest, Spaces) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);

  CheckPut("1 NameA", "Value1A");
  CheckPut("2 NameB", "Value2B");
  CheckPut("2 NameC", "Value2C");

  CheckGet("1 NameA", "Value1A");
  CheckGet("2 NameB", "Value2B");
  CheckGet("2 NameC", "Value2C");
}

TEST_F(RedisCacheTest, MultiGet) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);
  TestMultiGet();  // Test from CacheTestBase is just fine.
}

TEST_F(RedisCacheTest, BasicInvalid) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);

  // Check that we honor callback veto on validity.
  CheckPut("nameA", "valueA");
  CheckPut("nameB", "valueB");
  CheckGet("nameA", "valueA");
  CheckGet("nameB", "valueB");
  set_invalid_value("valueA");
  CheckNotFound("nameA");
  CheckGet("nameB", "valueB");
}

TEST_F(RedisCacheTest, GetStatus) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);

  GoogleString status;
  cache_[0]->GetStatus(&status);

  // Check that some reasonable info is present.
  EXPECT_THAT(status, HasSubstr(cache_[0]->ServerDescription()));
  EXPECT_THAT(status, HasSubstr("redis_version:"));
  EXPECT_THAT(status, HasSubstr("connected_clients:"));
  EXPECT_THAT(status, HasSubstr("tcp_port:"));
  EXPECT_THAT(status, HasSubstr("used_memory:"));
}

// Two following tests are identical and ensure that no keys are leaked between
// tests through shared running Redis server.
TEST_F(RedisCacheTest, TestsAreIsolated1) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);

  CheckNotFound(kSomeKey);
  CheckPut(kSomeKey, kSomeValue);
}

TEST_F(RedisCacheTest, TestsAreIsolated2) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);

  CheckNotFound(kSomeKey);
  CheckPut(kSomeKey, kSomeValue);
}

// Test to check multiple redis database(with different index)
TEST_F(RedisCacheTest, TestMultipleDatabases) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(kDatabaseIndex[0]);
  InitRedisWithCustomDatabaseIndex(kDatabaseIndex[1]);

  CheckPut("key1", "value1");
  // adding same key to second database
  CheckPut(cache_[1].get(), "key1", "value2");

  // checking key entries from databases
  CheckGet("key1", "value1");
  CheckGet(cache_[1].get(), "key1", "value2");

  CheckDelete("key1");

  // checking key deleted from first database
  CheckNotFound("key1");

  // check same key present in second database
  CheckGet(cache_[1].get(), "key1", "value2");
}

class RedisGetRespondingServerThread : public TcpServerThreadForTesting {
 public:
  RedisGetRespondingServerThread(apr_port_t listen_port,
                                 ThreadSystem* thread_system)
      : TcpServerThreadForTesting(listen_port, "redis_get_answering_server",
                                  thread_system) {}

  ~RedisGetRespondingServerThread() override { ShutDown(); }

 private:
  void HandleClientConnection(apr_socket_t* sock) override {
    // See http://redis.io/topics/protocol for details. Request is an array of
    // two bulk strings, answer for GET is a single bulk string.

    // during redis cache startup, Select database command is fired
    // being the first command, it is captured by the mock redis server
    static const char kSelectRequest[] =
        "*2\r\n"
        "$6\r\nSELECT\r\n"
        "$1\r\n0\r\n";
    static const char kSelectAnswer[] = "+OK\r\n";
    apr_size_t answer_size_select = STATIC_STRLEN(kSelectAnswer);

    char requestBuf[STATIC_STRLEN(kSelectRequest) + 1];
    apr_size_t recvSize = sizeof(requestBuf) - 1;

    apr_socket_recv(sock, requestBuf, &recvSize);
    EXPECT_EQ(STATIC_STRLEN(kSelectRequest), recvSize);
    requestBuf[recvSize] = 0;
    EXPECT_STREQ(kSelectRequest, requestBuf);

    apr_socket_send(sock, kSelectAnswer, &answer_size_select);

    static const char kRequest[] =
        "*2\r\n"
        "$3\r\nGET\r\n"
        "$7\r\nSomeKey\r\n";
    static const char kAnswer[] = "$9\r\nSomeValue\r\n";
    apr_size_t answer_size = STATIC_STRLEN(kAnswer);

    char buf[STATIC_STRLEN(kRequest) + 1];
    apr_size_t size = sizeof(buf) - 1;

    apr_socket_recv(sock, buf, &size);
    EXPECT_EQ(STATIC_STRLEN(kRequest), size);
    buf[size] = 0;
    EXPECT_STREQ(kRequest, buf);

    apr_socket_send(sock, kAnswer, &answer_size);
    apr_socket_close(sock);
  }
};

TEST_F(RedisCacheTest, ReconnectsInstantly) {
  InitRedisWithCustomServer();
  ASSERT_TRUE(StartCustomServer<RedisGetRespondingServerThread>());
  cache_[0]->StartUp();

  CheckGet(kSomeKey, kSomeValue);
  // Server closes connection after processing one request, but cache does not
  // know about that yet.
  WaitForCustomServerShutdown();
  EXPECT_TRUE(Cache()->IsHealthy());

  // Client should not reconnect as it learns about disconnection only when it
  // tries to run the command.
  ASSERT_TRUE(StartCustomServer<RedisGetRespondingServerThread>());
  CheckNotFound(kSomeKey);

  // First reconnection attempt should happen right away.
  EXPECT_TRUE(Cache()->IsHealthy());  // Allow reconnection.
  CheckGet(kSomeKey, kSomeValue);
}

TEST_F(RedisCacheTest, ReconnectsUntilSuccessWithTimeout) {
  InitRedisWithCustomServer();
  ASSERT_TRUE(StartCustomServer<RedisGetRespondingServerThread>());
  cache_[0]->StartUp();

  CheckGet(kSomeKey, kSomeValue);
  // Server closes connection after processing one request, but cache does not
  // know about that yet.
  WaitForCustomServerShutdown();
  EXPECT_TRUE(Cache()->IsHealthy());

  // Let client know that we're disconnected by trying to read.
  CheckNotFound(kSomeKey);

  // Try to reconnect right away after failure.
  EXPECT_TRUE(Cache()->IsHealthy());  // Reconnection is allowed...
  CheckNotFound(kSomeKey);            // ...but it fails.

  // Second attempt, should not reconnect before timeout.
  ASSERT_TRUE(StartCustomServer<RedisGetRespondingServerThread>());
  timer_.AdvanceMs(kReconnectionDelayMs - 1);
  EXPECT_FALSE(Cache()->IsHealthy());  // Reconnection is not allowed.
  CheckNotFound(kSomeKey);

  // Should reconnect after timeout passes.
  timer_.AdvanceMs(1);
  EXPECT_TRUE(Cache()->IsHealthy());  // Reconnection is allowed.
  CheckGet(kSomeKey, kSomeValue);
}

TEST_F(RedisCacheTest, ReconnectsIfStartUpFailed) {
  InitRedisWithCustomServer();
  cache_[0]->StartUp();

  // Client already knows that connection failed.
  EXPECT_FALSE(Cache()->IsHealthy());
  CheckNotFound(kSomeKey);

  // Should not reconnect before timeout.
  ASSERT_TRUE(StartCustomServer<RedisGetRespondingServerThread>());
  timer_.AdvanceMs(kReconnectionDelayMs - 1);
  EXPECT_FALSE(Cache()->IsHealthy());  // Reconnection is not allowed.
  CheckNotFound(kSomeKey);

  // Should reconnect after timeout passes.
  timer_.AdvanceMs(1);
  EXPECT_TRUE(Cache()->IsHealthy());  // Reconnection is allowed.
  CheckGet(kSomeKey, kSomeValue);
}

TEST_F(RedisCacheTest, DoesNotReconnectAfterShutdown) {
  if (!PrepareRedisOrSkip()) {
    return;
  }
  InitRedisWithCustomDatabaseIndex(0);

  CheckPut(kSomeKey, kSomeValue);
  CheckGet(kSomeKey, kSomeValue);
  EXPECT_TRUE(Cache()->IsHealthy());

  Cache()->ShutDown();
  timer_.AdvanceMs(kReconnectionDelayMs);

  EXPECT_FALSE(Cache()->IsHealthy());  // Reconnection is not allowed.
  CheckNotFound(kSomeKey);
}

// One CLUSTER SLOTS entry: slots [start, end] served by host:port.
GoogleString ClusterSlotsEntry(const GoogleString& start,
                               const GoogleString& end,
                               const GoogleString& host,
                               const GoogleString& port) {
  return StrCat("*3\r\n:", start, "\r\n:", end, "\r\n*2\r\n$",
                IntegerToString(host.size()), "\r\n", host, "\r\n:", port,
                "\r\n");
}

TEST_F(RedisCacheTest, RedirectionToAnInvalidPortIsIgnored) {
  InitRedisWithCustomServer();
  ASSERT_TRUE(StartScriptedServer({"+OK\r\n", "-MOVED 1 127.0.0.1:0\r\n"}));
  cache_[0]->StartUp();
  CheckNotFound(kSomeKey);
  EXPECT_EQ(1u, ConnectionCount());
}

TEST_F(RedisCacheTest, RedirectionToAnEmptyHostIsIgnored) {
  InitRedisWithCustomServer();
  ASSERT_TRUE(StartScriptedServer({"+OK\r\n", "-MOVED 1 :6379\r\n"}));
  cache_[0]->StartUp();
  CheckNotFound(kSomeKey);
  EXPECT_EQ(1u, ConnectionCount());
}

TEST_F(RedisCacheTest, ClusterSlotsWithAValidRangeIsUsed) {
  GetRedirectedThenMapped(
      StrCat("*1\r\n", ClusterSlotsEntry("0", "16383", "127.0.0.1", "2")));
  EXPECT_EQ(1u, ClusterMappingCount());
}

TEST_F(RedisCacheTest, ClusterSlotsWithAnOversizedSlotIsRejected) {
  // 2^32 + 5 would wrap to 5 in an int.
  GetRedirectedThenMapped(
      StrCat("*1\r\n", ClusterSlotsEntry("0", "4294967301", "127.0.0.1", "2")));
  EXPECT_EQ(0u, ClusterMappingCount());
}

TEST_F(RedisCacheTest, ClusterSlotsWithANegativeSlotIsRejected) {
  GetRedirectedThenMapped(
      StrCat("*1\r\n", ClusterSlotsEntry("-1", "5", "127.0.0.1", "2")));
  EXPECT_EQ(0u, ClusterMappingCount());
}

TEST_F(RedisCacheTest, ClusterSlotsWithPortZeroIsRejected) {
  GetRedirectedThenMapped(
      StrCat("*1\r\n", ClusterSlotsEntry("0", "5", "127.0.0.1", "0")));
  EXPECT_EQ(0u, ClusterMappingCount());
}

TEST_F(RedisCacheTest, ClusterSlotsWithAnOversizedPortIsRejected) {
  GetRedirectedThenMapped(
      StrCat("*1\r\n", ClusterSlotsEntry("0", "5", "127.0.0.1", "65536")));
  EXPECT_EQ(0u, ClusterMappingCount());
}

TEST_F(RedisCacheTest, ClusterSlotsWithAnEmptyHostIsRejected) {
  GetRedirectedThenMapped(
      StrCat("*1\r\n", ClusterSlotsEntry("0", "5", "", "2")));
  EXPECT_EQ(0u, ClusterMappingCount());
}

TEST_F(RedisCacheTest, ClusterConnectionsAreCapped) {
  // A slot mapping naming more servers than the cap: one slot each, every
  // one on its own port.
  const size_t servers = MaxClusterConnections() + 8;
  GoogleString reply = StrCat("*", IntegerToString(servers), "\r\n");
  for (size_t i = 0; i < servers; ++i) {
    const GoogleString slot = IntegerToString(i);
    StrAppend(&reply, ClusterSlotsEntry(slot, slot, "127.0.0.1",
                                        IntegerToString(20000 + i)));
  }
  GetRedirectedThenMapped(reply);
  EXPECT_LE(ConnectionCount(), MaxClusterConnections());
}

TEST_F(RedisCacheTest, RejectedClusterSlotsOpenNoConnections) {
  // A slot mapping naming as many servers as the cap does not fit next to
  // the two connections already open, so it is rejected as a whole.
  const size_t servers = MaxClusterConnections();
  GoogleString oversized = StrCat("*", IntegerToString(servers), "\r\n");
  for (size_t i = 0; i < servers; ++i) {
    const GoogleString slot = IntegerToString(i);
    StrAppend(&oversized, ClusterSlotsEntry(slot, slot, "127.0.0.1",
                                            IntegerToString(20000 + i)));
  }
  const GoogleString valid =
      StrCat("*1\r\n", ClusterSlotsEntry("0", "16383", "127.0.0.1", "2"));
  InitRedisWithCustomServer();
  ASSERT_TRUE(
      StartScriptedServer({"+OK\r\n", "-MOVED 1 127.0.0.1:1\r\n", oversized,
                           "-MOVED 1 127.0.0.1:1\r\n", valid}));
  cache_[0]->StartUp();
  CheckNotFound(kSomeKey);
  EXPECT_EQ(0u, ClusterMappingCount());
  // The main connection and the redirection target.
  EXPECT_EQ(2u, ConnectionCount());

  // A later valid mapping is still accepted.
  CheckNotFound(kSomeKey);
  WaitForCustomServerShutdown();
  EXPECT_EQ(1u, ClusterMappingCount());
  EXPECT_EQ(3u, ConnectionCount());
}

// This server always waits until connection is received to avoid race
// condition between server destruction and accepting connection (like in
// ShutDownDuringConnection). Other servers do not do that because tests
// actually rely on their answers to client.
class RedisNotRespondingServerThread : public TcpServerThreadForTesting {
 public:
  RedisNotRespondingServerThread(apr_port_t listen_port,
                                 ThreadSystem* thread_system)
      : TcpServerThreadForTesting(listen_port, "redis_not_responding_server",
                                  thread_system),
        connection_received_(thread_system) {}

  ~RedisNotRespondingServerThread() override {
    connection_received_.Wait();
    ShutDown();
  }

 protected:
  void HandleClientConnection(apr_socket_t* sock) override {
    // Do nothing, socket will be closed in destructor
    connection_received_.Notify();
  }

 private:
  WorkerTestBase::SyncPoint connection_received_;
};

// This server always waits until connection is received to avoid race
// condition between server destruction and accepting connection (like in
// ShutDownDuringConnection). Other servers do not do that because tests
// actually rely on their answers to client.
class RedisNotRespondingOperationTimeoutServerThread
    : public TcpServerThreadForTesting {
 public:
  RedisNotRespondingOperationTimeoutServerThread(apr_port_t listen_port,
                                                 ThreadSystem* thread_system)
      : TcpServerThreadForTesting(listen_port, "redis_not_responding_server",
                                  thread_system),
        connection_received_(thread_system) {}

  ~RedisNotRespondingOperationTimeoutServerThread() override {
    connection_received_.Wait();
    ShutDown();
  }

 protected:
  void HandleClientConnection(apr_socket_t* sock) override {
    // Do nothing, socket will be closed in destructor

    // during redis cache startup, Select database command is fired
    // being the first command, it is captured by the mock redis server
    static const char kSelectRequest[] =
        "*2\r\n"
        "$6\r\nSELECT\r\n"
        "$1\r\n0\r\n";
    static const char kSelectAnswer[] = "+OK\r\n";
    apr_size_t answer_size_select = STATIC_STRLEN(kSelectAnswer);

    char requestBuf[STATIC_STRLEN(kSelectRequest) + 1];
    apr_size_t recvSize = sizeof(requestBuf) - 1;

    apr_socket_recv(sock, requestBuf, &recvSize);
    EXPECT_EQ(STATIC_STRLEN(kSelectRequest), recvSize);
    requestBuf[recvSize] = 0;
    EXPECT_STREQ(kSelectRequest, requestBuf);

    apr_socket_send(sock, kSelectAnswer, &answer_size_select);
    connection_received_.Notify();
  }

 private:
  WorkerTestBase::SyncPoint connection_received_;
};

// These constants are for timeout tests.
namespace {
// Experiments showed that I/O functions on Linux may sometimes time out
// slightly earlier than configured. It does not look like precision or
// rounding error; waking up recv() 2ms earlier has probability around 0.7%.
// That is partially leveraged by the fact that we have a bunch of code around
// I/O in RedisCache, but probability is still non-zero (0.05%). Probability
// of 1ms gap in RedisCacheOperationTimeouTest was around 5% at the time it was
// written. On a loaded shared host timer jitter is larger still (a 100ms
// timeout was measured at 93.5ms). The floor only guards against the operation
// returning early, so a generous allowance of 20ms costs nothing: the tests
// still fail if the operation returns well before its timeout.
const int kTimedOutOperationMinTimeUs = kTimeoutUs - 20 * Timer::kMsUs;

// Upper gap is bigger because taking more time than time out is expected.
// On top of that, a loaded CI host or a sanitizer build can stall the process
// for several hundred milliseconds inside the timed operation, so the old
// 50 ms allowance still flaked. A full second absorbs those stalls while the
// tests still fail if the operation hangs.
const int kTimedOutOperationMaxTimeUs = kTimeoutUs + Timer::kSecondUs;

// We want timeout to be significantly greater than measuring gap.
static_assert(kTimeoutUs >= 60 * Timer::kMsUs,
              "kTimeoutUs is smaller than measuring gap");
}  // namespace

TEST_F(RedisCacheTest, ConnectionTimeout) {
  InitRedisWithUnreachableServer();
  PosixTimer timer;
  int64 started_at_us = timer.NowUs();
  cache_[0]->StartUp();  // Should try to connect as well.
  int64 waited_for_us = timer.NowUs() - started_at_us;
  EXPECT_FALSE(cache_[0]->IsHealthy());
  EXPECT_GE(waited_for_us, kTimedOutOperationMinTimeUs);
  EXPECT_LE(waited_for_us, kTimedOutOperationMaxTimeUs);
}

class GetRequestThread : public ThreadSystem::Thread {
 public:
  GetRequestThread(CacheInterface* cache, ThreadSystem* system)
      : ThreadSystem::Thread(system, "get_request_thread",
                             ThreadSystem::kJoinable),
        cache_(cache),
        check_called_(false) {}

  void Run() override { cache_->Get(kSomeKey, &callback_); }

  void CheckLookupResult(CacheInterface::KeyState expected_state) {
    check_called_ = true;
    Join();
    EXPECT_TRUE(callback_.called());
    EXPECT_EQ(callback_.state(), expected_state);
    if (expected_state == CacheInterface::kAvailable) {
      EXPECT_STREQ(callback_.value().Value(), kSomeValue);
    }
  }

  ~GetRequestThread() override {
    EXPECT_TRUE(check_called_)
        << "CheckLookupResult() was not called on GetRequestThread";
  }

 private:
  CacheInterface* cache_;
  CacheInterface::SynchronousCallback callback_;
  bool check_called_;
};

TEST_F(RedisCacheTest, IsHealthyDoesNotBlock) {
  InitRedisWithCustomServer();
  StartCustomServer<RedisGetRespondingServerThread>();
  cache_[0]->StartUp();

  // enabling thread synchronizer after cache start up because
  // cache startup fires redis command to select redis database
  // and this execution interferes with the test thread synchronization
  GetThreadSynchronizer()->EnableForPrefix("RedisCommand.After");

  GetRequestThread thread(Cache(), thread_system_.get());
  ASSERT_TRUE(thread.Start());
  GetThreadSynchronizer()->Wait("RedisCommand.After.Signal");

  // Check that IsHealthy() returns even when operation is in progress.
  Cache()->IsHealthy();

  GetThreadSynchronizer()->Signal("RedisCommand.After.Wait");
  thread.CheckLookupResult(CacheInterface::kAvailable);
}

TEST_F(RedisCacheTest, ConnectionFastFail) {
  InitRedisWithCustomServer();
  StartCustomServer<RedisGetRespondingServerThread>();
  GetThreadSynchronizer()->EnableForPrefix("RedisConnect.After");
  cache_[0]->StartUp(/* connect_now */ false);

  EXPECT_TRUE(Cache()->IsHealthy());
  GetRequestThread thread(Cache(), thread_system_.get());
  ASSERT_TRUE(thread.Start());
  GetThreadSynchronizer()->Wait("RedisConnect.After.Signal");

  EXPECT_FALSE(Cache()->IsHealthy());  // Connection is in progress.

  // Check that Get() returns immediately. Two times becase first call may
  // theoretically override cache's state (real bug).
  for (int iter = 1; iter <= 2; iter++) {
    CheckNotFound(kSomeKey);
  }

  GetThreadSynchronizer()->Signal("RedisConnect.After.Wait");
  thread.CheckLookupResult(CacheInterface::kAvailable);

  // Now that thread is terminated, connection is established, cache should be
  // healthy again.
  EXPECT_TRUE(Cache()->IsHealthy());
}

TEST_F(RedisCacheTest, ShutDownDuringConnection) {
  InitRedisWithCustomServer();
  StartCustomServer<RedisNotRespondingServerThread>();
  GetThreadSynchronizer()->EnableForPrefix("RedisConnect.After");
  cache_[0]->StartUp(/* connect_now */ false);

  EXPECT_TRUE(Cache()->IsHealthy());
  GetRequestThread thread(Cache(), thread_system_.get());
  ASSERT_TRUE(thread.Start());
  GetThreadSynchronizer()->Wait("RedisConnect.After.Signal");

  Cache()->ShutDown();

  GetThreadSynchronizer()->Signal("RedisConnect.After.Wait");
  thread.CheckLookupResult(CacheInterface::kNotFound);

  // Cache potentially may want to reconnect, so we advance timer to ensure that
  // it does not want to.
  timer_.AdvanceMs(kReconnectionDelayMs);
  EXPECT_FALSE(Cache()->IsHealthy());
}

// All RedisCacheOperationTimeoutTests start with a cache connected to a server
// which accepts single connection and does not answer until test is finished.
// The test calls a single command. If the timeout handling is correct, it times
// out and the test terminates correctly. If the timeout handling is not
// correct, the test hangs.
class RedisCacheOperationTimeoutTest : public RedisCacheTest {
 protected:
  void SetUp() override {
    InitRedisWithCustomServer();
    CHECK(StartCustomServer<RedisNotRespondingOperationTimeoutServerThread>());
    cache_[0]->StartUp();
  }

  // Times only the operation under test and checks that it did not return
  // before the operation timeout fired and did not run wildly past it. The
  // process can stall for hundreds of milliseconds on a loaded CI host or in
  // a sanitizer build; measuring anything besides the operation itself bills
  // those stalls to the operation under test.
  template <class Operation>
  void CheckOperationTimesOut(Operation operation) {
    PosixTimer timer;
    int64 started_at_us = timer.NowUs();
    operation();
    int64 waited_for_us = timer.NowUs() - started_at_us;
    EXPECT_GE(waited_for_us, kTimedOutOperationMinTimeUs);
    EXPECT_LE(waited_for_us, kTimedOutOperationMaxTimeUs);
  }
};

TEST_F(RedisCacheOperationTimeoutTest, Get) {
  CheckOperationTimesOut([this] { CheckNotFound("Key"); });
}

// TODO(yeputons): test MultiGet when it's a single command, not sequenced GET()

TEST_F(RedisCacheOperationTimeoutTest, Put) {
  CheckOperationTimesOut([this] { CheckPut("Key", "Value"); });
}

TEST_F(RedisCacheOperationTimeoutTest, Delete) {
  CheckOperationTimesOut([this] { CheckDelete("Key"); });
}

}  // namespace net_instaweb

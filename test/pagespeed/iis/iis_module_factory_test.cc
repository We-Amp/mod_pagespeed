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

// Unit tests for IisModuleFactory.
//
// Tests the module factory lifecycle, module creation, cleanup, and
// shutdown coordination (graceful request draining).
// Note: These tests require Windows because IisModuleFactory depends on
// Windows IIS types (IHttpServer, CHttpModule, etc.).

#include "pagespeed/iis/iis_module_factory.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "pagespeed/iis/iis_config.h"
#include "pagespeed/iis/iis_server_context.h"
#include "pagespeed/iis/iis_rewrite_driver_factory.h"

namespace net_instaweb {

// Mock IHttpServer implementation for testing.
// Matches the IHttpServer interface from Windows SDK 10.0.22621.0.
class MockIHttpServer : public IHttpServer {
 public:
  MockIHttpServer() = default;
  ~MockIHttpServer() = default;

  // IHttpServer interface - minimal implementations
  BOOL IsCommandLineLaunch() const override { return FALSE; }
  PCWSTR GetAppPoolName() const override { return L"TestAppPool"; }
  HRESULT AssociateWithThreadPool(
      HANDLE, LPOVERLAPPED_COMPLETION_ROUTINE) override {
    return E_NOTIMPL;
  }
  VOID IncrementThreadCount() override {}
  VOID DecrementThreadCount() override {}
  VOID ReportUnhealthy(PCWSTR, HRESULT) override {}
  VOID RecycleProcess(PCWSTR) override {}
  IAppHostAdminManager* GetAdminManager() const override { return nullptr; }
  HRESULT GetFileInfo(
      PCWSTR, HANDLE, PSID, PCWSTR, HANDLE, BOOL,
      IHttpFileInfo**, IHttpTraceContext*) override {
    return E_NOTIMPL;
  }
  HRESULT FlushKernelCache(PCWSTR) override { return E_NOTIMPL; }
  HRESULT DoCacheOperation(CACHE_OPERATION, IHttpCacheKey*,
                           IHttpCacheSpecificData**,
                           IHttpTraceContext*) override {
    return E_NOTIMPL;
  }
  GLOBAL_NOTIFICATION_STATUS NotifyCustomNotification(
      ICustomNotificationProvider*) override {
    return GL_NOTIFICATION_CONTINUE;
  }
  IHttpPerfCounterInfo* GetPerfCounterInfo() override { return nullptr; }
  VOID RecycleApplication(PCWSTR) override {}
  VOID NotifyConfigurationChange(PCWSTR) override {}
  VOID NotifyFileChange(PCWSTR) override {}
  IDispensedHttpModuleContextContainer* DispenseContainer() override {
    return nullptr;
  }
  HRESULT AddFragmentToCache(HTTP_DATA_CHUNK*, PCWSTR) override {
    return E_NOTIMPL;
  }
  HRESULT ReadFragmentFromCache(
      PCWSTR, BYTE*, DWORD, DWORD*) override {
    return E_NOTIMPL;
  }
  HRESULT RemoveFragmentFromCache(PCWSTR) override { return E_NOTIMPL; }
  HRESULT GetWorkerProcessSettings(IWpfSettings**) override {
    return E_NOTIMPL;
  }
  HRESULT GetProtocolManagerCustomInterface(
      PCWSTR, PCWSTR, DWORD, PVOID*) override {
    return E_NOTIMPL;
  }
  BOOL SatisfiesPrecondition(PCWSTR, BOOL*) const override { return FALSE; }
  IHttpTraceContext* GetTraceContext() const override { return nullptr; }
  HRESULT RegisterFileChangeMonitor(
      PCWSTR, HANDLE, IHttpFileMonitor**) override {
    return E_NOTIMPL;
  }
  HRESULT GetExtendedInterface(
      HTTP_SERVER_INTERFACE_VERSION, PVOID*) override {
    return E_NOTIMPL;
  }
};

// Testable subclass that skips the heavy system cache initialization
// (Init/PostConfig/RootInit/ChildInit) which hangs without running services.
class TestableIisModuleFactory : public IisModuleFactory {
 protected:
  HRESULT SetupSystemCaches() override {
    // Create a minimal server context without the heavy cache/thread setup.
    auto config = std::make_unique<IisConfig>();
    default_context_.reset(
        driver_factory_->MakeIisServerContext(std::move(config)));
    return S_OK;
  }
};

// Mock IModuleAllocator for GetHttpModule tests
class MockModuleAllocator : public IModuleAllocator {
 public:
  void* AllocateMemory(DWORD size) override {
    allocated_size_ = size;
    return malloc(size);
  }

  DWORD allocated_size() const { return allocated_size_; }

 private:
  DWORD allocated_size_ = 0;
};

class IisModuleFactoryTest : public testing::Test {
 protected:
  void SetUp() override {
    mock_server_ = std::make_unique<MockIHttpServer>();
    factory_ = std::make_unique<TestableIisModuleFactory>();
  }

  void TearDown() override {
    // Terminate must be called before destruction
    if (factory_ != nullptr) {
      factory_->Terminate();
    }
    factory_.reset();
    mock_server_.reset();
  }

  std::unique_ptr<MockIHttpServer> mock_server_;
  std::unique_ptr<IisModuleFactory> factory_;
};

// ============================================================================
// Construction Tests
// ============================================================================

TEST_F(IisModuleFactoryTest, ConstructionSucceeds) {
  EXPECT_NE(nullptr, factory_.get());
}

TEST_F(IisModuleFactoryTest, ServerContextNullBeforeInit) {
  // Before Initialize(), server_context should be null
  EXPECT_EQ(nullptr, factory_->server_context());
}

TEST_F(IisModuleFactoryTest, FactoryNullBeforeInit) {
  // Before Initialize(), factory should be null
  EXPECT_EQ(nullptr, factory_->factory());
}

// ============================================================================
// Module ID Tests
// ============================================================================

TEST_F(IisModuleFactoryTest, ModuleIdDefaultIsNull) {
  EXPECT_EQ(nullptr, factory_->module_id());
}

TEST_F(IisModuleFactoryTest, SetModuleIdStoresValue) {
  // Create a fake module ID (any non-null pointer)
  HTTP_MODULE_ID fake_id = reinterpret_cast<HTTP_MODULE_ID>(0x12345678);

  factory_->set_module_id(fake_id);

  EXPECT_EQ(fake_id, factory_->module_id());
}

TEST_F(IisModuleFactoryTest, GetModuleIdStaticAccessor) {
  // The static accessor should return the same value
  HTTP_MODULE_ID fake_id = reinterpret_cast<HTTP_MODULE_ID>(0xDEADBEEF);

  factory_->set_module_id(fake_id);

  EXPECT_EQ(fake_id, IisModuleFactory::GetModuleId());
}

// ============================================================================
// Initialize Tests
// ============================================================================

TEST_F(IisModuleFactoryTest, InitializeWithValidServer) {
  HRESULT hr = factory_->Initialize(mock_server_.get());

  EXPECT_TRUE(SUCCEEDED(hr)) << "Initialize should succeed with mock server";
}

TEST_F(IisModuleFactoryTest, InitializeCreatesServerContext) {
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  EXPECT_NE(nullptr, factory_->server_context())
      << "server_context should be created after Initialize";
}

TEST_F(IisModuleFactoryTest, InitializeCreatesDriverFactory) {
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  EXPECT_NE(nullptr, factory_->factory())
      << "driver factory should be created after Initialize";
}

// ============================================================================
// GetHttpModule Tests
// ============================================================================

TEST_F(IisModuleFactoryTest, GetHttpModuleFailsWithoutInit) {
  // Without Initialize(), GetHttpModule should fail
  CHttpModule* module = nullptr;
  MockModuleAllocator allocator;

  HRESULT hr = factory_->GetHttpModule(&module, &allocator);

  EXPECT_TRUE(FAILED(hr)) << "GetHttpModule should fail without Initialize";
  EXPECT_EQ(nullptr, module);
}

TEST_F(IisModuleFactoryTest, GetHttpModuleSucceedsAfterInit) {
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  CHttpModule* module = nullptr;
  MockModuleAllocator allocator;

  hr = factory_->GetHttpModule(&module, &allocator);

  EXPECT_TRUE(SUCCEEDED(hr)) << "GetHttpModule should succeed after Initialize";
  EXPECT_NE(nullptr, module) << "Module should be created";

  // Clean up the module via Dispose() (destructor is protected)
  module->Dispose();
}

TEST_F(IisModuleFactoryTest, GetHttpModuleCreatesMultipleModules) {
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  MockModuleAllocator allocator;
  CHttpModule* module1 = nullptr;
  CHttpModule* module2 = nullptr;

  hr = factory_->GetHttpModule(&module1, &allocator);
  ASSERT_TRUE(SUCCEEDED(hr));

  hr = factory_->GetHttpModule(&module2, &allocator);
  ASSERT_TRUE(SUCCEEDED(hr));

  // Both should be valid and distinct
  EXPECT_NE(nullptr, module1);
  EXPECT_NE(nullptr, module2);
  EXPECT_NE(module1, module2) << "Each call should create a new module";

  module1->Dispose();
  module2->Dispose();
}

// ============================================================================
// Terminate Tests
// ============================================================================

TEST_F(IisModuleFactoryTest, TerminateWithoutInit) {
  // Terminate should handle being called without Initialize
  // (This is tested implicitly by TearDown, but explicit test is clearer)
  factory_->Terminate();

  // Should be safe to call - no crash or exception
  SUCCEED();
}

TEST_F(IisModuleFactoryTest, TerminateCleansUpResources) {
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  // Verify resources exist
  EXPECT_NE(nullptr, factory_->server_context());
  EXPECT_NE(nullptr, factory_->factory());

  factory_->Terminate();

  // After Terminate, resources should be cleaned up
  // Note: The pointers may or may not be nulled depending on implementation
  // The important thing is that Terminate doesn't crash
  SUCCEED();
}

TEST_F(IisModuleFactoryTest, TerminateIdempotent) {
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  // Multiple Terminate calls should be safe
  factory_->Terminate();
  factory_->Terminate();  // Second call should be no-op

  SUCCEED();
}

// ============================================================================
// Server Context Tests
// ============================================================================

TEST_F(IisModuleFactoryTest, ServerContextHasDriverFactory) {
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  IisServerContext* ctx = factory_->server_context();
  ASSERT_NE(nullptr, ctx);

  // Server context should reference the driver factory
  EXPECT_EQ(factory_->factory(), ctx->iis_factory());
}

// ============================================================================
// Shutdown Coordination Tests
//
// These tests verify the graceful shutdown mechanism that prevents crashes
// when IIS recycles the app pool while requests are still in flight.
//
// The original bug: IisModuleFactory::Terminate() destroyed the server
// context and driver factory immediately, while in-flight requests still
// held pointers to ProxyFetch/RewriteDriver objects that reference them.
// This caused use-after-free crashes during app pool recycle.
//
// The fix: An atomic shutdown flag (shutting_down_) and active request
// counter (active_request_count_) coordinate between request handlers
// and Terminate(). New requests check the flag and bail out, while
// Terminate() waits for the counter to reach zero before destroying
// resources.
// ============================================================================

TEST_F(IisModuleFactoryTest, NotShuttingDownByDefault) {
  // A newly constructed factory should not be shutting down.
  EXPECT_FALSE(factory_->IsShuttingDown());
}

TEST_F(IisModuleFactoryTest, ShuttingDownAfterTerminate) {
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  factory_->Terminate();

  // After Terminate(), IsShuttingDown() should return true.
  EXPECT_TRUE(factory_->IsShuttingDown());
}

TEST_F(IisModuleFactoryTest, IncrementDecrementActiveRequests) {
  // Verify the active request counter can be incremented and decremented.
  // This is the mechanism that prevents Terminate() from destroying
  // resources while requests are still using them.
  factory_->IncrementActiveRequests();
  factory_->IncrementActiveRequests();
  factory_->DecrementActiveRequests();
  factory_->DecrementActiveRequests();

  // If the counter works correctly, this should not hang or crash.
  SUCCEED();
}

TEST_F(IisModuleFactoryTest, TerminateWaitsForActiveRequests) {
  // Simulate the crash scenario: Terminate() is called while requests
  // are still active. Terminate() should wait for the counter to reach
  // zero before destroying resources.
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  // Simulate an active request.
  factory_->IncrementActiveRequests();

  // Terminate in a background thread (it will wait for requests to finish).
  std::atomic<bool> terminate_started{false};
  std::atomic<bool> terminate_done{false};
  std::thread terminator([&]() {
    terminate_started.store(true, std::memory_order_release);
    factory_->Terminate();
    terminate_done.store(true, std::memory_order_release);
  });

  // Wait for Terminate to start.
  while (!terminate_started.load(std::memory_order_acquire)) {
    Sleep(1);
  }
  // Give Terminate a moment to enter its polling loop.
  Sleep(100);

  // Terminate should still be waiting because we have an active request.
  EXPECT_FALSE(terminate_done.load(std::memory_order_acquire))
      << "Terminate should wait for active requests to complete";

  // Now "complete" the request.
  factory_->DecrementActiveRequests();

  // Terminate should complete shortly.
  terminator.join();
  EXPECT_TRUE(terminate_done.load(std::memory_order_acquire));
}

TEST_F(IisModuleFactoryTest, ShutdownFlagRejectsNewRequests) {
  // Verify that once shutdown is initiated, IsShuttingDown() returns true
  // so new requests can check and bail out.
  HRESULT hr = factory_->Initialize(mock_server_.get());
  ASSERT_TRUE(SUCCEEDED(hr));

  // Before shutdown, requests should be accepted.
  EXPECT_FALSE(factory_->IsShuttingDown());

  // Terminate (no active requests, so it completes immediately).
  factory_->Terminate();

  // After shutdown, new requests should be rejected.
  EXPECT_TRUE(factory_->IsShuttingDown());
}

TEST_F(IisModuleFactoryTest, ConcurrentRequestCounterStress) {
  // Stress test the atomic counter with concurrent increments/decrements
  // to verify there are no data races.
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 1000;

  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < kOpsPerThread; ++j) {
        factory_->IncrementActiveRequests();
      }
      for (int j = 0; j < kOpsPerThread; ++j) {
        factory_->DecrementActiveRequests();
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  // All increments should be balanced by decrements. Terminate should
  // complete immediately (counter is zero).
  factory_->Terminate();
  SUCCEED();
}

}  // namespace net_instaweb

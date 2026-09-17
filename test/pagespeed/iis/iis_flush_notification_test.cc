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

#include "pagespeed/iis/iis_flush_notification.h"

#include <cstring>

#include "gtest/gtest.h"

namespace net_instaweb {

// =============================================================================
// PageSpeedFlushNotification Tests
// =============================================================================

class PageSpeedFlushNotificationTest : public testing::Test {
 protected:
  PageSpeedFlushNotification notification_;
};

TEST_F(PageSpeedFlushNotificationTest, QueryNotificationTypeReturnsCorrectString) {
  // The notification type should return PAGESPEED_FLUSH_NOTIFICATION
  const wchar_t* type = notification_.QueryNotificationType();
  ASSERT_NE(type, nullptr);
  EXPECT_EQ(std::wcscmp(type, L"PageSpeedFlushNotification"), 0);
}

TEST_F(PageSpeedFlushNotificationTest, DefaultLastErrorIsZero) {
  // Initial state should have last_error() == 0 (S_OK on Windows)
  EXPECT_EQ(notification_.last_error(), 0);
}

TEST_F(PageSpeedFlushNotificationTest, SetErrorStatusStoresError) {
  // Set an error code and verify it's stored
  notification_.SetErrorStatus(0x80004005);  // E_FAIL
  EXPECT_EQ(notification_.last_error(), 0x80004005);
}

TEST_F(PageSpeedFlushNotificationTest, LastErrorReturnsStoredValue) {
  // Set multiple error values and verify the last one is returned
  notification_.SetErrorStatus(1);
  EXPECT_EQ(notification_.last_error(), 1);

  notification_.SetErrorStatus(42);
  EXPECT_EQ(notification_.last_error(), 42);

  notification_.SetErrorStatus(0);
  EXPECT_EQ(notification_.last_error(), 0);
}

TEST_F(PageSpeedFlushNotificationTest, MultipleErrorsOverwritePrevious) {
  // Setting a new error should overwrite the previous one
  notification_.SetErrorStatus(100);
  notification_.SetErrorStatus(200);
  notification_.SetErrorStatus(300);
  EXPECT_EQ(notification_.last_error(), 300);
}

// =============================================================================
// FlushNotificationManager Tests
// =============================================================================

class FlushNotificationManagerTest : public testing::Test {
 protected:
  FlushNotificationManager manager_;
};

TEST_F(FlushNotificationManagerTest, NotificationPendingInitiallyFalse) {
  // Initial state should have notification_pending == false
  EXPECT_FALSE(manager_.notification_pending());
}

TEST_F(FlushNotificationManagerTest, SetNotificationPendingWorks) {
  // Set notification pending to true
  manager_.set_notification_pending(true);
  EXPECT_TRUE(manager_.notification_pending());

  // Set it back to false
  manager_.set_notification_pending(false);
  EXPECT_FALSE(manager_.notification_pending());
}

TEST_F(FlushNotificationManagerTest, NotificationAccessorWorks) {
  // Get the notification instance
  PageSpeedFlushNotification* notification = manager_.notification();
  ASSERT_NE(notification, nullptr);

  // Verify we can use the notification
  const wchar_t* type = notification->QueryNotificationType();
  EXPECT_EQ(std::wcscmp(type, L"PageSpeedFlushNotification"), 0);
}

TEST_F(FlushNotificationManagerTest, NotificationAccessorReturnsSameInstance) {
  // Multiple calls should return the same instance
  PageSpeedFlushNotification* first = manager_.notification();
  PageSpeedFlushNotification* second = manager_.notification();
  EXPECT_EQ(first, second);
}

TEST_F(FlushNotificationManagerTest, FlushCountInitiallyZero) {
  // Initial state should have flush_count == 0
  EXPECT_EQ(manager_.flush_count(), 0);
}

TEST_F(FlushNotificationManagerTest, IncrementFlushCountWorks) {
  // Increment and verify
  manager_.increment_flush_count();
  EXPECT_EQ(manager_.flush_count(), 1);

  manager_.increment_flush_count();
  EXPECT_EQ(manager_.flush_count(), 2);

  manager_.increment_flush_count();
  EXPECT_EQ(manager_.flush_count(), 3);
}

TEST_F(FlushNotificationManagerTest, FlushCountAndPendingAreIndependent) {
  // Verify that flush count and pending state are independent
  EXPECT_EQ(manager_.flush_count(), 0);
  EXPECT_FALSE(manager_.notification_pending());

  manager_.increment_flush_count();
  EXPECT_EQ(manager_.flush_count(), 1);
  EXPECT_FALSE(manager_.notification_pending());

  manager_.set_notification_pending(true);
  EXPECT_EQ(manager_.flush_count(), 1);
  EXPECT_TRUE(manager_.notification_pending());

  manager_.increment_flush_count();
  EXPECT_EQ(manager_.flush_count(), 2);
  EXPECT_TRUE(manager_.notification_pending());
}

TEST_F(FlushNotificationManagerTest, NotificationErrorStateIsolated) {
  // Verify that setting error on the notification doesn't affect manager state
  PageSpeedFlushNotification* notification = manager_.notification();
  notification->SetErrorStatus(0x80004005);

  // Manager state should be unchanged
  EXPECT_FALSE(manager_.notification_pending());
  EXPECT_EQ(manager_.flush_count(), 0);

  // But notification should have the error
  EXPECT_EQ(notification->last_error(), 0x80004005);
}

// =============================================================================
// Integration Tests
// =============================================================================

class FlushNotificationIntegrationTest : public testing::Test {
 protected:
  FlushNotificationManager manager_;
};

TEST_F(FlushNotificationIntegrationTest, SimulateFlushCycle) {
  // Simulate a typical flush notification cycle:
  // 1. ProxyFetch has data ready
  // 2. Set notification pending
  // 3. Send notification (simulated)
  // 4. IIS handler processes it
  // 5. Clear pending, increment count

  EXPECT_FALSE(manager_.notification_pending());
  EXPECT_EQ(manager_.flush_count(), 0);

  // Step 1-2: Data ready, mark pending
  manager_.set_notification_pending(true);
  EXPECT_TRUE(manager_.notification_pending());

  // Step 3: Would call IHttpContext::NotifyCustomNotification here
  // (can't test that part without real IIS)

  // Step 4-5: Handler processed, clear pending and increment
  manager_.set_notification_pending(false);
  manager_.increment_flush_count();

  EXPECT_FALSE(manager_.notification_pending());
  EXPECT_EQ(manager_.flush_count(), 1);

  // Simulate multiple flushes
  for (int i = 0; i < 5; ++i) {
    manager_.set_notification_pending(true);
    // ... notification sent and handled ...
    manager_.set_notification_pending(false);
    manager_.increment_flush_count();
  }

  EXPECT_EQ(manager_.flush_count(), 6);
}

TEST_F(FlushNotificationIntegrationTest, SimulateErrorDuringFlush) {
  // Simulate an error occurring during flush notification

  PageSpeedFlushNotification* notification = manager_.notification();

  // Start flush cycle
  manager_.set_notification_pending(true);

  // Simulate error from IIS
  notification->SetErrorStatus(0x80070005);  // E_ACCESSDENIED

  // Error should be recorded
  EXPECT_EQ(notification->last_error(), 0x80070005);

  // Can still complete the cycle
  manager_.set_notification_pending(false);
  manager_.increment_flush_count();

  EXPECT_FALSE(manager_.notification_pending());
  EXPECT_EQ(manager_.flush_count(), 1);

  // Error persists on notification
  EXPECT_EQ(notification->last_error(), 0x80070005);
}

}  // namespace net_instaweb

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// The image's two registrations: which one IIS is making, the process-wide
// setup that runs once, and the main registration's module id, which a
// final-view registration must never replace.

#include "pagespeed/iis/iis_registration.h"

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

int main_tag = 0;
int final_tag = 0;
const void* const kMainId = &main_tag;
const void* const kFinalId = &final_tag;

TEST(IisRegistrationTest, TheFinalViewEntryIsKnownByItsNameOnly) {
  EXPECT_EQ(IisRegistrationKind::kFinalView,
            IisRegistrationKindForName(L"PageSpeedFinalView"));
  EXPECT_EQ(IisRegistrationKind::kFinalView,
            IisRegistrationKindForName(L"pagespeedfinalview"));
  EXPECT_EQ(IisRegistrationKind::kMain,
            IisRegistrationKindForName(L"PageSpeedModule"));
  // An operator's own name for the main entry stays the main entry.
  EXPECT_EQ(IisRegistrationKind::kMain,
            IisRegistrationKindForName(L"MyPageSpeed"));
  EXPECT_EQ(IisRegistrationKind::kMain,
            IisRegistrationKindForName(L"PageSpeedFinalView2"));
  EXPECT_EQ(IisRegistrationKind::kMain,
            IisRegistrationKindForName(L"PageSpeedFinalVie"));
  EXPECT_EQ(IisRegistrationKind::kMain, IisRegistrationKindForName(L""));
  EXPECT_EQ(IisRegistrationKind::kMain, IisRegistrationKindForName(nullptr));
}

TEST(IisRegistrationTest, ProcessSetupRunsOnce) {
  IisRegistrations registrations;
  EXPECT_TRUE(registrations.BeginProcessSetup());
  EXPECT_FALSE(registrations.BeginProcessSetup());
  EXPECT_FALSE(registrations.BeginProcessSetup());
}

TEST(IisRegistrationTest, ASecondRegistrationLeavesTheMainIdIntact) {
  // Main first, as <globalModules> lists it after an install.
  IisRegistrations registrations;
  EXPECT_TRUE(
      registrations.NoteRegistered(IisRegistrationKind::kMain, kMainId));
  EXPECT_TRUE(
      registrations.NoteRegistered(IisRegistrationKind::kFinalView, kFinalId));
  EXPECT_EQ(kMainId, registrations.main_id());
  EXPECT_EQ(kFinalId, registrations.final_view_id());
}

TEST(IisRegistrationTest, AFinalViewRegisteredFirstLeavesTheMainIdToMain) {
  IisRegistrations registrations;
  EXPECT_TRUE(
      registrations.NoteRegistered(IisRegistrationKind::kFinalView, kFinalId));
  EXPECT_EQ(nullptr, registrations.main_id());
  EXPECT_TRUE(
      registrations.NoteRegistered(IisRegistrationKind::kMain, kMainId));
  EXPECT_EQ(kMainId, registrations.main_id());
  EXPECT_EQ(kFinalId, registrations.final_view_id());
}

TEST(IisRegistrationTest, AnEntryListedTwiceIsRefused) {
  IisRegistrations registrations;
  EXPECT_TRUE(
      registrations.NoteRegistered(IisRegistrationKind::kMain, kMainId));
  EXPECT_FALSE(
      registrations.NoteRegistered(IisRegistrationKind::kMain, kFinalId));
  EXPECT_EQ(kMainId, registrations.main_id());
  EXPECT_TRUE(
      registrations.NoteRegistered(IisRegistrationKind::kFinalView, kFinalId));
  EXPECT_FALSE(
      registrations.NoteRegistered(IisRegistrationKind::kFinalView, kMainId));
  EXPECT_EQ(kFinalId, registrations.final_view_id());
  EXPECT_EQ(kMainId, registrations.main_id());
}

}  // namespace
}  // namespace net_instaweb

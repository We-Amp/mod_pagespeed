// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/rule_pack_admin_view.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "absl/strings/str_cat.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace rule_pack_view {
namespace {

namespace fs = std::filesystem;

// 2026-10-11 12:00:00 UTC.
constexpr int64_t kNoon = 1791720000000LL;

std::string Line(int64_t ts, const std::string& path, const std::string& rule,
                 const std::string& action, const std::string& reason) {
  return absl::StrCat("{\"ts\":", ts, ",\"pack\":\"edge-seo@0.1.0\",",
                      "\"sha256\":\"0123456789abcdef\",\"host\":\"t.test\",",
                      "\"path\":\"", path, "\",\"rule\":\"", rule,
                      "\",\"kind\":\"canonical\",\"mode\":\"report\",",
                      "\"action\":\"", action, "\",\"reason\":\"", reason,
                      "\",\"defect\":\"\",\"before\":\"\",\"after\":\"\"}");
}

class TailTest : public testing::Test {
 protected:
  void SetUp() override {
    const testing::TestInfo* info =
        testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::path(GTestTempDir()) / "rule_pack_admin_view" / info->name();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    path_ = (dir_ / "decisions.jsonl").string();
  }
  void Write(const std::string& contents) {
    std::ofstream(path_, std::ios::binary | std::ios::trunc) << contents;
  }
  fs::path dir_;
  std::string path_;
};

TEST_F(TailTest, AMissingFileIsAStatus) {
  Tail tail = ReadDecisionLogTail(path_, 10);
  EXPECT_EQ(Tail::Status::kMissing, tail.status);
  EXPECT_TRUE(tail.lines.empty());
}

TEST_F(TailTest, AnUnreadablePathIsAnError) {
  // A directory is not a log file.
  Tail tail = ReadDecisionLogTail(dir_.string(), 10);
  EXPECT_EQ(Tail::Status::kError, tail.status);
  EXPECT_FALSE(tail.error.empty());
}

TEST_F(TailTest, AnEmptyFileHasNoLines) {
  Write("");
  Tail tail = ReadDecisionLogTail(path_, 10);
  EXPECT_EQ(Tail::Status::kOk, tail.status);
  EXPECT_TRUE(tail.lines.empty());
}

TEST_F(TailTest, AFileShorterThanTheCapIsReadWhole) {
  Write("a\nb\nc\n");
  Tail tail = ReadDecisionLogTail(path_, 10, 1024);
  ASSERT_EQ(3u, tail.lines.size());
  EXPECT_EQ("a", tail.lines[0]);
  EXPECT_EQ("c", tail.lines[2]);
}

TEST_F(TailTest, TheLimitKeepsTheLastLines) {
  Write("a\nb\nc\nd\n");
  Tail tail = ReadDecisionLogTail(path_, 2, 1024);
  ASSERT_EQ(2u, tail.lines.size());
  EXPECT_EQ("c", tail.lines[0]);
  EXPECT_EQ("d", tail.lines[1]);
}

TEST_F(TailTest, ALongFileIsReadFromTheEndAndTheCutLineIsDropped) {
  // 20 lines of "line-NN\n" (8 bytes): 160 bytes. A 20-byte window starts
  // inside a line.
  std::string contents;
  for (int i = 0; i < 20; ++i) {
    contents += absl::StrCat("line-", i < 10 ? "0" : "", i, "\n");
  }
  Write(contents);
  Tail tail = ReadDecisionLogTail(path_, 100, 20);
  ASSERT_EQ(Tail::Status::kOk, tail.status);
  ASSERT_EQ(2u, tail.lines.size());
  EXPECT_EQ("line-18", tail.lines[0]);
  EXPECT_EQ("line-19", tail.lines[1]);
}

TEST_F(TailTest, AWindowStartingOnALineBoundaryKeepsItsFirstLine) {
  // 4 lines of 8 bytes; a 16-byte window starts exactly at line 2.
  Write("line-00\nline-01\nline-02\nline-03\n");
  Tail tail = ReadDecisionLogTail(path_, 100, 16);
  ASSERT_EQ(2u, tail.lines.size());
  EXPECT_EQ("line-02", tail.lines[0]);
}

TEST_F(TailTest, AFileExactlyTheCapLongIsReadWhole) {
  Write("line-00\nline-01\n");  // 16 bytes
  Tail tail = ReadDecisionLogTail(path_, 100, 16);
  ASSERT_EQ(2u, tail.lines.size());
  EXPECT_EQ("line-00", tail.lines[0]);
}

TEST_F(TailTest, AFileOneByteOverTheCapLosesItsFirstLine) {
  Write("line-00\nline-01\n");  // 16 bytes
  Tail tail = ReadDecisionLogTail(path_, 100, 15);
  ASSERT_EQ(1u, tail.lines.size());
  EXPECT_EQ("line-01", tail.lines[0]);
}

TEST_F(TailTest, AWindowInsideOneLineHasNoLines) {
  Write(std::string(100, 'x') + "\n");
  Tail tail = ReadDecisionLogTail(path_, 100, 10);
  EXPECT_EQ(Tail::Status::kOk, tail.status);
  EXPECT_TRUE(tail.lines.empty());
}

TEST(LimitTest, IsClamped) {
  EXPECT_EQ(kDefaultLimit, ClampLimit(""));
  EXPECT_EQ(kDefaultLimit, ClampLimit("abc"));
  EXPECT_EQ(kDefaultLimit, ClampLimit("0"));
  EXPECT_EQ(kDefaultLimit, ClampLimit("-5"));
  EXPECT_EQ(7u, ClampLimit("7"));
  EXPECT_EQ(kMaxLimit, ClampLimit("1000"));
  EXPECT_EQ(kMaxLimit, ClampLimit("5000"));
  EXPECT_EQ(kMaxLimit, ClampLimit("99999999999999999999"));
  EXPECT_EQ(kDefaultLimit, ClampLimit("abcdefghijkl"));
  EXPECT_EQ(kDefaultLimit, ClampLimit("12345678901x"));
}

Page LoadedPage() {
  Page page;
  page.scope = "vhost";
  page.host = "t.test";
  page.filter_enabled = true;
  page.pack_file = "/etc/packs/edge.json";
  page.mode = "report";
  page.state = "loaded";
  page.last_event_ms = kNoon;
  page.pack.present = true;
  page.pack.id = "edge-seo";
  page.pack.version = "0.1.0";
  page.pack.publisher = "Example";
  page.pack.sha256 = std::string(64, 'a');
  page.pack.warnings = {"a warning"};
  page.have_counters = true;
  page.counters = {{"rule_pack_pages_evaluated", 3}};
  page.log_date = "2026-10-11";
  page.tail.status = Tail::Status::kOk;
  page.tail.lines = {Line(kNoon, "/a", "canonical-keep", "insert", "missing"),
                     Line(kNoon + 1000, "/b", "title-keep", "none", "present"),
                     Line(kNoon + 2000, "/<script>alert(1)</script>",
                          "canonical-keep", "insert", "missing")};
  return page;
}

TEST(RenderTest, ShowsThePackTheCountersAndTheRowsNewestFirst) {
  const std::string html = RenderHtml(LoadedPage());
  EXPECT_NE(std::string::npos, html.find("edge-seo"));
  EXPECT_NE(std::string::npos, html.find("0.1.0"));
  EXPECT_NE(std::string::npos, html.find("Example"));
  EXPECT_NE(std::string::npos, html.find("title=\"" + std::string(64, 'a') +
                                         "\">" + std::string(16, 'a')));
  EXPECT_NE(std::string::npos, html.find("rule_pack_pages_evaluated"));
  EXPECT_NE(std::string::npos, html.find("a warning"));
  EXPECT_NE(std::string::npos, html.find("2026-10-11 12:00:00 UTC"));
  // Newest first.
  EXPECT_LT(html.find("alert(1)"), html.find(">/b<"));
  EXPECT_LT(html.find(">/b<"), html.find(">/a<"));
  EXPECT_NE(std::string::npos, html.find("3 matching, newest first"));
}

TEST(RenderTest, LinksTheCatalogueAndTheDocs) {
  const Page page = LoadedPage();
  const std::string html = RenderHtml(page);
  EXPECT_NE(std::string::npos,
            html.find("<a href=\"https://modpagespeed.com/packs/\">"
                      "Available packs: modpagespeed.com/packs</a>"));
  EXPECT_NE(
      std::string::npos,
      html.find("<a href=\"https://modpagespeed.com/docs/rule-packs/\">"));
  EXPECT_EQ(std::string::npos, html.find("?engine="));
  // Plain text output is unchanged.
  EXPECT_EQ(std::string::npos, RenderRaw(page).find("modpagespeed.com"));
}

TEST(RenderTest, EscapesValuesFromTheLogAndTheRequest) {
  const std::string html = RenderHtml(LoadedPage());
  EXPECT_EQ(std::string::npos, html.find("<script>"));
  EXPECT_NE(std::string::npos,
            html.find("/&lt;script&gt;alert(1)&lt;/script&gt;"));

  Page page = LoadedPage();
  page.filter.rule = "\"><script>x</script>";
  const std::string filtered = RenderHtml(page);
  EXPECT_EQ(std::string::npos, filtered.find("<script>"));
  EXPECT_NE(std::string::npos, filtered.find("&quot;&gt;&lt;script&gt;x"));
}

TEST(RenderTest, FiltersApplyAfterParsing) {
  Page page = LoadedPage();
  page.filter.rule = "title-keep";
  std::vector<Row> rows = SelectRows(page);
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ("/b", rows[0].path);

  page.filter = Filter();
  page.filter.action = "insert";
  EXPECT_EQ(2u, SelectRows(page).size());

  page.filter.rule = "title-keep";
  EXPECT_EQ(0u, SelectRows(page).size());

  page.filter = Filter();
  page.filter.reason = "present";
  EXPECT_EQ(1u, SelectRows(page).size());
}

TEST(RenderTest, TheLimitCountsMatchesNotLines) {
  Page page = LoadedPage();
  page.filter.rule = "canonical-keep";
  page.limit = 1;
  std::vector<Row> rows = SelectRows(page);
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ(kNoon + 2000, rows[0].ts_ms);  // the newest match
  // A rule that only the oldest line has is still found.
  page.tail.lines.push_back(
      Line(kNoon + 3000, "/c", "title-keep", "none", "x"));
  page.tail.lines.push_back(
      Line(kNoon + 4000, "/d", "title-keep", "none", "x"));
  page.filter.rule = "canonical-keep";
  page.limit = 5;
  EXPECT_EQ(2u, SelectRows(page).size());
  EXPECT_NE(std::string::npos,
            RenderHtml(page).find("2 matching, newest first"));
}

TEST(RenderTest, AVhostShowsOnlyItsHostCaseInsensitively) {
  Page page = LoadedPage();
  page.tail.lines.push_back(Line(kNoon + 5000, "/other", "r", "none", "x"));
  std::string other = page.tail.lines.back();
  other.replace(other.find("t.test"), 6, "other.test");
  page.tail.lines.back() = other;
  EXPECT_EQ(4u, SelectRows(page).size());
  page.filter.host = "T.TEST";
  EXPECT_EQ(3u, SelectRows(page).size());
  page.filter.host = "other.test";
  EXPECT_EQ(1u, SelectRows(page).size());
}

TEST(RenderTest, TheTableNamesThePackOfEachLine) {
  const std::string html = RenderHtml(LoadedPage());
  EXPECT_NE(std::string::npos, html.find("<th>pack</th>"));
  EXPECT_NE(std::string::npos,
            html.find("title=\"sha256 0123456789abcdef\">edge-seo@0.1.0"));
}

TEST(RenderTest, ALineThatIsNotJsonIsSkipped) {
  Page page = LoadedPage();
  page.tail.lines.push_back("not json");
  page.tail.lines.push_back("[1,2]");
  EXPECT_EQ(3u, SelectRows(page).size());
}

TEST(RenderTest, RawIsTheLoggedLinesNewestFirst) {
  Page page = LoadedPage();
  page.filter.rule = "canonical-keep";
  const std::string raw = RenderRaw(page);
  EXPECT_EQ(0u, raw.find("{\"ts\":" + std::to_string(kNoon + 2000)));
  EXPECT_EQ(std::string::npos, raw.find("title-keep"));
  EXPECT_EQ('\n', raw.back());
}

TEST(RenderTest, SaysWhenThereIsNothingToShow) {
  Page page = LoadedPage();
  page.tail = Tail();
  EXPECT_NE(std::string::npos,
            RenderHtml(page).find("No decisions logged today"));
  page.tail.status = Tail::Status::kError;
  page.tail.error = "permission denied";
  EXPECT_NE(std::string::npos, RenderHtml(page).find("permission denied"));
  page.log_off = true;
  EXPECT_NE(std::string::npos, RenderHtml(page).find("LogDir is not set"));
  page = Page();
  page.state = "none";
  page.no_pack_file = true;
  const std::string html = RenderHtml(page);
  EXPECT_NE(std::string::npos, html.find("RulePackFile is not set"));
  EXPECT_EQ(std::string::npos, html.find("not yet loaded in this process"));
  page.pack_file = "/p.json";
  page.no_pack_file = false;
  EXPECT_NE(std::string::npos,
            RenderHtml(page).find("not yet loaded in this process"));
}

// Two packs, the second licensed and verified, with an overlap.
Page TwoPackPage() {
  Page page = LoadedPage();
  page.require_signed = true;
  page.now_ms = kNoon + 5000;
  page.tail_pack = "edge-seo";
  PackEntry first;
  first.file = "/etc/packs/edge.json";
  first.state = "loaded";
  first.last_event_ms = kNoon;
  first.pack = page.pack;
  PackEntry second;
  second.file = "/etc/packs/second.json";
  second.state = "failed-keeping-earlier";
  second.last_event_ms = kNoon + 1000;
  second.last_error = "unexpected key";
  second.pack.present = true;
  second.pack.id = "fixture-second";
  second.pack.version = "1.2.0";
  second.pack.publisher = "Example";
  second.pack.sha256 = std::string(64, 'b');
  second.pack.key_id = "example-key";
  second.pack.license.present = true;
  second.pack.license.label = "Second pack";
  second.pack.license.expires = "2027-10-31";
  second.pack.license.terms = "https://example.com/terms";
  PackEntry third;
  third.file = "/etc/packs/missing.json";
  third.state = "none";
  page.packs = {first, second, third};
  page.overlap_warnings = {"rule pack overlap: title owned by edge-seo"};
  return page;
}

constexpr char kTwoPackJson[] =
    R"({"configuration":{"filter_enabled":true,"rule_pack_files":)"
    R"(["/etc/packs/edge.json","/etc/packs/second.json",)"
    R"("/etc/packs/missing.json"],"rule_pack_mode":"report",)"
    R"("rule_pack_require_signed":true},"counters":)"
    R"({"rule_pack_pages_evaluated":3},"decisions":{"date":"2026-10-11",)"
    R"("error":null,"limit":100,"pack":"edge-seo","rows":[)"
    R"({"action":"insert","after":"","at":"2026-10-11T12:00:02Z",)"
    R"("before":"","claimed_by":null,"defect":"","header":false,"host":"t.test",)"
    R"("kind":"canonical","mode":"report","pack_id":"edge-seo","pack_version":"0.1.0",)"
    R"("path":"/<script>alert(1)</script>","reason":"missing","removed":"",)"
    R"("rule":"canonical-keep","sha256_prefix":"0123456789abcdef",)"
    R"("ts_ms":1791720002000},)"
    R"({"action":"none","after":"","at":"2026-10-11T12:00:01Z",)"
    R"("before":"","claimed_by":null,"defect":"","header":false,"host":"t.test",)"
    R"("kind":"canonical","mode":"report","pack_id":"edge-seo","pack_version":"0.1.0",)"
    R"("path":"/b","reason":"present","removed":"","rule":"title-keep",)"
    R"("sha256_prefix":"0123456789abcdef","ts_ms":1791720001000},)"
    R"({"action":"insert","after":"","at":"2026-10-11T12:00:00Z",)"
    R"("before":"","claimed_by":null,"defect":"","header":false,"host":"t.test",)"
    R"("kind":"canonical","mode":"report","pack_id":"edge-seo","pack_version":"0.1.0",)"
    R"("path":"/a","reason":"missing","removed":"","rule":"canonical-keep",)"
    R"("sha256_prefix":"0123456789abcdef","ts_ms":1791720000000}],)"
    R"("status":"ok"},"generated_at":"2026-10-11T12:00:05Z",)"
    R"("host":"t.test","overlap_warnings":)"
    R"(["rule pack overlap: title owned by edge-seo"],"packs":[)"
    R"({"file":"/etc/packs/edge.json","id":"edge-seo","key_id":null,)"
    R"("last_error":null,"last_event_at":"2026-10-11T12:00:00Z",)"
    R"("license":null,"position":0,"publisher":"Example",)"
    R"("sha256":")"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    R"(","state":"loaded","version":"0.1.0","warnings":["a warning"]},)"
    R"({"file":"/etc/packs/second.json","id":"fixture-second",)"
    R"("key_id":"example-key","last_error":"unexpected key",)"
    R"("last_event_at":"2026-10-11T12:00:01Z","license":{"expires":)"
    R"("2027-10-31","label":"Second pack","terms":)"
    R"("https://example.com/terms"},"position":1,"publisher":"Example",)"
    R"("sha256":")"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    R"(","state":"failed-keeping-earlier","version":"1.2.0",)"
    R"("warnings":[]},{"file":"/etc/packs/missing.json","id":null,)"
    R"("key_id":null,"last_error":null,"last_event_at":null,"license":null,)"
    R"("position":2,"publisher":null,"sha256":null,"state":"none",)"
    R"("version":null,"warnings":[]}],"schema":1,"scope":"vhost"}
)";

// The JSON is a contract: the exact text is pinned.
TEST(JsonTest, TheShapeIsPinned) {
  EXPECT_EQ(std::string(kTwoPackJson), RenderJson(TwoPackPage()));
}

TEST(JsonTest, NoCountersNoLogAreStatusesNotErrors) {
  Page page = TwoPackPage();
  page.have_counters = false;
  page.log_off = true;
  const std::string json = RenderJson(page);
  EXPECT_NE(std::string::npos, json.find("\"counters\":null"));
  EXPECT_NE(std::string::npos, json.find("\"status\":\"log_dir_not_set\""));
  EXPECT_NE(std::string::npos, json.find("\"rows\":[]"));
}

TEST(JsonTest, AFilterNarrowsTheRows) {
  Page page = TwoPackPage();
  page.tail.lines.push_back(
      "{\"ts\":1791720003000,\"host\":\"t.test\",\"path\":\"/c\","
      "\"rule\":\"x\",\"action\":\"none\",\"reason\":\"claimed\","
      "\"claimed_by\":\"edge-seo\"}");
  const std::string claimed = RenderJson(page);
  EXPECT_NE(std::string::npos, claimed.find("\"claimed_by\":\"edge-seo\""));
  page.filter.rule = "title-keep";
  const std::string json = RenderJson(page);
  EXPECT_NE(std::string::npos, json.find("\"path\":\"/b\""));
  EXPECT_EQ(std::string::npos, json.find("\"path\":\"/a\""));
}

TEST(RenderTest, SeveralPacksAreListedInOrderWithStateAndLicenseText) {
  const std::string html = RenderHtml(TwoPackPage());
  const size_t first = html.find(">edge-seo<");
  const size_t second = html.find(">fixture-second<");
  ASSERT_NE(std::string::npos, first);
  ASSERT_NE(std::string::npos, second);
  EXPECT_LT(first, second);
  EXPECT_NE(std::string::npos, html.find("failed-keeping-earlier"));
  EXPECT_NE(std::string::npos, html.find("unexpected key"));
  EXPECT_NE(std::string::npos, html.find("Second pack, expires 2027-10-31, "
                                         "https://example.com/terms"));
  EXPECT_NE(std::string::npos, html.find(">example-key<"));
  EXPECT_NE(std::string::npos, html.find("rule pack overlap: title owned"));
  EXPECT_NE(std::string::npos, html.find("/etc/packs/missing.json"));
  EXPECT_NE(std::string::npos, html.find("RulePackRequireSigned"));
  EXPECT_NE(std::string::npos, html.find("href=\"?pack=fixture-second\""));
  EXPECT_NE(std::string::npos, html.find("Decisions of pack edge-seo"));
}

TEST(RenderTest, AnExplicitPackIsKeptByTheRawLinkAndTheForm) {
  Page page = TwoPackPage();
  page.tail_pack = "fixture-second";
  page.tail_pack_requested = true;
  const std::string html = RenderHtml(page);
  EXPECT_NE(std::string::npos, html.find("&amp;pack=fixture-second"));
  EXPECT_NE(std::string::npos,
            html.find("name=\"pack\" value=\"fixture-second"));
  // Without ?pack= the single-pack page has no trace of it.
  const std::string plain = RenderHtml(LoadedPage());
  EXPECT_EQ(std::string::npos, plain.find("pack="));
  EXPECT_EQ(std::string::npos, plain.find("Packs (load order)"));
}

}  // namespace
}  // namespace rule_pack_view
}  // namespace net_instaweb

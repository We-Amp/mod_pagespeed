// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// The text of a <style> element that PageSpeed writes into an HTML page.
//
// Every filter that puts CSS into a new or rewritten <style> element gets the
// bytes to write from InlineStyleText::Prepare(), which either returns them
// or refuses the CSS. Prepare() checks its own output with
// IsSafeInlineStyleText() before it returns it; that check knows nothing
// about CSS and is the property the rest of the code relies on.

#ifndef PAGESPEED_KERNEL_HTML_INLINE_STYLE_TEXT_H_
#define PAGESPEED_KERNEL_HTML_INLINE_STYLE_TEXT_H_

#include <atomic>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// How CSS text is treated for the document it is written into.
enum class InlineStyleMode {
  // An HTML document with a declared charset from the supported set (see
  // InlineStyleModeForCharset). A '<' in a CSS string, comment or unquoted
  // url(), and an escaped '<' anywhere, are written as the CSS escape
  // "\3c "; a '<' elsewhere is kept only before ASCII whitespace, '=', a
  // digit or '.', and any other one refuses the CSS.
  kHtml,
  // Any other document (XHTML, a type or charset that is not known or not
  // supported): CSS that holds a '<' anywhere is refused; nothing is
  // rewritten.
  kStrict,
};

// Changes whenever the rules above change. Filters put it in the cache keys
// of results that depend on the rules, so results computed under older rules
// are never read.
extern const char kInlineStyleScanVersion[];

// The single-character tag of a mode, for cache keys: "h" or "x".
const char* InlineStyleModeKey(InlineStyleMode mode);

// The mode for an HTML document whose declared charset is `charset` (empty
// when none was declared). Only the labels the Encoding Standard defines for
// UTF-8, windows-1250 to windows-1258 and ISO-8859-x get kHtml; an empty or
// any other label gets kStrict.
InlineStyleMode InlineStyleModeForCharset(StringPiece charset);

// For tests: the labels InlineStyleModeForCharset() accepts, in the sorted
// order its lookup needs.
StringPieceVector InlineStyleCharsetLabelsForTesting();

// True iff `text` may be written as the contents of a <style> element of a
// document in `mode`: in kStrict it holds no '<'; in kHtml every '<' in it
// is followed by ASCII whitespace, '=', a digit or '.' (so it is never the
// last byte). Independent of any CSS parsing.
bool IsSafeInlineStyleText(StringPiece text, InlineStyleMode mode);

class InlineStyleText {
 public:
  // An empty text. Only Prepare() fills one in.
  InlineStyleText() = default;
  InlineStyleText(InlineStyleText&&) = default;
  InlineStyleText& operator=(InlineStyleText&&) = default;

  // Prepares `css` to be written as the text of a <style> element in a
  // document of `mode`. Returns false when it must not be written; then
  // `*reason` (when not null) is set to a fixed description and `*out` is
  // left empty. On true, out->text() is what to write, and it passed
  // IsSafeInlineStyleText(out->text(), mode).
  static bool Prepare(StringPiece css, InlineStyleMode mode,
                      InlineStyleText* out, const char** reason);

  const GoogleString& text() const { return text_; }

  // Whether any byte of the input was rewritten.
  bool changed() const { return changed_; }

  // For tests: called for every Prepare() with its input, its mode, and the
  // text it returns (null when it refuses). Set it while no rewrite runs, and
  // reset it to null.
  using PrepareObserver = void (*)(StringPiece css, InlineStyleMode mode,
                                   const GoogleString* text);
  static void set_prepare_observer_for_testing(PrepareObserver observer);

 private:
  static std::atomic<PrepareObserver> prepare_observer_;

  GoogleString text_;
  bool changed_ = false;

  InlineStyleText(const InlineStyleText&) = delete;
  InlineStyleText& operator=(const InlineStyleText&) = delete;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_KERNEL_HTML_INLINE_STYLE_TEXT_H_

// include/itch/dispatch.hpp
#pragma once

// Routes one framed message to a handler by type, after checking its exact length.
//
//   Handled      the type is in scope and the length matched; h.on(view) was called
//   Skipped      the type is unknown or out of scope; nothing was called
//   WrongLength  the type is in scope but the length is wrong; nothing was called
//
// A wrong length is never decoded: reading a field from a short buffer is how a parser
// turns a corrupt file into silent garbage.

#include <cstddef>
#include <itch/messages.hpp>
#include <span>
#include <type_traits>
#include <utility>

namespace itch {

enum class DecodeStatus { Handled, Skipped, WrongLength };

namespace detail {

// True when h.on(view) cannot throw for every view type decode() can build. A handler that
// can throw (a book builder reporting exhausted capacity, or std::bad_alloc) must see its
// exception reach the caller, not std::terminate, so noexcept is conditional.
template <class H>
inline constexpr bool kHandlerNothrow =
    noexcept(std::declval<H&>().on(std::declval<AddOrderView>())) &&
    noexcept(std::declval<H&>().on(std::declval<ExecutedView>())) &&
    noexcept(std::declval<H&>().on(std::declval<ExecutedPriceView>())) &&
    noexcept(std::declval<H&>().on(std::declval<CancelView>())) &&
    noexcept(std::declval<H&>().on(std::declval<DeleteView>())) &&
    noexcept(std::declval<H&>().on(std::declval<ReplaceView>())) &&
    noexcept(std::declval<H&>().on(std::declval<TradeView>())) &&
    noexcept(std::declval<H&>().on(std::declval<CrossTradeView>())) &&
    noexcept(std::declval<H&>().on(std::declval<BrokenTradeView>())) &&
    noexcept(std::declval<H&>().on(std::declval<SystemEventView>())) &&
    noexcept(std::declval<H&>().on(std::declval<TradingActionView>())) &&
    noexcept(std::declval<H&>().on(std::declval<StockDirectoryView>()));

template <class View, class H>
[[nodiscard]] inline DecodeStatus dispatch_one(std::span<const std::byte> msg, H& h,
                                               std::size_t expected) noexcept(kHandlerNothrow<H>) {
    if (msg.size() != expected) {
        return DecodeStatus::WrongLength;
    }
    h.on(View(msg.data()));
    return DecodeStatus::Handled;
}

}  // namespace detail

template <class H>
[[nodiscard]] inline DecodeStatus decode(std::span<const std::byte> msg,
                                         H& h) noexcept(detail::kHandlerNothrow<H>) {
    if (msg.empty()) {
        return DecodeStatus::Skipped;
    }
    switch (detail::char_at(msg.data())) {
        case 'A':
            return detail::dispatch_one<AddOrderView>(msg, h, AddOrderView::kSize);
        case 'F':
            return detail::dispatch_one<AddOrderView>(msg, h, AddOrderView::kSizeWithMpid);
        case 'E':
            return detail::dispatch_one<ExecutedView>(msg, h, ExecutedView::kSize);
        case 'C':
            return detail::dispatch_one<ExecutedPriceView>(msg, h, ExecutedPriceView::kSize);
        case 'X':
            return detail::dispatch_one<CancelView>(msg, h, CancelView::kSize);
        case 'D':
            return detail::dispatch_one<DeleteView>(msg, h, DeleteView::kSize);
        case 'U':
            return detail::dispatch_one<ReplaceView>(msg, h, ReplaceView::kSize);
        case 'P':
            return detail::dispatch_one<TradeView>(msg, h, TradeView::kSize);
        case 'Q':
            return detail::dispatch_one<CrossTradeView>(msg, h, CrossTradeView::kSize);
        case 'B':
            return detail::dispatch_one<BrokenTradeView>(msg, h, BrokenTradeView::kSize);
        case 'S':
            return detail::dispatch_one<SystemEventView>(msg, h, SystemEventView::kSize);
        case 'H':
            return detail::dispatch_one<TradingActionView>(msg, h, TradingActionView::kSize);
        case 'R':
            return detail::dispatch_one<StockDirectoryView>(msg, h, StockDirectoryView::kSize);
        default:
            return DecodeStatus::Skipped;
    }
}

}  // namespace itch

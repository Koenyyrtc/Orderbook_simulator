// order_book.hpp: a single-symbol limit order book with price-time priority.
#pragma once

#include "types.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob {

class OrderBook {
public:
    explicit OrderBook(Listener* listener = nullptr, bool self_trade_prevention = true)
        : listener_(listener), stp_(self_trade_prevention) {}

    // ---- Order entry --------------------------------------------------------
    // Returns true if the order was accepted (it may still have traded fully,
    // partly, or rested). Returns false if it was rejected.
    bool submit(Order o, Time now) {
        o.ts = now;
        if (o.type == OrderType::Market) o.tif = TimeInForce::IOC;  // markets never rest

        if (auto why = validate(o, /*check_duplicate=*/true)) return reject(o, *why, now);

        if (listener_) listener_->on_accept(o, now);

        if (o.side == Side::Buy) match(o, asks_, now);
        else                     match(o, bids_, now);

        if (o.qty > 0) {  // something is left over
            if (o.type == OrderType::Limit && o.tif == TimeInForce::GTC) {
                if (o.side == Side::Buy) rest(bids_, o);
                else                     rest(asks_, o);
            } else if (listener_) {
                const CancelReason why = o.type == OrderType::Market ? CancelReason::MarketRemainder
                                                                     : CancelReason::IocRemainder;
                listener_->on_cancel(o, why, now);
            }
        }
        return true;
    }

    bool cancel(OrderId id, Time now) {
        std::optional<Order> removed = remove_resting(id);
        if (!removed) return false;
        if (listener_) listener_->on_cancel(*removed, CancelReason::UserRequested, now);
        return true;
    }

    // Shrinking the quantity at the same price keeps queue priority.
    // Anything else (new price, larger size) is cancel + new order: priority is lost.
    bool modify(OrderId id, Price new_price, Qty new_qty, Time now) {
        auto found = index_.find(id);
        if (found == index_.end() || new_qty <= 0 || new_price <= 0) return false;

        Order& o = *found->second.it;
        if (new_price == o.price && new_qty <= o.qty) {
            level_of(found->second).total -= (o.qty - new_qty);
            o.qty = new_qty;
            return true;
        }
        const Order old = o;  // copy: remove_resting() destroys the list node o refers to
        Order replacement = old;
        replacement.price = new_price;
        replacement.qty   = new_qty;
        if (validate(replacement, /*check_duplicate=*/false)) return false;  // old order stays
        remove_resting(id);
        if (listener_) listener_->on_cancel(old, CancelReason::Replaced, now);
        return submit(replacement, now);  // re-enters at the back of the queue
    }

    // ---- Market data --------------------------------------------------------
    std::optional<Price> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }
    std::optional<Price> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }
    std::optional<LevelInfo> top(Side side) const {
        return side == Side::Buy ? top_of(bids_) : top_of(asks_);
    }
    std::vector<LevelInfo> depth(Side side, std::size_t n) const {
        return side == Side::Buy ? snapshot(bids_, n) : snapshot(asks_, n);
    }
    const Order* find(OrderId id) const {
        auto it = index_.find(id);
        return it == index_.end() ? nullptr : &*it->second.it;
    }
    std::size_t order_count() const { return index_.size(); }

    // Self-check used by the tests: returns false if any structural rule is broken.
    bool check_invariants() const {
        if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
            return false;  // crossed book
        std::size_t seen = 0;
        auto check_side = [&](const auto& book) {
            for (const auto& [price, level] : book) {
                if (level.fifo.empty()) return false;  // empty levels must be erased
                Qty sum = 0;
                for (const Order& o : level.fifo) {
                    if (o.price != price || o.qty <= 0) return false;
                    sum += o.qty;
                    ++seen;
                    auto f = index_.find(o.id);
                    if (f == index_.end() || &*f->second.it != &o) return false;
                }
                if (sum != level.total) return false;  // cached total drifted
            }
            return true;
        };
        return check_side(bids_) && check_side(asks_) && seen == index_.size();
    }

private:
    // All resting orders at one price, oldest first, plus a cached total.
    struct PriceLevel {
      std::list<Order> fifo; // oldest first
      Qty total = 0;
    };
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>; // uses 'using' to define that strucutre more efficiently
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>; // maps price to price level
    
    struct Locator { // enough information to find an order
      Side side;
      Price price;
      std::list<Order>::iterator it; // creates a pointer (iterator) for the list of order objects 
    };
  

    // Every pre-trade check in one place. Returns the reason to reject, or nothing.
    std::optional<RejectReason> validate(const Order& o, bool check_duplicate) const {
        if (o.qty <= 0)                                 return RejectReason::BadQuantity;
        if (o.type == OrderType::Limit && o.price <= 0) return RejectReason::BadPrice;
        if (o.post_only && (o.type == OrderType::Market || o.tif != TimeInForce::GTC))
                                                        return RejectReason::BadFlags;
        if (check_duplicate && index_.count(o.id) != 0) return RejectReason::DuplicateId;
        if (o.post_only && would_cross(o))              return RejectReason::PostOnlyWouldCross;
        if (o.tif == TimeInForce::FOK && fillable_qty(o) < o.qty)
                                                        return RejectReason::FokNotFillable;
        return std::nullopt;
    }

    bool reject(const Order& o, RejectReason why, Time now) {
        if (listener_) listener_->on_reject(o, why, now);
        return false;
    }

    // Does this incoming order trade against a resting order at level_price?
    static bool crosses(const Order& o, Price level_price) {
        if (o.type == OrderType::Market) return true;
        return o.side == Side::Buy ? level_price <= o.price : level_price >= o.price;
    }

    bool would_cross(const Order& o) const {
        if (o.side == Side::Buy) return !asks_.empty() && crosses(o, asks_.begin()->first);
        return !bids_.empty() && crosses(o, bids_.begin()->first);
    }

    // How much could this order trade right now? Used for fill-or-kill.
    Qty fillable_qty(const Order& o) const {
        return o.side == Side::Buy ? fillable(asks_, o) : fillable(bids_, o);
    }
    template <class Book>
    Qty fillable(const Book& book, const Order& o) const {
        Qty sum = 0;
        for (const auto& [price, level] : book) {
            if (!crosses(o, price)) break;
            if (stp_) {  // orders from the same owner would be cancelled, not traded
                for (const Order& m : level.fifo)
                    if (m.owner != o.owner) sum += m.qty;
            } else {
                sum += level.total;
            }
            if (sum >= o.qty) break;
        }
        return sum;
    }

    // The matching loop. `book` is the OPPOSITE side of the incoming order.
    // begin() is always the best price on that side, because of the map ordering.
    template <class Book>
    void match(Order& in, Book& book, Time now) {
        while (in.qty > 0 && !book.empty()) {
            auto level_it = book.begin();
            if (!crosses(in, level_it->first)) break;
            PriceLevel& level = level_it->second;

            while (in.qty > 0 && !level.fifo.empty()) {
                Order& maker = level.fifo.front();  // oldest order at this price

                if (stp_ && maker.owner == in.owner) {  // self-trade: cancel the resting one
                    const Order dead = maker;
                    level.total -= dead.qty;
                    index_.erase(dead.id);
                    level.fifo.pop_front();
                    if (listener_) listener_->on_cancel(dead, CancelReason::SelfTrade, now);
                    continue;
                }

                const Qty fill = std::min(in.qty, maker.qty);
                in.qty      -= fill;
                maker.qty   -= fill;
                level.total -= fill;
                if (listener_)
                    listener_->on_trade(Trade{in.id, maker.id, in.owner, maker.owner,
                                              in.side, level_it->first, fill, now});
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    level.fifo.pop_front();
                }
            }
            if (level.fifo.empty()) book.erase(level_it);  // never keep empty levels
        }
    }

    template <class Book>
    void rest(Book& book, const Order& o) {
        PriceLevel& level = book[o.price];  // operator[] creates the level if missing
        level.fifo.push_back(o);
        level.total += o.qty;
        index_.emplace(o.id, Locator{o.side, o.price, std::prev(level.fifo.end())});
    }

    std::optional<Order> remove_resting(OrderId id) {
        auto found = index_.find(id);
        if (found == index_.end()) return std::nullopt;
        const Locator loc = found->second;
        const Order removed = *loc.it;
        if (loc.side == Side::Buy) erase_from(bids_, loc);
        else                       erase_from(asks_, loc);
        index_.erase(found);
        return removed;
    }

    template <class Book>
    static void erase_from(Book& book, const Locator& loc) {
        auto level_it = book.find(loc.price);
        level_it->second.total -= loc.it->qty;
        level_it->second.fifo.erase(loc.it);
        if (level_it->second.fifo.empty()) book.erase(level_it);
    }

    PriceLevel& level_of(const Locator& loc) {
        return loc.side == Side::Buy ? bids_.find(loc.price)->second
                                     : asks_.find(loc.price)->second;
    }

    template <class Book>
    static std::optional<LevelInfo> top_of(const Book& book) {
        if (book.empty()) return std::nullopt;
        const auto& [price, level] = *book.begin();
        return LevelInfo{price, level.total, static_cast<int>(level.fifo.size())};
    }

    template <class Book>
    static std::vector<LevelInfo> snapshot(const Book& book, std::size_t n) {
        std::vector<LevelInfo> out;
        for (const auto& [price, level] : book) {
            if (out.size() == n) break;
            out.push_back({price, level.total, static_cast<int>(level.fifo.size())});
        }
        return out;
    }

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Locator> index_;
    Listener* listener_;
    bool      stp_;
};

}  // namespace ob

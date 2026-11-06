// engine.hpp: the exchange front door around the book. It adds id allocation,
// pre-trade risk checks, ownership checks and running market statistics.
#pragma once

#include "order_book.hpp"

#include <cmath>
#include <optional>

namespace ob {

struct RiskLimits {
    Qty    max_order_qty = 1'000;
    double price_band    = 0.10;  // reject limit prices more than 10% from the reference
};

class Engine : public Listener {
public:
    struct Quote {
        std::optional<Price> bid, ask;
        Qty   bid_qty = 0, ask_qty = 0;
        Price last = 0;  // 0 until the first trade
    };

    // `downstream` receives every event after the engine has updated its own state.
    explicit Engine(Listener* downstream = nullptr, RiskLimits limits = {},
                    bool self_trade_prevention = true)
        : book_(this, self_trade_prevention), downstream_(downstream), limits_(limits) {}

    OrderId allocate_id() { return ++last_id_; }
    void    set_reference_price(Price p) { reference_price_ = p; }

    bool submit(Order o, Time now) {
        if (!passes_risk(o, now)) return false;
        return book_.submit(o, now);
    }

    // Only the owner may cancel or modify an order.
    bool cancel(AgentId requester, OrderId id, Time now) {
        const Order* o = book_.find(id);
        if (o == nullptr || o->owner != requester) return false;
        return book_.cancel(id, now);
    }
    bool modify(AgentId requester, OrderId id, Price new_price, Qty new_qty, Time now) {
        const Order* o = book_.find(id);
        if (o == nullptr || o->owner != requester) return false;
        Order probe = *o;  // what the order would look like after the change
        probe.price = new_price;
        probe.qty   = new_qty;
        if (!passes_risk(probe, now)) return false;  // modify must not be a way around the limits
        return book_.modify(id, new_price, new_qty, now);
    }

    Quote quote() const {
        Quote q;
        q.last = last_price_;
        if (auto b = book_.top(Side::Buy))  { q.bid = b->price; q.bid_qty = b->qty; }
        if (auto a = book_.top(Side::Sell)) { q.ask = a->price; q.ask_qty = a->qty; }
        return q;
    }

    const OrderBook& book() const { return book_; }
    Qty          volume() const { return volume_; }
    std::int64_t trade_count() const { return trades_; }

    // ---- Listener: update statistics, then pass the event on ----------------
    void on_accept(const Order& o, Time t) override { if (downstream_) downstream_->on_accept(o, t); }
    void on_reject(const Order& o, RejectReason r, Time t) override {
        if (downstream_) downstream_->on_reject(o, r, t);
    }
    void on_cancel(const Order& o, CancelReason r, Time t) override {
        if (downstream_) downstream_->on_cancel(o, r, t);
    }
    void on_trade(const Trade& tr) override {
        last_price_      = tr.price;
        reference_price_ = tr.price;
        volume_         += tr.qty;
        ++trades_;
        if (downstream_) downstream_->on_trade(tr);
    }

private:
    // Pre-trade risk checks shared by new orders and modifications.
    bool passes_risk(const Order& o, Time now) {
        if (o.qty > limits_.max_order_qty) {
            on_reject(o, RejectReason::SizeLimit, now);
            return false;
        }
        if (o.type == OrderType::Limit && reference_price_ > 0) {
            const double away = std::abs(static_cast<double>(o.price - reference_price_));
            if (away > limits_.price_band * static_cast<double>(reference_price_)) {
                on_reject(o, RejectReason::PriceBand, now);
                return false;
            }
        }
        return true;
    }

    OrderBook    book_;  // declared first: initialised first, see the constructor
    Listener*    downstream_;
    RiskLimits   limits_;
    OrderId      last_id_         = 0;
    Price        last_price_      = 0;
    Price        reference_price_ = 0;
    Qty          volume_          = 0;
    std::int64_t trades_          = 0;
};

}  // namespace ob

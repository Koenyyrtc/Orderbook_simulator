#pragma once

#include <cstdint>

namespace ob {

using Price   = std::int64_t;  // integer number of ticks, never a float
using Qty     = std::int64_t;  // signed, so "position -= fill" is safe
using OrderId = std::uint64_t;
using AgentId = std::uint32_t;
using Time    = std::int64_t;  // simulated nanoseconds

constexpr Time US  = 1'000;
constexpr Time MS  = 1'000'000;
constexpr Time SEC = 1'000'000'000;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

constexpr Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

struct Order {
    OrderId     id        = 0;
    AgentId     owner     = 0;
    Side        side      = Side::Buy;
    OrderType   type      = OrderType::Limit;
    TimeInForce tif       = TimeInForce::GTC;
    bool        post_only = false;
    Price       price     = 0;  // ignored for market orders
    Qty         qty       = 0;  // remaining quantity; shrinks as the order fills
    Time        ts        = 0;  // arrival time at the book (stamped by the book)
};

struct Trade {
    OrderId taker_id;
    OrderId maker_id;
    AgentId taker_owner;
    AgentId maker_owner;
    Side    taker_side;  // the aggressor's side
    Price   price;       // always the resting (maker) order's price
    Qty     qty;
    Time    ts;
};

// One aggregated row of market depth.
struct LevelInfo {
    Price price;
    Qty   qty;
    int   orders;
};

enum class RejectReason : std::uint8_t {
    BadQuantity,
    BadPrice,
    BadFlags,
    DuplicateId,
    PostOnlyWouldCross,
    FokNotFillable,
    SizeLimit,
    PriceBand,
};
constexpr int kRejectReasons = 8;

enum class CancelReason : std::uint8_t {
    UserRequested,
    IocRemainder,
    MarketRemainder,
    SelfTrade,
    Replaced,
};

inline const char* to_string(RejectReason r) {
    switch (r) {
        case RejectReason::BadQuantity:        return "BadQuantity";
        case RejectReason::BadPrice:           return "BadPrice";
        case RejectReason::BadFlags:           return "BadFlags";
        case RejectReason::DuplicateId:        return "DuplicateId";
        case RejectReason::PostOnlyWouldCross: return "PostOnlyWouldCross";
        case RejectReason::FokNotFillable:     return "FokNotFillable";
        case RejectReason::SizeLimit:          return "SizeLimit";
        case RejectReason::PriceBand:          return "PriceBand";
    }
    return "?";
}

// The book reports everything that happens through this interface. Every method
// has an empty default body, so a listener overrides only what it cares about.
class Listener {
public:
    virtual ~Listener() = default;
    virtual void on_accept(const Order&, Time) {}
    virtual void on_reject(const Order&, RejectReason, Time) {}
    virtual void on_trade(const Trade&) {}
    virtual void on_cancel(const Order&, CancelReason, Time) {}
};

}

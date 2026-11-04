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

BidMap bids_;
AskMap asks_;
std::unordered_map<OrderId, Locator> index_;

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

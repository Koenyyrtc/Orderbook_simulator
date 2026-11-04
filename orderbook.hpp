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

class Context {
public:
    virtual ~Context() = default;
    virtual Time                   now() const = 0;
    virtual Engine::Quote          quote() const = 0;                       
    virtual std::vector<LevelInfo> depth(Side, std::size_t) const = 0;
    virtual OrderId limit(Side, Price, Qty, TimeInForce, bool post_only) = 0;
    virtual OrderId market(Side, Qty) = 0;
    virtual void    cancel(OrderId) = 0;
    virtual std::mt19937_64& rng() = 0;
};

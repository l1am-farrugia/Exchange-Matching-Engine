#include "event_io.h"

#include <sstream>
#include <unordered_map>

namespace ob
{
    static std::string chomp_cr(std::string s)
    {
        // strips windows carriage return if present
        if (!s.empty() && s.back() == '\r')
        {
            s.pop_back();
        }
        return s;
    }

    const char* event_type_to_string(EventType t)
    {
        switch (t)
        {
        case EventType::OrderAccepted:
            return "order_accepted";
        case EventType::OrderRejected:
            return "order_rejected";
        case EventType::Trade:
            return "trade";
        case EventType::OrderResting:
            return "order_resting";
        case EventType::OrderCompleted:
            return "order_completed";
        case EventType::MakerCompleted:
            return "maker_completed";
        case EventType::OrderCancelled:
            return "order_cancelled";
        case EventType::CancelRejected:
            return "cancel_rejected";
        }
        return "unknown";
    }

    std::optional<EventType> string_to_event_type(const std::string& s)
    {
        static const std::unordered_map<std::string, EventType> map {
            { "order_accepted", EventType::OrderAccepted },
            { "order_rejected", EventType::OrderRejected },
            { "trade", EventType::Trade },
            { "order_resting", EventType::OrderResting },
            { "order_completed", EventType::OrderCompleted },
            { "maker_completed", EventType::MakerCompleted },
            { "order_cancelled", EventType::OrderCancelled },
            { "cancel_rejected", EventType::CancelRejected }
        };

        auto it = map.find(s);
        if (it == map.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    const char* side_to_string(Side s)
    {
        return (s == Side::Buy) ? "buy" : "sell";
    }

    std::optional<Side> string_to_side(const std::string& s)
    {
        if (s == "buy")
        {
            return Side::Buy;
        }
        if (s == "sell")
        {
            return Side::Sell;
        }
        return std::nullopt;
    }

    std::string event_to_line(const Event& e)
    {
        // stable key value output so diffs are easy
        std::ostringstream oss;

        oss << "type=" << event_type_to_string(e.type);

        oss << " id=" << e.id;
        oss << " seq=" << e.seq;

        oss << " side=" << side_to_string(e.side);
        oss << " px=" << e.price_ticks;
        oss << " qty=" << e.qty;
        oss << " rem=" << e.remaining_qty;

        // trade fields are written always, unused values remain 0
        oss << " maker=" << e.maker_id;
        oss << " maker_seq=" << e.maker_seq;
        oss << " taker=" << e.taker_id;
        oss << " taker_seq=" << e.taker_seq;
        oss << " tpx=" << e.trade_price_ticks;
        oss << " tq=" << e.trade_qty;

        // reason is last and can be empty
        oss << " reason=" << e.reason;

        return oss.str();
    }

    static bool read_kv(std::istringstream& iss, std::string& key, std::string& value)
    {
        std::string token;
        if (!(iss >> token))
        {
            return false;
        }

        const auto pos = token.find('=');
        if (pos == std::string::npos)
        {
            return false;
        }

        key = token.substr(0, pos);
        value = token.substr(pos + 1);
        return true;
    }

    std::optional<Event> line_to_event(const std::string& line)
    {
        // parse the same key value format we produce
        std::istringstream iss(chomp_cr(line));

        Event e {};

        std::string key;
        std::string value;

        // type must come first
        if (!read_kv(iss, key, value) || key != "type")
        {
            return std::nullopt;
        }

        const auto et = string_to_event_type(value);
        if (!et.has_value())
        {
            return std::nullopt;
        }
        e.type = *et;

        auto read_u64 = [](const std::string& s, std::uint64_t& out) -> bool
        {
            try
            {
                out = static_cast<std::uint64_t>(std::stoull(s));
                return true;
            }
            catch (...)
            {
                return false;
            }
        };

        auto read_i64 = [](const std::string& s, std::int64_t& out) -> bool
        {
            try
            {
                out = static_cast<std::int64_t>(std::stoll(s));
                return true;
            }
            catch (...)
            {
                return false;
            }
        };

        if (!read_kv(iss, key, value) || key != "id")
        {
            return std::nullopt;
        }
        if (!read_u64(value, e.id))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "seq")
        {
            return std::nullopt;
        }
        if (!read_u64(value, e.seq))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "side")
        {
            return std::nullopt;
        }
        const auto sd = string_to_side(value);
        if (!sd.has_value())
        {
            return std::nullopt;
        }
        e.side = *sd;

        if (!read_kv(iss, key, value) || key != "px")
        {
            return std::nullopt;
        }
        if (!read_i64(value, e.price_ticks))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "qty")
        {
            return std::nullopt;
        }
        if (!read_i64(value, e.qty))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "rem")
        {
            return std::nullopt;
        }
        if (!read_i64(value, e.remaining_qty))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "maker")
        {
            return std::nullopt;
        }
        if (!read_u64(value, e.maker_id))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "maker_seq")
        {
            return std::nullopt;
        }
        if (!read_u64(value, e.maker_seq))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "taker")
        {
            return std::nullopt;
        }
        if (!read_u64(value, e.taker_id))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "taker_seq")
        {
            return std::nullopt;
        }
        if (!read_u64(value, e.taker_seq))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "tpx")
        {
            return std::nullopt;
        }
        if (!read_i64(value, e.trade_price_ticks))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "tq")
        {
            return std::nullopt;
        }
        if (!read_i64(value, e.trade_qty))
        {
            return std::nullopt;
        }

        if (!read_kv(iss, key, value) || key != "reason")
        {
            return std::nullopt;
        }
        
        if (value == "accepted") e.reason = "accepted";
        else if (value == "filled") e.reason = "filled";
        else if (value == "trade") e.reason = "trade";
        else if (value == "resting") e.reason = "resting";
        else if (value == "cancelled") e.reason = "cancelled";
        else if (value == "invalid") e.reason = "invalid";
        else if (value == "not_found") e.reason = "not_found";
        else if (value == "duplicate_id") e.reason = "duplicate_id";
        else e.reason = "";

        return e;
    }
}

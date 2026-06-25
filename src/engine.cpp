#include "engine.h"

#include "event_io.h"

namespace ob
{
    void Engine::apply(const Command& cmd, std::vector<Event>& out_events)
    {
        const std::size_t start_idx = out_events.size();

        // dispatch on command type
        if (cmd.type == CommandType::AddLimit)
        {
            book_.add_limit(cmd.id, cmd.side, cmd.price_ticks, cmd.qty, out_events);
        }
        else
        {
            book_.cancel(cmd.id, out_events);
        }

        // log if enabled
        if (log_.has_value())
        {
            for (std::size_t i = start_idx; i < out_events.size(); ++i)
            {
                // write each event on its own line
                (*log_) << event_to_line(out_events[i]) << "\n";
            }
            log_->flush();
        }

        return events;
    }

    void Engine::apply_all(const std::vector<Command>& cmds, std::vector<Event>& out_events)
        {
            // apply sequentially to preserve determinism
            for (const auto& c : cmds)
            {
                apply(c, out_events);
            }
        }

    bool Engine::start_event_log(const std::string& path)
    {
        // opens the log file and enables event writing
        std::ofstream f(path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!f)
        {
            return false;
        }

        log_.emplace(std::move(f));
        return true;
    }

    void Engine::stop_event_log()
    {
        if (log_.has_value())
        {
            log_->flush();
        }
        log_.reset();
    }

    const OrderBook& Engine::book() const
    {
        return book_;
    }
}

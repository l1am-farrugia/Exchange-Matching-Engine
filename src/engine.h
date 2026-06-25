#pragma once

#include "command.h"
#include "event.h"
#include "order_book.h"

#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace ob
{
    // engine is the command in and event out boundary
    class Engine
    {
    public:
        // applies one command and returns produced events
        void apply(const Command& cmd, std::vector<Event>& out_events);
        
        // applies a list of commands and appends events
        void apply_all(const std::vector<Command>& cmds, std::vector<Event>& out_events);
        
        // enable file logging of events
        bool start_event_log(const std::string& path);

        // stop event logging
        void stop_event_log();

        // read only book access for tests
        const OrderBook& book() const;

    private:
        // book state for this engine instance
        OrderBook book_;

        // event log stream if enabled
        std::optional<std::ofstream> log_;
    };
}

#include "engine.h"

#include "event_io.h"
#include "script.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void print_usage()
{
    std::cout << "usage:\n";
    std::cout << "  ob_sim --script <path>\n";
    std::cout << "  ob_sim --script <path> --record <event_log>\n";
    std::cout << "  ob_sim --replay <path> --events <event_log>\n";
    std::cout << "  ob_sim --bench <path> --iters <n>\n";
}

static std::string chomp_cr(std::string s)
{
    // strips windows carriage return if present
    if (!s.empty() && s.back() == '\r')
    {
        s.pop_back();
    }
    return s;
}

static bool read_all_lines(const std::string& path, std::vector<std::string>& out_lines)
{
    // reads file into lines without trailing '\r'
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }

    std::string line;
    while (std::getline(in, line))
    {
        line = chomp_cr(line);
        if (!line.empty())
        {
            out_lines.push_back(line);
        }
    }
    return true;
}

static int run_script(const std::string& script_path, const std::string& record_path)
{
    const auto cmds_opt = ob::load_script(script_path);
    if (!cmds_opt.has_value())
    {
        std::cerr << "failed to load script\n";
        return 10;
    }

    ob::Engine eng;

    if (!record_path.empty())
    {
        if (!eng.start_event_log(record_path))
        {
            std::cerr << "failed to open event log\n";
            return 11;
        }
    }

    std::vector<ob::Event> events;

    for (const auto& cmd : *cmds_opt)
    {
        events.clear();
        eng.apply(cmd, events);

        for (const auto& e : events)
        {
            std::cout << ob::event_to_line(e) << "\n";
        }
    }

    eng.stop_event_log();
    return 0;
}

static int replay_script(const std::string& script_path, const std::string& events_path)
{
    const auto cmds_opt = ob::load_script(script_path);
    if (!cmds_opt.has_value())
    {
        std::cerr << "failed to load script\n";
        return 10;
    }

    std::vector<std::string> expected_lines;
    if (!read_all_lines(events_path, expected_lines))
    {
        std::cerr << "failed to read events file\n";
        return 12;
    }

    ob::Engine eng;

    std::vector<std::string> actual_lines;
    {
        std::vector<ob::Event> events;
        eng.apply_all(*cmds_opt, events);
        actual_lines.reserve(events.size());

        for (const auto& e : events)
        {
            actual_lines.push_back(ob::event_to_line(e));
        }
    }

    if (actual_lines.size() != expected_lines.size())
    {
        std::cerr << "mismatch lines count expected=" << expected_lines.size()
                  << " actual=" << actual_lines.size() << "\n";
        return 20;
    }

    for (std::size_t i = 0; i < actual_lines.size(); ++i)
    {
        if (actual_lines[i] != expected_lines[i])
        {
            // print local context around first mismatch
            const std::size_t line_no = i + 1;

            std::cerr << "mismatch at line " << line_no << "\n";
            std::cerr << "expected: " << expected_lines[i] << "\n";
            std::cerr << "actual:   " << actual_lines[i] << "\n";

            if (i > 0)
            {
                std::cerr << "prev exp: " << expected_lines[i - 1] << "\n";
                std::cerr << "prev act: " << actual_lines[i - 1] << "\n";
            }

            if (i + 1 < actual_lines.size())
            {
                std::cerr << "next exp: " << expected_lines[i + 1] << "\n";
                std::cerr << "next act: " << actual_lines[i + 1] << "\n";
            }

            return 21;
        }
    }

    std::cout << "replay ok\n";
    return 0;
}

static int bench_script(const std::string& script_path, std::uint64_t iters)
{
    // benches apply_all using the same command list each run
    const auto cmds_opt = ob::load_script(script_path);
    if (!cmds_opt.has_value())
    {
        std::cerr << "failed to load script\n";
        return 10;
    }

    if (iters == 0)
    {
        std::cerr << "iters must be > 0\n";
        return 30;
    }

    using clock = std::chrono::high_resolution_clock;

    std::uint64_t total_events { 0 };

    // pre allocate event sink
    std::vector<ob::Event> events;
    events.reserve(100000); // prevent resizing

    std::uint64_t total_ns { 0 };

    for (std::uint64_t i = 0; i < iters; ++i)
    {
        ob::Engine eng;
        events.clear(); // reset size to 0

        const auto t0 = clock::now();
        eng.apply_all(*cmds_opt, events);
        const auto t1 = clock::now();

        total_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        total_events += static_cast<std::uint64_t>(events.size());
    }

    const auto ns = total_ns;

    const double per_iter_ns = static_cast<double>(ns) / static_cast<double>(iters);
    const double per_event_ns = (total_events > 0) ? (static_cast<double>(ns) / static_cast<double>(total_events)) : 0.0;

    std::cout << "bench iters=" << iters << " total_ns=" << ns << "\n";
    std::cout << "per_iter_ns=" << static_cast<std::uint64_t>(per_iter_ns) << "\n";
    std::cout << "per_event_ns=" << static_cast<std::uint64_t>(per_event_ns) << "\n";

    return 0;
}

int main(int argc, char** argv)
{
    std::string script_path;
    std::string record_path;

    bool replay = false;
    std::string replay_script_path;
    std::string replay_events_path;

    bool bench = false;
    std::string bench_script_path;
    std::uint64_t bench_iters { 0 };

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];

        if (a == "--script" && i + 1 < argc)
        {
            script_path = argv[++i];
        }
        else if (a == "--record" && i + 1 < argc)
        {
            record_path = argv[++i];
        }
        else if (a == "--replay" && i + 1 < argc)
        {
            replay = true;
            replay_script_path = argv[++i];
        }
        else if (a == "--events" && i + 1 < argc)
        {
            replay_events_path = argv[++i];
        }
        else if (a == "--bench" && i + 1 < argc)
        {
            bench = true;
            bench_script_path = argv[++i];
        }
        else if (a == "--iters" && i + 1 < argc)
        {
            bench_iters = static_cast<std::uint64_t>(std::stoull(argv[++i]));
        }
        else
        {
            print_usage();
            return 1;
        }
    }

    if (bench)
    {
        if (bench_script_path.empty() || bench_iters == 0)
        {
            print_usage();
            return 1;
        }
        return bench_script(bench_script_path, bench_iters);
    }

    if (replay)
    {
        if (replay_script_path.empty() || replay_events_path.empty())
        {
            print_usage();
            return 1;
        }
        return replay_script(replay_script_path, replay_events_path);
    }

    if (script_path.empty())
    {
        print_usage();
        return 1;
    }

    return run_script(script_path, record_path);
}

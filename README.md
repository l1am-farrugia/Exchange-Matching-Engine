# Limit Orderbook Matching Engine (C++)

To understand how I built this matching engine, we first have to look at what an order book actually does at a conceptual level. At its core, an electronic order book is just a data structure that keeps track of buyers and sellers in a financial market. It has two main sides: 

- The Bids: A collection of buy orders, sorted from the highest price to the lowest price.

  (Buyers want to pay as little as possible, so the highest bid is the most competitive). 

- The Asks (or Offers): A collection of sell orders, sorted from the lowest price to the highest price. 

  (Sellers want to get paid as much as possible, so the lowest ask is the most competitive). 

The point where the highest bid and the lowest ask meet is the current market price. 

When a new order arrives, the engine has a simple choice. If the incoming order is a "Taker" order, meaning it crosses the market price to immediately match with a resting "Maker" order, the engine executes a trade. If it doesn't match, it becomes a Maker order itself, sitting in the book and waiting for someone else to fill it.

In my original implementation, the system was built using standard C++ containers. 

- `std::map<PriceTicks, PriceLevel>` was used to hold the bids and asks. 

- Each `PriceLevel` contained a `std::list<Order*>` to store individual orders at that specific price in a FIFO queue. 

- A `std::unordered_map<OrderId, Locator>` acted as a master index so we could look up any order instantly by its ID when a user wanted to cancel it. 

When evaluated using the small benchmark script over 1,000,000 iterations, this original layout achieved an execution time of **58ns** on average per event. However, this metric represented an ideal scenario rather than a realistic workload. Because the script only maintained a small footprint of about a dozen active orders at any given moment, the standard library containers never grew to be very large. The entire execution footprint fits entirely inside the L1 cache, requiring minimal tree comparisons. To test on a much heavier script, I evaluated the engine against a dense market script `dense_script.txt` clustering 50,000 events tightly within a narrow 100-tick price window, and a highly sparse macro market script `deep_script.txt` scattering 20,000 active resting orders widely across a 200,000-tick spread. Under the dense workload, the original engine's average event time expanded to about **90ns**, generating **48 million L1 data cache misses**. Under the sparse macro workload, performance degraded to  about **660ns** per event, thrashing the CPU with **4 Billion L1 cache misses**.

To understand why this architecture becomes slow at scale, we have to look at how the program interacts with the operating system and hardware layers, and what issues arise from those interactions which slow the program. The two main issues I found with the original implementation were: 

1) Using keywords like `new` and `delete` make system calls to the OS kernel, when the engine executes `new Order()` the OS performs a context switch; pausing what it's doing to search and find a free chunk of memory on the heap that fits the exact size of our struct, lock that memory so no other program can touch it, and then hand it back to our code. 

In normal applications this context switching takes negligible time, but for the orderbook, in the critical loop where orders are being matched, making the CPU jump back and forth between our engine and the OS kernel is costly. 

Furthermore, `std::list` and `std::map` are node based containers. Every single time a new order enters the book or a price level is created, they technically call `new` under the hood to allocate a node. When an order is filled, they call `delete`. This constant memory churn creates massive, non-deterministic latency spikes.
  
2) The original orderbook also suffered from poor cache utilisation. System RAM has massive capacity, but is physically located far away from the CPU core. Accessing it is (in this case) very slow, taking roughly 60 to 100 nanoseconds (ns). Alternatively, L1/L2 Caches are memory built directly onto the CPU chip itself. Accessing the L1 cache is very fast, taking less than 1 ns (generally about 3-4 CPU clock cycles), while L2 is slightly slower at about 3-5ns or 10-20 clock cycles. 
  
The CPU reads data in continuous memory chunks of 64 bytes called cache lines. If the data the CPU needs next is already sitting in that 64-byte chunk, we have a cache hit and the data is processed instantly. If the data is scattered somewhere else in RAM, it suffers a cache miss, and the entire CPU core completely stalls, sitting idle for 100ns waiting for the data to arrive from the main motherboard. 
  
Since our original design used `std::list` and `std::map`, where every node is allocated independently via `new`, the memory addresses end up completely scattered across the heap. Node A might be at address 0x1000, while Node B is at 0x9999. When the CPU tries to traverse the linked list to match orders, it reads Node A, follows the pointer to Node B, and instantly triggers a cache miss because Node B is nowhere near Node A in physical memory. Since the data has no predictable pattern, the CPU prefetcher becomes completely useless as it can’t easily predict what memory is needed next in order to load it ahead of time. `std::list` is thus terrible for our cache lines because it does exactly this. 
  
Using `std::list<Order*>`, created two layers of indirection. The standard library container allocates its own internal node structure under the hood. This node contains pointers to the next and previous list elements, along with a pointer to your actual Order struct.
  
**Image 1 here**

To traverse this queue, the CPU has to read the list node, find the memory address of the order, jump to that address, and then jump back to the next list node. This constant ping-ponging across the system memory has a high chance of resulting in cache misses.

To fix this, we migrated to an Intrusive Doubly-Linked List. Instead of letting an external container manage our layout, we embedded the next and prev raw pointers directly inside the `Order` struct itself:

```cpp
struct Order
{
    OrderId id;
    Side side;
    PriceTicks price_ticks;
    Qty qty;
    std::uint64_t seq;

    // Intrusive pointers embedded directly in the data
    Order* next { nullptr };
    Order* prev { nullptr };
};
```

This change fundamentally reshapes how the data sits in memory. When the matching engine fetches an Order, the pointers required to find the next order in the FIFO queue are loaded into the exact same 64-byte cache line simultaneously.

**Image 2 Here**

We completely eliminated the mid-level container nodes. Now, each `PriceLevel` only needs to store a head and a tail pointer. Walking the queue becomes a straightforward matter of following the embedded pointers already sitting in the CPU's L1 cache.

While this cleaned up our memory layout, we were still fundamentally tied to the OS because we were using `new` and `delete` to create and destroy the `Order` nodes. To remove the dynamic memory allocation completely, I built a custom memory pool. 

The core philosophy here is simple: Allocate everything upfront and recycle everything aggressively. Instead of asking the OS kernel for a small piece of memory every single time an order is submitted, the orderbook will pre-allocate a massive contiguous block of 4,096 Order structs right at startup.

To manage this block without adding any extra memory overhead, we implemented a Free List. Because an idle order node isn't resting in the book, its embedded next pointer isn't being used. 

We can repurpose that pointer to link all the unallocated slots together in a single chain.

**Image 3 Here**

When a new limit order arrives at the matching engine, we call `allocate_order()`:

```cpp
Order* OrderBook::allocate_order()
{
    if (free_list_ != nullptr)
    {
        Order* o = free_list_;
        free_list_ = o->next;
        return o;
    }
}
```

This is an O(1) operation. The engine grabs the node right at the front of `free_list_`, shifts the pointer forward, and hands the memory to the engine. This completely avoids any system calls, heap traversal or kernel locks.

When an order gets filled or cancelled, we pass it to `free_order()`:

```cpp
void OrderBook::free_order(Order* o)
{
    o->next = free_list_;
    free_list_ = o;
}
```

Instead of wiping the memory or releasing it back to the OS, the engine weaves the dead node right back into the front of the free list. The memory stays warm in the CPU cache, perfectly ready to be reused by the very next incoming event. 

By implementing the intrusive layout and the memory pool, we completely protected the hot path from the OS kernel, bringing our latency down to a stable baseline of **728ns**.

With the order nodes tightly packed via the memory pool and linked lists, the next step was to revise how the engine searched for price levels. 

Originally, I relied on `std::map<PriceTicks, PriceLevel>`. A C++ `std::map` is universally implemented as a Red-Black Tree. When the matching engine receives an incoming order, it has to look up the correct price level by starting at the root node of the tree and following left or right pointers down to the target leaf.

This structural pattern is a massive performance bottleneck because of this pointer chasing. To find a price level, the CPU core has to read a node, extract a pointer address, jump to that new memory address, read the next node, and repeat. Even though the lookup complexity is O(log n), it forces the CPU to constantly guess where to look next, still disabling the CPU prefetcher. 

In attempts to correct this, I initially stripped out the `std::map` completely and replaced it with `std::vector<LevelRecord>`. The vector stores all its elements in a single contiguous block of memory. To keep the price levels sorted (highest bids first, lowest asks first), I used `std::lower_bound` to run a binary search across the array whenever a new price level needed to be found. Because the vector is contiguous, the CPU prefetcher can load adjacent elements into the L1 cache before the code even requests them, providing a cleaner memory access routine.

**Image 4 Here**

This however, barely improved execution time, from an average of **728ns** to **708ns**. While searching a vector is fast, inserting a new price level into the middle of the array forces the vector to move every subsequent element one slot down in memory to maintain order. This memory shuffle effectively cancelled out the benefits gained from avoiding the tree traversal.

To maintain the cache locality of a vector while avoiding binary searches and memory shuffles. I implemented a Direct-Mapped Static Array. Instead of storing only active price levels, two arrays, one for bids and one for asks are used and pre-sized to a fixed window of 1,000,000 price ticks.

```cpp
static constexpr std::int64_t MAX_TICKS = 1000000;
std::vector<PriceLevel> bids_(MAX_TICKS);
std::vector<PriceLevel> asks_(MAX_TICKS);
```

This fundamentally transformed how the engine resolves prices. The price tick value is the index of the array. If we send a buy order at a price tick of 420,069, the matching engine will calculate the memory offset instantly: `bids_[420, 069]`

Lookup and insertion are O(1). Removing the memory shuffles, pointer-chasing steps, sorting routines. The memory for every possible price level is allocated exactly once at system startup.

“Of course, maintaining massive static arrays introduces a new engineering hurdle. If an incoming buy order needs to match against the best available ask price, how do we know where the top of the book is without looping through all one million slots?”

Initially, I added logic to search the array sequentially whenever the best price level became empty, and introduced global tracking variables `best_bid_`, `best_ask_` alongside active order counters `active_bid_count_`, `active_ask_count_`

```cpp
if (active_ask_count_ == 0)
{
    best_ask_ = MAX_TICKS; // Instantly short-circuit if the book is empty
}
else if (level.empty() && loc.price_ticks == best_ask_)
{
    while (best_ask_ < MAX_TICKS && asks_[best_ask_].empty())
    {
        best_ask_++; // Fast-forward only across populated gaps
    }
}
```

By tracking live order quantities globally via tracking variables and counters, I aimed to instantly short circuit empty price slots, allowing the pointer to step past blank ranges. 

While this successfully handled basic book clearings, it introduced something I called **“The Empty Book Scan of Doom and Despair”**. When exposed to a highly sparse macro market layout, active price clusters are separated by giant bulks of empty ticks. If a large aggressive order swept the top of the book, the engine was forced to step sequentially through thousands of inactive array indices using a linear pointer increment loop: 

```cpp
while (asks_[best_ask_].empty()) 
{
    best_ask_++;
}
```

This search completely stalled the CPU execution pipeline, accumulating about **47 Billion conditional branches** and **4.6 Billion L1 cache misses** under high event loads.

Furthermore, the benchmarking metrics helped me realise that the testing harness was constructing and destroying the `ob::Engine` instance inside the core iteration loops, forcing the operating system to clear and reallocate 48 Megabytes of memory repeatedly. I thus fixed this and implemented a `reset()` function to clear the active array counters and zero out the index boundaries, while preserving the memory.

To completely eliminate the 47 billion branch loop penalty while maintaining the O(1) lookup, I implemented a Hierarchical Bitmask Index. Instead of verifying array slots one by one, the book tracks populated prices using a two tiered bitset composed of 64bit unsigned integers `std::uint64_t words`:

1) An array where each bit represents an individual price tick. A bit value of 1 indicates an active price level while 0 indicates empty space. **(level 1)**

2) An array where each bit tracks an entire 64-bit word of level 1. **(level 2)**

When a trade or cancellation empties a price level, the state transition flips the bit off. When searching for the next active price, the engine uses the second level to bypass thousands of empty ticks instantly, allowing the CPU to calculate the precise memory offset of the next active price level in a single hardware clock cycle.

Integrating the bitmask showed performance benefits across all testing scripts: 

- Sparse Market `deep_script.txt`: The engine achieved an execution latency of **30ns** per event, down from the previous **643ns**. This is a result of conditional branches dropping from **47 Billion to 590 Million** and the L1 data cache misses dropping from **4.6 Billion to 16 Million**. Comparing that to our original `std::map` engine's **656ns** baseline, demonstrates the great improvement. 

- Dense Market `dense_script.txt`: The optimised engine completed matching sweeps in about **26ns** per event compared to the original engine's **90ns** baseline, proving that removing tree layers gives a roughly **3x** performance increase when operations are clustered. 

- Basic Workload `script.txt`: Possibly as a surprise, the original engine tracks slightly faster on this script (**~58ns vs ~112ns**) due to the zero indexing abstractions under an artificial, warm L1 footprint.

Noting that the optimised engine maintains invariant execution times regardless of whether the book contains a dozen orders or millions of rows.

Optimising for "average execution speed" on clean, isolated loops is a vanity metric. 

What dictates production survival is deterministic tail latency under heavy load. A standard library tree + list architecture degrades logarithmically as volume scales. 

When transaction volumes surge, a single `std::map::insert` node allocation will eventually trigger a kernel memory page fault or heap reorganization. When this occurs, latency will spike from nanoseconds into microseconds. 

(You can have a look, as I left the first iteration on the original-implementation branch). 

In my updated implementation, I isolated the execution pathway entirely from the operating system kernel. This architecture ensures that order throughput remains a flat, predictable timeline under market pressure.

---

## Folder layout From the root: 

src/ Library and app source 
tests/ Googletest suite 
examples/ Sample scripts
build/ Build output (not committed)

--- 

Follow these instructions to compile, verify, and run benchmarks against the matching engine on your local machine.

## System Prerequisites

Before configuring the software, ensure your Linux machine has a modern C++17 compliant compiler, CMake, and Git installed. Run the following command to pull all required system dependencies:

```shell
sudo apt-get install build-essential cmake git
```

## Codebase Cloning: 

The codebase relies on GoogleTest as an external git submodule for its unit-testing suite. You must clone the repository recursively to automatically initialise and pull down the testing framework components:

```shell
git clone --recursive [https://github.com/l1am-farrugia/cppOrderbook.git](https://github.com/l1am-farrugia/cppOrderbook.git)
```

## Engine Compilation:

This repository utilises CMake to manage optimised release builds. Create a isolation compilation directory, configure CMake to build with high level Release optimisations and compile the binaries, utilising all available cores in parallel:

```shell
mkdir build && cd build

cmake -DCMAKE_BUILD_TYPE=Release ..

make -j$(nproc)
```

Upon a successful build, the following native executable binaries will be generated inside your current build/ folder:

ob_tests: The structural unit-testing suite. 
ob_sim: The command line trading simulator, replay verifier, and hot-path benchmarking tool.

## Running the Unit Testing Suite: 

To ensure that the engine's custom memory pools, intrusive lists, and array tracking boundaries are functioning on your machine architecture, execute the native GoogleTest binary:

```shell
./ob_tests
```

## Operating the Engine Simulator: 

The ob_sim binary allows you to feed raw trading scripts into the engine. These script files must be text documents containing one instruction per line. The supported command syntax is as follows:

Add Limit Order: `add <OrderId> <Side> <PriceTicks> <Qty>`
`add 5001 buy 1250 50` ← Places a buy limit order with ID 5001 at a price of 1250 ticks for a volume of 50 units. 

Cancel Order: `cancel <OrderId>`
`cancel 5001` ← Locates resting order 5001 and removes it from the queue. 

## Execution:

To run the standard sequential script with console logging enabled use:

```shell
./ob_sim --script ../examples/script.txt
```
 
To run a high volume, 50,000 message clustered script use:

```shell
./ob_sim --bench ../examples/dense_script.txt --iters 100 
```

To run a high volume, widely distributed 200,000 tick script use: 

```
./ob_sim --bench ../examples/deep_script.txt --iters 100
```

I also recommend prefixing the instructions with: 

```shell
sudo perf stat -e L1-dcache-load-misses,cache-misses,branches,branch-misses 
```

In order to gather more metrics on the CPU. 

*note, keep `--iters` <= 100 for the macro scripts because the simulation's isolation reset routine introduces artificial memory bus overhead that distorts the engine's metrics.

---

## Appendix: Key Definitions & Explanations

### Orderbook
An orderbook is a data structure that aggregates all unexecuted buy and sell orders in a financial market. It is split into bids (buy orders) and asks (sell orders). The system maintains the bids sorted from highest to lowest price, and the asks sorted from lowest to highest price, to track the most competitive market orders.

### Matching Engine
A matching engine is the core software component of an exchange that processes incoming order commands. It applies execution rules, such as First-In, First-Out (FIFO) priority, to determine whether an incoming order can execute immediately against a resting order or if it must be placed into the orderbook.

### Price Tick & Tick Spread
A price tick is the absolute minimum fractional increment by which an asset's price can move on an exchange. The tick spread refers to the physical distance or gap between the highest active buy order (best bid) and the lowest active sell order (best ask). In a highly sparse macro market, this spread can swell to encompass thousands of empty price slots, introducing a structural challenge for contiguous data structures.

### Latency
Latency is the time delay between the receipt of an input command and the generation of the corresponding output event. In this engine, it measures the exact number of nanoseconds required to process an order from the entry boundary to the emission of market events.

### Hot Path
The hot path refers to the specific sequence of instructions within a program that executes frequently during live operations. In a matching engine, this comprises the code responsible for checking prices, updating order quantities, and executing trades. Optimising this path requires removing unnecessary logic to ensure execution times remain minimal.

### Clock Cycle
A clock cycle is the smallest unit of time used by a processor to execute a fundamental instruction, such as adding integers or shifting bits. At a processor frequency of 4.0 GHz, each clock cycle takes exactly 0.25 nanoseconds. Total execution latency is determined by the number of clock cycles a program requires to complete.

### Cache Locality
Cache locality is a property where data items that are accessed close together in time are stored close together in physical memory. Because hardware reads memory in contiguous 64-byte blocks, arranging data sequentially ensures subsequent operations access data that has already been loaded into the cache.

### Cache Hit
A cache hit occurs when the processor requests a specific memory address, and the requested data is already present in the L1 or L2 cache. This allows the processor to retrieve the data immediately, minimising execution time.

### Cache Miss
A cache miss occurs when the processor requests a memory address that is not present in its local caches. The processor must stall its execution pipeline while the data is retrieved from the slower System RAM, which significantly increases latency.

### CPU Prefetcher
The CPU prefetcher is a hardware mechanism that analyzes the memory access patterns of a running program. If it identifies a predictable, linear pattern, it automatically loads upcoming memory blocks from RAM into the L1 cache before the program explicitly requests them.

### Context Switch
A context switch is an operating system operation where the processor stops executing the current application code, saves its execution state, and moves into the OS kernel to perform a system task. This occurs during dynamic memory operations, such as heap allocation, and introduces significant processing delays.

### Red-Black Tree
A Red-Black tree is a self-balancing binary search tree data structure utilised by standard containers like std::map. It provides predictable lookup scaling, but degrades low-latency performance because each element is allocated as an independent node on the heap, forcing non-contiguous memory access.

### Intrusive Linked List
An intrusive linked list is a data structure where the navigation pointers (next and prev) are embedded directly inside the user data structure, rather than inside external container nodes. This design ensures that queue navigation data resides on the same cache line as the order variables.

### Memory Pool 
A memory pool is an optimisation technique where an application allocates a large, contiguous block of memory from the operating system once during system startup. The application manages this pre-allocated block internally, avoiding the use of heap allocation operations during live processing.

### Free List
A free list is a mechanism used to manage inactive elements within a memory pool. When an element is idle, its embedded pointers are used to link it into a chain of unallocated slots. Acquiring an empty slot requires shifting the head pointer forward.

### Direct-Mapped Static Array
A direct-mapped static array is a data structure where an item's value or identifier directly determines its physical index in memory. In this engine, the price tick serves as the array index. This allows the system to compute the exact memory address of a price level using basic arithmetic, eliminating search routines and conditional branches.

### Hierarchical Bitmask Index
A two tiered bitset data structure utilised to track the active state of individual price levels across a broad layout. By packing active states into 64-bit unsigned integer words and utilising summary indexing arrays, it allows the matching engine to query bitwise blocks simultaneously. This design enables the system to completely skip large vacuums of empty space and calculate memory offsets using registers in constant time.

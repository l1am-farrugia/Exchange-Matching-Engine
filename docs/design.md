# Design Notes

## Matching Rules
- Limit orders are matched using price priority then fifo time priority.
- Buy orders match the lowest ask prices first while price <= buy limit.
- Sell orders match the highest bid prices first while price >= sell limit.
- Trades execute at the maker price.
- Partial fills are supported and remaining qty stays resting or becomes resting.

## Data Structures
- Bids and asks are stored in direct-mapped static arrays where the price tick serves as the direct memory index for O(1) lookups.
- Queue priority is managed via intrusive doubly-linked lists by embedding the next and prev pointers directly inside the Order struct to ensure optimal L1 cache line utilisation.
- Active price levels are tracked using a two-tiered hierarchical bitmask index, allowing the engine to instantly bypass empty ticks.
- All order nodes are pre-allocated at startup inside a custom slab memory pool and recycled through an intrusive free list to eliminate runtime heap allocations.
- A master index maps order IDs directly to memory locations to provide O(1) order cancellation.

## Determinism Strategy
- The entire execution footprint is bounded and allocated at system startup to isolate the hot path from operating system memory jitter.
- A reset() routine clears active indices and bitmasks between execution passes while preserving the underlying raw memory capacity.
- Inbound commands are processed sequentially, and outbound market events are emitted in a rigid, single line key-value log format for precise replay verification.

## Invariants
- Index size matches total number of resting orders across all levels.
- The hierarchical bitmask accurately reflects book state: bits are flipped to 1 if a price level has orders, and cleared to 0 when empty.
- All unallocated order structures exist cleanly within the memory pool's free list chain
- All resting orders have qty > 0 and seq != 0.

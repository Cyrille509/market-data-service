# Market data service

Originally built in July 2026; re-uploaded in November 2026.

A C++17 project that builds a BTCUSDT order book from Binance depth updates.
It keeps track of update IDs and rebuilds the book if it misses data. You can
connect to the live feed or replay the included sample data offline.

Live mode only works on Windows. The replay mode, tests, and benchmark are portable.

## Data flow

```text
Binance WebSocket --+
                    +--> parse updates --> sync engine --> order book --> console
Replay file --------+                         ^
                                              |
                             snapshot (REST or file)
```

The sync engine joins the snapshot with incoming updates and checks for gaps.

## Design decisions

- Prices and quantities use scaled integers to avoid floating-point rounding issues.
- `std::map` keeps price levels sorted and makes the best bids and asks easy to read.
  It's a simple starting point, with an allocation cost compared to more compact storage.
- Book updates run on one thread, which keeps update ordering and recovery easier to follow.
- Live data and replay use the same parsing and book logic, so saved data can exercise
  the same code without a network connection.
- When an update is missed, the service fetches a new snapshot and rebuilds the book.
  Continuing with missing data would leave the book unreliable.

## Build

You'll need CMake, Ninja, Git, and a C++17 compiler. The first build downloads simdjson.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Try it

Replay the sample data and check the result:

```powershell
build\mdp replay fixtures\snapshot.json fixtures\depth_frames.jsonl --golden fixtures\golden_top10.json
```

Or connect to Binance on Windows:

```powershell
build\mdp live
```

If Binance.com is unavailable in your region, you can try Binance.US:

```powershell
build\mdp live --rest-host api.binance.us --ws-host stream.binance.us
```

This handles one symbol's order book. There's no trading or order execution.
Benchmark details are in [docs/benchmarks.md](docs/benchmarks.md).

## ADDED Requirements

### Requirement: FEC Encode Pipeline
The system SHALL accumulate packets into batches, encode them via RaptorQ, and emit encoded shards with FEC headers.

#### Scenario: Batch accumulation and encoding
- **WHEN** the encode pipeline receives packets from the input channel
- **THEN** it SHALL accumulate them for at most `timeout_ms` milliseconds
- **AND** it SHALL encode accumulated packets as a RaptorQ block when batch is full or timeout expires

#### Scenario: Symbol generation with adaptive overhead
- **WHEN** encoding a batch of K source symbols
- **THEN** the system SHALL generate `K + ceil(K × overhead)` total symbols (up to 65535)
- **AND** each symbol SHALL be emitted as a separate packet with FEC wire-format header

#### Scenario: Empty batch is skipped
- **WHEN** no packets arrive within `timeout_ms`
- **THEN** the system SHALL skip the encode cycle and wait for new packets

### Requirement: FEC Decode Pipeline
The system SHALL receive FEC shards, buffer them by group, and decode via RaptorQ when enough symbols arrive.

#### Scenario: Successful decode
- **WHEN** a ring buffer slot accumulates `source_count` or more unique shards for a given group_seq
- **THEN** the system SHALL attempt RaptorQ decode and split the recovered blob into original packets
- **AND** each recovered packet SHALL pass through the filter chain before output

#### Scenario: Decode failure
- **WHEN** RaptorQ decode fails (insufficient distinct symbols)
- **THEN** the system SHALL increment the loss failure counter and discard the ring buffer slot

#### Scenario: Decode timeout
- **WHEN** a ring buffer slot's `first_packet_time` exceeds `decode_timeout` (3×RTT_ewma + timeout_ms, min 50ms)
- **THEN** the system SHALL evict the slot and discard all buffered shards

#### Scenario: Ring buffer overflow
- **WHEN** the ring buffer is full and a new group_seq arrives
- **THEN** the system SHALL evict the stalest slot (by timeout or earliest first_packet_time)

### Requirement: Wire Format
The system SHALL use a 4-byte little-endian DWORD header (bits 0-23 = group_seq, bits 24-31 = flags), followed by a 1-byte feedback field, an optional 8-byte echo timestamp, and mode-specific fields.

#### Scenario: FEC data shard header
- **WHEN** a FEC data shard is encoded
- **THEN** the wire format SHALL be: `[DWORD:4B][fb:1B][echo?:8B][shard_index:1B/2B][source_count:1B/2B][IV:1~8B][symbol_data]`
- **AND** shard_index and source_count width (1B vs 2B) is determined by flags bit0

#### Scenario: PING packet
- **WHEN** a PING is sent
- **THEN** the wire format SHALL be: `[DWORD:4B with bit4=1][fb:1B][echo?:8B][payload:8B]`
- **AND** payload SHALL contain the sender's µs timestamp as 8B LE

#### Scenario: FEEDBACK_ONLY packet
- **WHEN** no FEC data has been sent for `feedback_timeout_ms` and a pending echo exists
- **THEN** the wire format SHALL be: `[DWORD:4B with bit5=1][fb:1B][echo:8B]`

#### Scenario: REPEAT packet (single packet redundancy)
- **WHEN** symbol_count = 1 and `repeat_ratio > 0`
- **THEN** the wire format SHALL be: `[DWORD:4B with bit6=1][fb:1B][echo?:8B][IV:1~8B][raw_packet]`
- **AND** `1 + ceil(repeat_ratio)` copies SHALL be sent for the same group_seq

### Requirement: PING/RTT Measurement
The system SHALL periodically send PING packets and measure round-trip time via echo timestamps.

#### Scenario: PING interval
- **WHEN** `ping_interval_ms` elapses since the last PING
- **THEN** the encoder SHALL send a PING packet with the current µs timestamp

#### Scenario: RTT calculation on echo receipt
- **WHEN** a FEEDBACK or data packet with kEcho flag is received and the echo timestamp is nonzero
- **THEN** the decoder SHALL compute `rtt_us = now_us - echo_timestamp`
- **AND** update RTT EWMA as `rtt_ewma = (rtt_ewma × 7 + rtt_us) / 8`

#### Scenario: PING echo relay
- **WHEN** a PING packet is received by the decoder
- **THEN** the decoder SHALL store the PING payload as `pending_feedback_echo` in shared state
- **AND** the encoder SHALL send a FEEDBACK_ONLY packet with that echo on its next cycle

### Requirement: Blob Format
Encoded blobs SHALL use the format `[u32 pkt_count][u16 pkt_len][pkt_data]...` with zero-padding to symbol_size boundary.

#### Scenario: Blob construction
- **WHEN** building a blob from N packets
- **THEN** the blob SHALL contain N as a 4-byte LE count, followed by N entries of (2-byte LE length + data)
- **AND** the blob SHALL be zero-padded so its size is a multiple of symbol_size

#### Scenario: Blob split after decode
- **WHEN** a blob is successfully decoded
- **THEN** the system SHALL read pkt_count, then read each (pkt_len, data) pair to reconstruct original packets

### Requirement: Direction-aware Pipeline
The FecPipeline SHALL operate in either encode or decode mode, determined at construction time.

#### Scenario: Encode mode
- **WHEN** `is_encoder = true`
- **THEN** the pipeline SHALL accumulate packets from input, encode batches via RaptorQ, and write to output

#### Scenario: Decode mode
- **WHEN** `is_encoder = false`
- **THEN** the pipeline SHALL read FEC packets from input, buffer in ring buffer, decode groups, and write recovered packets to output

### Requirement: Lua API
The system SHALL expose `hole.fec_pipeline()` and `hole.fec_shared_state()` in the Lua API.

#### Scenario: Creating FEC pipelines
- **WHEN** Lua calls `hole.fec_pipeline(in, filters, out, cfg, is_encoder, shared)`
- **THEN** the system SHALL create a FecPipeline instance with the given configuration
- **AND** start it as a fiber

#### Scenario: Creating shared state
- **WHEN** Lua calls `hole.fec_shared_state()`
- **THEN** the system SHALL return a FecSharedState object that can be passed to both encode and decode pipelines

### Requirement: Codec Strategy Abstraction
The FEC pipeline SHALL be structured as a transport layer plus a pluggable codec strategy. A codec SHALL be a pure synchronous processor: it SHALL NOT perform I/O, SHALL NOT suspend (no co_await), and SHALL NOT own fibers. All I/O and fiber scheduling SHALL live in the single transport implementation.

#### Scenario: Codec interface
- **WHEN** the transport receives a packet from its input
- **THEN** it SHALL call `codec.OnPacket(packet, out)` synchronously
- **AND** the codec SHALL append all outbound packets to the caller-provided `out` vector

#### Scenario: Codec idle tick
- **WHEN** the transport poll timer fires (100µs) while the queue is idle
- **THEN** the transport SHALL call `codec.Tick(out)` so the codec can flush deferred output (encoder repair batches, decoder watermark recovery)

#### Scenario: Codec selection
- **WHEN** `fec_codec = "rs"`
- **THEN** the pipeline SHALL instantiate the RS codec (Vandermonde GF256)
- **WHEN** `fec_codec = "lcrq"` (default)
- **THEN** the pipeline SHALL instantiate the RaptorQ codec

### Requirement: Transport Read Loop
The transport SHALL keep exactly one pending asynchronous read on the input endpoint at all times, via a dedicated reader fiber feeding a FIFO queue. The input descriptor SHALL NOT be left unregistered with the reactor while the worker processes or writes.

#### Scenario: Reader fiber
- **WHEN** the pipeline starts
- **THEN** a reader fiber SHALL issue the next async read immediately after each read completes
- **AND** SHALL push received packets into a `std::deque` FIFO queue

#### Scenario: Worker fiber
- **WHEN** the queue is non-empty
- **THEN** the worker fiber SHALL pop the front packet (O(1)) and pass it to `codec.OnPacket`
- **AND** SHALL write each packet appended to `out` to the output endpoint

#### Scenario: Idle queue
- **WHEN** the queue is empty and the reader has not finished
- **THEN** the worker SHALL wait on the 100µs poll timer before re-checking, and SHALL call `codec.Tick(out)` each cycle

### Requirement: Container Discipline (hot path)
The FEC hot path SHALL NOT use associative containers (`std::map`), content-based linear scans, or per-packet heap allocation for state indexed by sequence/batch id.

#### Scenario: Decode-side shard storage
- **WHEN** a source shard is cached by sequence number
- **THEN** it SHALL be stored in a fixed-size ring of slots indexed by `seq & (N-1)` (power of two, N >> max in-flight shards)
- **AND** each slot SHALL validate the stored seq and retain its payload vector capacity across packets

#### Scenario: Repair storage
- **WHEN** repairs are buffered by batch id
- **THEN** they SHALL use the same fixed-slot ring pattern indexed by `bid & (N-1)`

#### Scenario: Inter-fiber queues and codec output
- **WHEN** packets move between the reader and worker fibers
- **THEN** the queue SHALL be a `std::deque` (O(1) push_back / pop_front), NOT a vector with front-erase
- **AND** the codec output vector SHALL be reused across calls (clear between calls, capacity retained)

### Requirement: IV XOR Obfuscation
When `obfuscate = true`, the system SHALL XOR data with a random IV before transmission.

#### Scenario: Encode-side obfuscation
- **WHEN** `obfuscate = true` and a shard or REPEAT packet is encoded
- **THEN** `iv_len` random bytes SHALL be generated, XORed with the entire payload, and prepended to the packet
- **AND** the IV length SHALL be encoded in flags bits 1-3

#### Scenario: Decode-side deobfuscation
- **WHEN** `obfuscate = true` and a FEC data or REPEAT packet is received
- **THEN** the system SHALL read IV length from flags bits 1-3, extract the IV, and XOR the payload to recover original data

### Requirement: Configuration Validation
The system SHALL validate FEC configuration at pipeline start and reject invalid configs.

#### Scenario: MTU constraint check
- **WHEN** `symbol_size + max_wire_overhead + IP_UDP_overhead > mtu`
- **THEN** the system SHALL terminate with an error

#### Scenario: IV length validation
- **WHEN** `obfuscate = true` and `iv_len ∉ [1, 8]`
- **THEN** the system SHALL terminate with an error

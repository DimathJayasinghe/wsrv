# Audit of the default resource limits (REQUIREMENTS section 4)

Done 2026-11-26 on `develop` at `7aad5fd`, for the 1.0 release (`docs/RELEASE.md` section 4). Every row of
[REQUIREMENTS section 4](REQUIREMENTS.md) was checked three ways, by reading the code and the tests (nothing
was run for this audit; the test suites run in CI):

1. **Default**: the value in the configuration struct equals the table, and a test asserts it (so a change of the
   default fails a test).
2. **Enforcement**: the code that applies the limit, and the status or close code it answers with, equals the
   "Violation behaviour" column.
3. **Test**: a test drives the violation and checks the answer.

Result: **all 19 limits are enforced with the documented default and answer.** Two rows described the design
differently from the code (rows 10 and 14; the code was the safer side, so the requirements text was corrected),
and three defaults had no test (rows 7 and 16, now asserted). Nothing was found unenforced or missing.

| # | Limit | Default in code | Enforced at (answer) | Test of the violation |
|---|---|---|---|---|
| 1 | Handshake size, 8 KiB | `HttpLimits::max_total_bytes = 8192` | `http/request_parser` (431) | `HttpRequestParser.TotalSizeAtTheLimitIsAcceptedAndOneOverIs431` |
| 2 | Header count, 64 | `HttpLimits::max_headers = 64` | `http/request_parser` (431) | `HttpRequestParser.HeaderCountAtTheLimitIsAcceptedAndOneOverIs431` |
| 3 | Handshake timeout, 5 s | `ServerConfig::handshake_timeout` | `Connection::on_handshake_timeout` (close) | `Integration.ASlowlorisHandshakeIsClosedAtTheTimeoutAndOthersAreServed`, `TlsServer.AClientThatSendsNothingIsClosedAtTheHandshakeTimeout` |
| 4 | Frame payload, 1 MiB | `FrameParserConfig::max_frame_payload` | `frame_parser.cpp` (1009), before the payload arrives | `Session.AnOversizedFrameIsAProtocolErrorBeforeItsPayloadArrives` |
| 5 | Message size, 1 MiB | `AssemblerConfig::max_message_size` | `message_assembler.cpp` (1009) | `Integration.AMessageOverTheLimitClosesWith1009AndTheLimitHasNoOffByOne` |
| 6 | Fragments, 1024 | `AssemblerConfig::max_fragments` | `message_assembler.cpp` (1008) | `MessageAssembler.TooManyFragmentsIsAPolicyViolation` |
| 7 | Incomplete message, 30 s | `SessionConfig::incomplete_message_timeout` | `session.cpp` tick (1008) | `SessionTimers.AnUnfinishedMessageIsAbandonedAfterTheIncompleteTimeout` |
| 8 | Outbound frame, 64 KiB | `OutboundConfig::max_frame_payload` | outbound scheduler (auto-fragmentation) | `ConnectionTest.AWriterSendsOneMessageAsFragments` |
| 9 | Data frames per second, 10 000 | `SessionConfig::max_data_frames_per_second` | `session.cpp` (1008) | `SessionRates.TooManyDataFramesInOneWindowCloseWith1008` |
| 10 | Connections, global, 10 000 | `ServerConfig::max_connections` | `Server` accept path: the TCP connection is accepted and closed at once, no HTTP answer (finding 3) | `Server.TheConnectionCapRefusesTheExtraClientCheaply` |
| 11 | Connections per IP, 100 | `ServerConfig::max_connections_per_ip` | `Server` admission (429) | `IpLimitsServer.*` (`expect_429`), `IpLimitsServer.TheDefaultsAreTheDocumentedOnes` |
| 12 | Handshakes per IP, 50 / s | `ServerConfig::max_new_connections_per_ip_per_second` | `Server` admission (429) | `IpLimitsServer.AFastNewcomerIsRefusedAndForgivenAfterASecond`, `IpLimits.TheBucketAllowsABurstOfOneSecondsWorthThenRefillsAtTheRate` |
| 13 | Outbound queue, 2 MiB (twice `max_message_size`, ADR-0043) | `OutboundConfig::max_queued_bytes`, `slow_consumer = Close` | `Connection` (1008) | `ConnectionTest.ASlowConsumerIsClosedWith1008ByDefault`, `Integration.ASlowReaderIsDroppedAndANeighbourIsUnaffected` |
| 14 | Inbound read buffer, 16 KiB per loop | `kScratchSize` in `loop_shard.cpp`; per connection a bounded `ByteBuffer` (finding 2) | `Connection` read path | `ByteBuffer.MemoryNeverExceedsTheBoundEvenWithConsumedBytesAtTheFront` |
| 15 | Idle timeout, 60 s | `SessionConfig::idle_timeout` | `session.cpp` (1001) | `SessionTimers.TheIdleTimeoutClosesWith1001ExactlyAtTheLimit` |
| 16 | Ping 30 s / pong 10 s | `SessionConfig::ping_interval`, `pong_timeout` | `session.cpp` (1001 on a missing pong) | `SessionTimers.AQuietConnectionIsPingedAndAPongKeepsItAlive`, `Integration.ASilentPeerIsPingedThenClosedByTheHeartbeat` (1 s values) |
| 17 | Close handshake, 5 s | `SessionConfig::close_timeout` | `session.cpp` (force close) | `ConnectionTest.APeerThatNeverAnswersOurCloseIsDroppedAtTheCloseTimeout` |
| 18 | Control frames per second, 100 | `SessionConfig::max_control_frames_per_second` | `session.cpp` (1008) | `SessionRates.TooManyControlFramesInOneWindowCloseWith1008` |
| 19 | Inflate ratio, 100 : 1 | `DeflateOptions::max_inflate_ratio = 100` (`SessionDeflate` too) | `session.cpp` (1009) | `ServerDeflate.ADecompressionBombIsRefusedWith1009AndTheServerCarriesOn` |
| 20 | Bytes buffered by all connections together, 1 GiB (added for 1.1, ADR-0040; not part of the 1.0 audit) | `ServerConfig::max_buffered_bytes = kDefaultMaxBufferedBytes` | `Server::on_accept` (503 + `Retry-After: 1`, before a per-IP slot) | `BufferBudgetServer.AServerOverItsBudgetShedsNewConnectionsAndLeavesTheOnesItHas`; the default and both range edges: `ServerConfig.TheBufferedBytesBudgetHasABoundedDefaultAndIsRangeChecked` |

## Findings

1. **Three defaults were not asserted by any test** (rows 7 and 16: the incomplete-message timeout, the ping
   interval and the pong timeout). The enforcement tests set their own values, so a changed default would have
   passed every test. Fixed: `ServerConfig.DefaultsMatchTheRequirementsTable` now asserts them.
2. **Row 14 described a design the code does not have, and the code is leaner.** The table said "64 KiB per
   connection". The code reads into **one 16 KiB buffer per loop, shared by all connections** (`loop_shard.cpp`,
   `kScratchSize`), and each connection's own `ByteBuffer` holds only the unfinished part of a frame: nothing is
   reserved up front, it is bounded by the frame limit, and it gives back capacity above 16 KiB when drained
   (`base/byte_buffer.hpp`). The memory bound is tighter than the row promised. **Applied** (owner, 2026-11-26):
   REQUIREMENTS section 4 now describes this.
3. **Row 10 said "refuse accept / 503"; the code does the first half only.** Over the global cap the server
   accepts the TCP connection and closes it at once, without an HTTP answer. That is the cheapest refusal and
   matches FR-S1 ("refuse further with minimal cost"); a 503 is sent only when the per-IP table is full, and for
   a handshake the application decider did not answer. **Applied** (owner, 2026-11-26): the row now says "refused
   at accept: closed at once, no answer".

No limit was found without enforcement, with a different default, or with a different close code.

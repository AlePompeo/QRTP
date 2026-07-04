Example 1: Code Review Command
File: .claude/commands/code-review.md  # Code Review Command

Carefully perform a comprehensive code review of $ARGUMENTS.

## Review Standards
Examples of excellent code that you should match the design/style/conventions of:
- src/protocol.cpp (connection state machine)
- src/framing.cpp (frame header pack/unpack)
- src/adaptive.cpp (AIMD controller)

## Process
1. *First*: Read the example files above to understand our design patterns, naming conventions, and code style
2. *Second*: Analyze $ARGUMENTS against these standards
3. *Third*: Create detailed critique covering:
   - Code structure and organization
   - Adherence to established patterns
   - Protocol correctness (sequence numbers, window bounds, ACK/SACK semantics)
   - Performance considerations (camera/display loop hot paths)
   - Security implications (untrusted frame parsing from an optical channel)
   - Maintainability concerns
   - Test coverage gaps (loopback + synthetic loss/corruption cases)

## Output Requirements
- Save review as ai-code-reviews/{filename}.review.md for each file reviewed
- Include specific line references for issues
- Provide concrete suggestions for improvements
- Rate overall quality: Excellent/Good/Needs Improvement/Poor
- Estimate refactoring effort: Low/Medium/High

## Review Checklist
- Follows project naming conventions
- Proper error handling for malformed/corrupt frames (never trust a decoded QR payload blindly — `decode_frame` must reject before any field is used)
- Byte-offset/sequence wraparound (mod 2^32) handled correctly in comparisons and window math
- No busy-loops in the capture/display hot path — actual blocking waits (condition variables, camera read), not polling spins
- No hardcoded values, secrets, or magic numbers (thresholds belong in `AdaptiveConfig`/`ProtocolConfig` defaults, not scattered literals)
- Camera/display resources are always released, even on exception (RAII destructors, not manual open/close)
- No obvious security vulnerabilities (frame payload length must be bounds-checked before use — it comes from an untrusted external source, treat it like network input)
- Any header-only change is confirmed to actually trigger a rebuild (`make`'s `-MMD -MP` dependency tracking) before trusting a "fix" — see `not_working.md` for how this bit us once
- Appropriate comments and documentation
- Follows existing design principles and consistent with exemplars
- Performance optimizations considered, but not at the cost of protocol correctness

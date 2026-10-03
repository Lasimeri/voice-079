#!/bin/sh
# One MCP tool call to phi-stream mcp over stdio: mcp-call.sh TOOL [JSON-ARGS]
# Prints the tool's text (the screen for screen/type/keys).
B="$HOME/Intel Phi Stream/target/release/phi-stream"
tool="$1"; args="${2:-{\}}"
printf '%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"mcp-call","version":"0"}}}' \
  "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"$tool\",\"arguments\":$args}}" \
  | "$B" mcp | tail -1 | jq -r '.result.content[0].text // .error.message'

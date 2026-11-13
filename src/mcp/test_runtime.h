#pragma once
#ifdef DARKSPEAK_WITH_TEST_MCP
namespace ds::mcp {
bool testRuntimeRequested(int argc, char* argv[]);
int runTestRuntime(int argc, char* argv[]);
} // namespace ds::mcp
#endif

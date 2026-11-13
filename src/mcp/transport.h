#pragma once
#include <QString>
#include <memory>
namespace ds::mcp {
class Service;
#ifdef DARKSPEAK_WITH_TEST_MCP
class TestSupport;
#endif
class Transport final {
  public:
    Transport(Service& service, const QString& address, int port
#ifdef DARKSPEAK_WITH_TEST_MCP
              ,
              TestSupport* controller = nullptr
#endif
    );
    ~Transport();
    void start();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ds::mcp

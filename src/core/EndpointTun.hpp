#pragma once

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>

#include "Endpoint.hpp"

namespace gh {

class FecStats;

class Tun : public Endpoint {
public:
  Tun(boost::asio::io_context& io_context, std::string const& name);

  Omni::Fiber::Coroutine<ErrorCode> Read(Packet& p, Cancel&) override;
  ErrorCode TryRead(Packet& p) override;
  Omni::Fiber::Coroutine<ErrorCode> Write(Packet& p, Cancel&) override;

  // 统计系统注入 (可空, 由 lua stats 配置后调用)
  void SetStats(FecStats* stats) { _Stats = stats; }

protected:
  std::string GetName() const override;
  Omni::Fiber::Coroutine<ErrorCode> DoStart() override;
  Omni::Fiber::Coroutine<ErrorCode> DoGracefulStop() override;

private:
  boost::asio::posix::stream_descriptor _TunFileDescriptor;
  const std::string _Name;
  FecStats* _Stats = nullptr;
};

} // namespace gh

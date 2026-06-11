#include "EndpointTun.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <linux/if_tun.h>

#include "Asio.hpp"
#include "ErrorCode.hpp"

namespace gh {

Tun::Tun(boost::asio::io_context& io_context, std::string const& name) : _TunFileDescriptor(io_context), _Name(name) {}

std::string Tun::GetName() const { return "Tun:" + _Name; }

Omni::Fiber::Coroutine<ErrorCode> Tun::DoStart() {
  int fd = ::open("/dev/net/tun", O_RDWR);
  if (fd < 0) {
    co_return ErrorCode(errno, system_category());
  }

  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, _Name.c_str(), IFNAMSIZ);
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  if (::ioctl(fd, TUNSETIFF, (void*)&ifr) < 0) {
    ::close(fd);
    co_return ErrorCode(errno, system_category());
  }

  _TunFileDescriptor.assign(fd);
  _TunFileDescriptor.non_blocking(true);
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> Tun::DoGracefulStop() {
  co_await _PipielineUsageCounter.WaitAll();
  _TunFileDescriptor.close();
  co_return ErrorCode{};
}

Omni::Fiber::Coroutine<ErrorCode> Tun::Read(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  auto [err, bytes_transferred] = co_await _TunFileDescriptor.async_read_some(
      boost::asio::mutable_buffer(p),
      boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  p._Length = bytes_transferred;
  co_return err;
}

Omni::Fiber::Coroutine<ErrorCode> Tun::Write(Packet& p, Cancel& c) {
  if (c.IsTriggered()) {
    co_return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
  }
  auto [err, bytes_transferred] = co_await _TunFileDescriptor.async_write_some(
      boost::asio::const_buffer(p),
      boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
  assert(p._Length == bytes_transferred);
  co_return err;
}


ErrorCode Tun::TryRead(Packet& p) {
  boost::system::error_code ec;
  std::size_t n = _TunFileDescriptor.read_some(boost::asio::mutable_buffer(p), ec);
  if (ec) {
    if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
      return ErrorCode{AppErrorCategory::kOperationAborted, kAppError};
    }
    if (ec == boost::asio::error::eof) {
      return ErrorCode{AppErrorCategory::kEndOfStream, kAppError};
    }
    return ErrorCode(ec.value(), system_category());
  }
  p._Length = n;
  return ErrorCode{};
}

} // namespace gh

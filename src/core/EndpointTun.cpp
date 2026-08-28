#include "EndpointTun.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/log/trivial.hpp>
#include <linux/if_tun.h>

#include "Asio.hpp"
#include "ErrorCode.hpp"
#include "FecStats.hpp"

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
  // tun 非阻塞写可能部分写入或 would_block (队列满): 必须循环写到完,
  // 否则尾段静默丢失 (生产 NDEBUG 编译, 旧 assert 失效 = 静默截断,
  // 这是真实丢包/数据损坏路径, 2026-08-29 由 fec-test2 调试构建触发)
  std::size_t done = 0;
  while (done < p._Length) {
    auto span = p.Data();
    auto [err, bytes_transferred] = co_await _TunFileDescriptor.async_write_some(
        boost::asio::const_buffer(span.data() + done, span.size() - done),
        boost::asio::bind_cancellation_slot(c.AsioSlot().Slot(), Omni::Fiber::AsioUseFiber));
    if (err) {
      // EAGAIN = 队列满等待 (不是失败, 但反映压力); 其余错误 = 真实失败
      if (err == boost::asio::error::would_block ||
          err == boost::asio::error::try_again) {
        if (_Stats) _Stats->WriteEagain();
        continue;  // 等下一个可写 edge (EPOLLET: 重新注册, 队列排空后触发)
      }
      if (_Stats) _Stats->WriteFail(err.value(), p._Length);
      co_return err;
    }
    if (bytes_transferred == 0) {
      // 无进度且无错误: 防御死循环 (tun 上理论不发生)
      if (_Stats) _Stats->WriteFail(0, p._Length);
      co_return ErrorCode{boost::asio::error::eof};
    }
    done += bytes_transferred;
    if (_Stats && done < p._Length) _Stats->WritePartial();
  }
  co_return ErrorCode{};
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

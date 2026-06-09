#pragma once

#include <memory>
#include <vector>

#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "Endpoint.hpp"
#include "ErrorCode.hpp"
#include "Filter.hpp"
#include "Service.hpp"

#include <boost/asio/io_context.hpp>

namespace gh {

class Pipeline : public std::enable_shared_from_this<Pipeline>, public Service {
public:
  Pipeline(boost::asio::io_context& io, std::shared_ptr<EndpointInput> in,
           const std::vector<std::shared_ptr<Filter>>& filters,
           std::shared_ptr<EndpointOutput> out);
  ~Pipeline() override {}

  Omni::Fiber::Coroutine<ErrorCode> Start() override;
  Omni::Fiber::Coroutine<ErrorCode> Stop() override;

protected:
  bool IsCritical(const ErrorCode& ec);

  virtual Omni::Fiber::Coroutine<void> Process();

  boost::asio::io_context& _Io;
  std::shared_ptr<EndpointInput> _In;
  std::shared_ptr<EndpointOutput> _Out;
  std::vector<std::shared_ptr<Filter>> _Filters;
  Cancel _Stop;
};

} // namespace gh

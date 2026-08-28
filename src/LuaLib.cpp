#include "LuaLib.hpp"

#include <array>
#include <memory>

#include <boost/asio/ip/address_v6.hpp>
#include <lua.hpp>

#include "Coroutine.hpp"
#include "EndpointTun.hpp"
#include "EndpointUdp.hpp"
#include "EndpointUdpDynMux.hpp"
#include "EndpointUdpMux.hpp"
#include "ErrorCode.hpp"
#include "FilterXor.hpp"
#include "LuaInterface.hpp"
#include "Pipeline.hpp"
#include "FecConfig.hpp"
#include "FecPipeline.hpp"
#include "FecStats.hpp"
#include "ResolverCombinedEndpoint.hpp"
#include "ResolverHelper.hpp"

// IMPORTANT NOTE:
// Stack-Unwinding Yield Leak: C++ local variables (like std::shared_ptr<ResolverEndpoint>) inside yielding Lua C API
// functions. Since lua_yield uses a longjmp mechanism to suspend the Lua thread, standard C++ stack unwinding is
// bypassed, leaking any active local C++ objects in the function.

namespace gh {

static boost::asio::ip::address_v6 get_address(const char* str) {
  auto address = boost::asio::ip::make_address(str);
  if (address.is_v4()) {
    return boost::asio::ip::make_address_v6(boost::asio::ip::v4_mapped_t(), address.to_v4());
  } else {
    return address.to_v6();
  }
}

template <int f(lua_State* L)> static int safe_call(lua_State* L) {
  try {
    return f(L);
  } catch (std::exception const& e) {
    return luaL_error(L, e.what());
  }
}

template <void f(lua_State* L)> static int safe_yield(lua_State* L) {
  try {
    f(L);
    return lua_yield(L, 0);
  } catch (std::exception const& e) {
    return luaL_error(L, e.what());
  }
}

constexpr const char name_pipeline[] = "Hole.pipeline";
constexpr const char name_endpoint[] = "Hole.endpoint";
constexpr const char name_filter[] = "Hole.filter";
constexpr const char name_udp[] = "Hole.udp";
constexpr const char name_udp_mux_server[] = "Hole.udp-mux-server";
constexpr const char name_fec_shared_state[] = "Hole.fec-shared-state";
constexpr const char name_fec_pipeline[] = "Hole.fec-pipeline";
constexpr const char name_udp_dyn_mux[] = "Hole.udp-dyn-mux";
constexpr const char name_fec_stats[] = "Hole.fec-stats";

template <typename T, const char N[]> static int gc(lua_State* L) {
  typedef std::shared_ptr<T> P;
  ((P*)luaL_checkudata(L, 1, N))->~P();
  return 0;
}

static const struct luaL_Reg filter_metatable[] = {{"__gc", safe_call<gc<Filter, name_filter>>}, {NULL, NULL}};

static int filter_xor_new(lua_State* L) {
  auto c = lua_gettop(L);
  if (c != 1) {
    return luaL_error(L, "filter_xor: not enough arguments");
  }

  size_t len;
  auto s = lua_tolstring(L, 1, &len);
  if (len < 32 || s == NULL) {
    return luaL_error(L, "filter_xor: malformed xor key");
  }

  new (lua_newuserdata(L, sizeof(std::shared_ptr<Filter>)))
      std::shared_ptr<Filter>(new FilterXor(std::vector<char>(s, s + len)));
  luaL_getmetatable(L, name_filter);
  lua_setmetatable(L, -2);
  return 1;
}

static void pipeline_stop(lua_State* L) {
  auto& pipe = *(std::shared_ptr<Pipeline>*)luaL_checkudata(L, 1, name_pipeline);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, pipe](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await pipe->Stop();
    co_return 0;
  });
}

static auto pipeline_metatable = std::to_array<const struct luaL_Reg>({
    {.name = "__gc", .func = safe_call<gc<Pipeline, name_pipeline>>},
    {.name = "stop", .func = safe_yield<pipeline_stop>},
    {.name = NULL, .func = NULL},
});

static void pipeline_new(lua_State* L) {
  auto c = lua_gettop(L);
  if (c < 2) {
    throw std::runtime_error("pipeline: not enough arguments");
  }

  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<EndpointInput> in = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, 1, name_endpoint);
  std::vector<std::shared_ptr<Filter>> filters(c - 2);
  for (auto i = 2; i < c; ++i) {
    filters[i - 2] = *(std::shared_ptr<Filter>*)luaL_checkudata(L, i, name_filter);
  }
  std::shared_ptr<EndpointOutput> out = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, c, name_endpoint);

  auto pipe = new (lua_newuserdata(L, sizeof(std::shared_ptr<Pipeline>)))
      std::shared_ptr<Pipeline>(new Pipeline(interface.GetContext(), in, filters, out));
  luaL_getmetatable(L, name_pipeline);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, pipe](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*pipe)->Start();
    if (err) {
      throw boost::system::system_error(err, "pipeline start error");
    }
    co_return 1;
  });
}

// =========================== fec_shared_state ===========================
static void fec_shared_state_new(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  new (lua_newuserdata(L, sizeof(std::shared_ptr<FecSharedState>)))
      std::shared_ptr<FecSharedState>(new FecSharedState());
  luaL_getmetatable(L, name_fec_shared_state);
  lua_setmetatable(L, -2);

  // Schedule a trivial fiber to balance lua_yield in safe_yield
  interface.Schedule([](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_return 1;
  });
}

static auto fec_shared_state_metatable = std::to_array<const struct luaL_Reg>({
    {.name = "__gc", .func = safe_call<gc<FecSharedState, name_fec_shared_state>>},
    {.name = NULL, .func = NULL},
});

// =========================== fec_pipeline ===========================
static void fec_pipeline_stop(lua_State* L) {
  auto& pipe = *(std::shared_ptr<FecPipeline>*)luaL_checkudata(L, 1, name_fec_pipeline);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, pipe](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await pipe->Stop();
    co_return 0;
  });
}

static auto fec_pipeline_metatable = std::to_array<const struct luaL_Reg>({
    {.name = "__gc", .func = safe_call<gc<FecPipeline, name_fec_pipeline>>},
    {.name = "stop", .func = safe_yield<fec_pipeline_stop>},
    {.name = NULL, .func = NULL},
});

// =========================== fec_stats (LLM-CSV 统计系统) ===========================
static auto fec_stats_metatable = std::to_array<const struct luaL_Reg>({
    {.name = "__gc", .func = safe_call<gc<FecStats, name_fec_stats>>},
    {.name = NULL, .func = NULL},
});

static void fec_stats_new(lua_State* L) {
  // hole.fec_stats({ enabled=true, interval_ms=60000, csv=true, csv_dir=..., ... })
  // 返回共享统计实例; 测试配置启用, 生产不创建 (默认 disabled 不影响数据面)
  FecStatsConfig sc;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "enabled");
    if (lua_isboolean(L, -1)) sc.enabled = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "interval_ms");
    if (lua_isinteger(L, -1)) sc.interval_ms = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "csv");
    if (lua_isboolean(L, -1)) sc.csv = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "csv_dir");
    if (lua_isstring(L, -1)) sc.csv_dir = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "csv_keep_days");
    if (lua_isinteger(L, -1)) sc.csv_keep_days = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "sys_interface");
    if (lua_isstring(L, -1)) sc.sys_interface = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "event_log");
    if (lua_isboolean(L, -1)) sc.event_log = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "ring_buffer_sec");
    if (lua_isinteger(L, -1)) sc.ring_buffer_sec = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  new (lua_newuserdata(L, sizeof(std::shared_ptr<FecStats>)))
      std::shared_ptr<FecStats>(std::make_shared<FecStats>(sc));
  luaL_getmetatable(L, name_fec_stats);
  lua_setmetatable(L, -2);
}

static void fec_pipeline_new(lua_State* L) {
  auto c = lua_gettop(L);
  if (c < 3) {
    throw std::runtime_error("fec_pipeline: not enough arguments");
  }

  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<EndpointInput> in = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, 1, name_endpoint);

  // Parse filters from table at index 2
  luaL_checktype(L, 2, LUA_TTABLE);
  std::vector<std::shared_ptr<Filter>> filters;
  int filter_count = (int)lua_rawlen(L, 2);
  for (int i = 1; i <= filter_count; i++) {
    lua_rawgeti(L, 2, i);
    filters.push_back(*(std::shared_ptr<Filter>*)luaL_checkudata(L, -1, name_filter));
    lua_pop(L, 1);
  }

  std::shared_ptr<EndpointOutput> out = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, 3, name_endpoint);

  // Parse FecConfig from table at index 4 (optional)
  FecConfig cfg;
  bool is_encoder = true;
  if (c >= 4 && lua_istable(L, 4)) {
    lua_getfield(L, 4, "fec_codec");
    if (lua_isstring(L, -1)) cfg.fec_codec = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "timeout_ms");
    if (lua_isinteger(L, -1)) cfg.timeout_ms = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "overhead");
    if (lua_isnumber(L, -1)) cfg.overhead = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "max_overhead");
    if (lua_isnumber(L, -1)) cfg.max_overhead = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "repeat_ratio");
    if (lua_isnumber(L, -1)) cfg.repeat_ratio = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "repeat_ratio_min");
    if (lua_isnumber(L, -1)) cfg.repeat_ratio_min = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "repeat_ratio_max");
    if (lua_isnumber(L, -1)) cfg.repeat_ratio_max = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "symbol_size");
    if (lua_isinteger(L, -1)) cfg.symbol_size = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "mtu");
    if (lua_isinteger(L, -1)) cfg.mtu = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "max_batch");
    if (lua_isinteger(L, -1)) cfg.max_batch = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "obfuscate");
    if (lua_isboolean(L, -1)) cfg.obfuscate = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "iv_len");
    if (lua_isinteger(L, -1)) cfg.iv_len = (uint8_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "decode_window");
    if (lua_isinteger(L, -1)) cfg.decode_window = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "ping_interval_ms");
    if (lua_isinteger(L, -1)) cfg.ping_interval_ms = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "feedback_timeout_ms");
    if (lua_isinteger(L, -1)) cfg.feedback_timeout_ms = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "feedback_stale_ms");
    if (lua_isinteger(L, -1)) cfg.feedback_stale_ms = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "ping_loss_threshold");
    if (lua_isinteger(L, -1)) cfg.ping_loss_threshold = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "decode_timeout_ms");
    if (lua_isinteger(L, -1)) cfg.decode_timeout_ms = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    // === Adaptive algorithm ===
    lua_getfield(L, 4, "algo");
    if (lua_isinteger(L, -1)) cfg.algo = (uint8_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "loss_window_groups");
    if (lua_isinteger(L, -1)) cfg.loss_window_groups = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "loss_alpha");
    if (lua_isnumber(L, -1)) cfg.loss_alpha = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "safety_margin");
    if (lua_isnumber(L, -1)) cfg.safety_margin = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "loss_deadband");
    if (lua_isnumber(L, -1)) cfg.loss_deadband = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    // === Controllable packet loss ===
    lua_getfield(L, 4, "test_drop_pattern");
    if (lua_isinteger(L, -1)) cfg.test_drop_pattern = (uint8_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "test_drop_rate");
    if (lua_isnumber(L, -1)) cfg.test_drop_rate = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "test_drop_rate2");
    if (lua_isnumber(L, -1)) cfg.test_drop_rate2 = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 4, "test_drop_burst");
    if (lua_isinteger(L, -1)) cfg.test_drop_burst = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 4, "is_encoder");
    if (lua_isboolean(L, -1)) is_encoder = lua_toboolean(L, -1);
    lua_pop(L, 1);
  }
  if (c >= 5 && lua_isboolean(L, 5)) {
    is_encoder = lua_toboolean(L, 5);
  }

  // Parse optional fec_shared_state at index 6
  std::shared_ptr<FecSharedState> shared;
  if (c >= 6 && lua_isuserdata(L, 6)) {
    shared = *(std::shared_ptr<FecSharedState>*)luaL_checkudata(L, 6, name_fec_shared_state);
  }

  // Parse optional fec_stats at index 7 (统计系统, 由 hole.fec_stats 创建)
  if (c >= 7 && lua_isuserdata(L, 7)) {
    auto st = *(std::shared_ptr<FecStats>*)luaL_checkudata(L, 7, name_fec_stats);
    cfg.stats = st;
    // 注入 stats 到 in/out (Tun / UdpDynMux::Channel)
    if (st) {
      if (auto* tun = dynamic_cast<Tun*>(in.get())) tun->SetStats(st.get());
      if (auto* tun = dynamic_cast<Tun*>(out.get())) tun->SetStats(st.get());
      if (auto* ch = dynamic_cast<UdpDynMux::Channel*>(in.get())) ch->SetStats(st.get());
      if (auto* ch = dynamic_cast<UdpDynMux::Channel*>(out.get())) ch->SetStats(st.get());
    }
  }

  auto pipe = new (lua_newuserdata(L, sizeof(std::shared_ptr<FecPipeline>)))
      std::shared_ptr<FecPipeline>(new FecPipeline(interface.GetContext(), in, filters, out, cfg, is_encoder, shared));
  luaL_getmetatable(L, name_fec_pipeline);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, pipe](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*pipe)->Start();
    if (err) {
      throw boost::system::system_error(err, "fec_pipeline start error");
    }
    co_return 1;
  });
}
static void endpoint_stop(lua_State* L) {

  auto& ep = *(std::shared_ptr<Endpoint>*)luaL_checkudata(L, 1, name_endpoint);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, ep](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await ep->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg endpoint_metatable[] = {
    {"__gc", safe_call<gc<Endpoint, name_endpoint>>}, {"stop", safe_yield<endpoint_stop>}, {NULL, NULL}};

static void udp_create_channel(lua_State* L) {
  auto top = lua_gettop(L);
  if (top < 2 || top > 3) {
    throw std::runtime_error("udp_create_channel: invalid number of arguments");
  }

  auto& u = *(std::shared_ptr<Udp>*)luaL_checkudata(L, 1, name_udp);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<ResolverEndpoint> resolver;
  if (top == 2) {
    auto input = luaL_checkstring(L, 2);
    resolver = FindResolverEndpoint(input, u->GetResolveFor());
  } else {
    auto host = luaL_checkstring(L, 2);
    luaL_checkany(L, 3);
    auto port = lua_tostring(L, 3);
    if (!port) {
      throw std::runtime_error("udp_create_channel: port must be a number or a string");
    }
    resolver = std::make_shared<ResolverCombinedEndpoint>(FindResolverIp(host, u->GetResolveFor()),
                                                          FindResolverPort(port, u->GetResolveFor()));
  }

  auto ch = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>();
  luaL_getmetatable(L, name_endpoint);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, u, resolver = std::move(resolver), ch](this auto self, lua_State* L,
                                                                         int nres) -> Omni::Fiber::Coroutine<int> {
    *ch = co_await u->CreateChannel(resolver);
    co_return 1;
  });
}

static void udp_stop(lua_State* L) {
  auto& u = *(std::shared_ptr<Udp>*)luaL_checkudata(L, 1, name_udp);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg udp_metatable[] = {{"__gc", safe_call<gc<Udp, name_udp>>},
                                                {"create_channel", safe_yield<udp_create_channel>},
                                                {"stop", safe_yield<udp_stop>},
                                                {NULL, NULL}};

static void udp_new(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<Udp>* udp;
  switch (lua_gettop(L)) {
  case 0:
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<Udp>))) std::shared_ptr<Udp>(new Udp(interface.GetContext()));
    break;
  case 1: {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<Udp>)))
        std::shared_ptr<Udp>(new Udp(interface.GetContext(), bind));
    break;
  }
  default:
    throw std::runtime_error("udp: not enough arguments");
  }

  luaL_getmetatable(L, name_udp);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start();
    if (err) {
      throw boost::system::system_error(err, "udp start error");
    }
    co_return 1;
  });
}

// udp-mux-server
static void udp_mux_server_create_channel(lua_State* L) {
  auto top = lua_gettop(L);
  if (top < 2 || top > 4) {
    throw std::runtime_error("udp_mux_server_create_channel: invalid number of arguments");
  }

  auto id = (uint8_t)luaL_checknumber(L, 2);
  auto& u = *(std::shared_ptr<UdpMux>*)luaL_checkudata(L, 1, name_udp_mux_server);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<ResolverEndpoint> resolver = nullptr;
  if (top == 3) {
    auto input = luaL_checkstring(L, 3);
    resolver = FindResolverEndpoint(input, u->GetResolveFor());
  } else if (top == 4) {
    auto host = luaL_checkstring(L, 3);
    luaL_checkany(L, 4);
    auto port = lua_tostring(L, 4);
    if (!port) {
      throw std::runtime_error("udp_mux_server_create_channel: port must be a number or a string");
    }
    resolver = std::make_shared<ResolverCombinedEndpoint>(FindResolverIp(host, u->GetResolveFor()),
                                                          FindResolverPort(port, u->GetResolveFor()));
  }

  interface.Schedule([&interface, u, id, resolver = std::move(resolver)](this auto self, lua_State* L,
                                                                         int nres) -> Omni::Fiber::Coroutine<int> {
    auto ch = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>();
    luaL_getmetatable(L, name_endpoint);
    lua_setmetatable(L, -2);

    if (resolver) {
      *ch = co_await u->CreateChannel(id, resolver);
    } else {
      *ch = co_await u->CreateChannel(id);
    }
    co_return 1;
  });
}

static void udp_mux_server_stop(lua_State* L) {
  auto& u = *(std::shared_ptr<UdpMux>*)luaL_checkudata(L, 1, name_udp_mux_server);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg udp_mux_server_metatable[] = {
    {"__gc", safe_call<gc<UdpMux, name_udp_mux_server>>},
    {"create_channel", safe_yield<udp_mux_server_create_channel>},
    {"stop", safe_yield<udp_mux_server_stop>},
    {NULL, NULL}};

static void udp_mux_server_new(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<UdpMux>* udp;
  switch (lua_gettop(L)) {
  case 0:
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpMux>)))
        std::shared_ptr<UdpMux>(new UdpMux(interface.GetContext()));
    break;
  case 1: {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpMux>)))
        std::shared_ptr<UdpMux>(new UdpMux(interface.GetContext(), bind));
    break;
  }
  default:
    throw std::runtime_error("udp_mux_server: not enough arguments");
  }

  luaL_getmetatable(L, name_udp_mux_server);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start();
    if (err) {
      throw boost::system::system_error(err, "udp_mux_server start error");
    }
    co_return 1;
  });
}

// udp-dyn-mux
static void udp_dyn_mux_create_channel(lua_State* L) {
  auto top = lua_gettop(L);
  if (top < 2 || top > 4) {
    throw std::runtime_error("udp_dyn_mux_create_channel: invalid number of arguments");
  }

  size_t len;
  auto s = luaL_checklstring(L, 2, &len);
  if (len != 16) {
    throw std::runtime_error("udp_dyn_mux_create_channel: psk must be exactly 16 bytes");
  }
  UdpDynMux::PskType psk;
  std::copy_n(reinterpret_cast<const uint8_t*>(s), 16, psk.begin());

  auto& u = *(std::shared_ptr<UdpDynMux>*)luaL_checkudata(L, 1, name_udp_dyn_mux);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  std::shared_ptr<ResolverEndpoint> resolver = nullptr;
  if (top == 3) {
    auto input = luaL_checkstring(L, 3);
    resolver = FindResolverEndpoint(input, u->GetResolveFor());
  } else if (top == 4) {
    auto host = luaL_checkstring(L, 3);
    luaL_checkany(L, 4);
    auto port = lua_tostring(L, 4);
    if (!port) {
      throw std::runtime_error("udp_dyn_mux_create_channel: port must be a number or a string");
    }
    resolver = std::make_shared<ResolverCombinedEndpoint>(FindResolverIp(host, u->GetResolveFor()),
                                                          FindResolverPort(port, u->GetResolveFor()));
  }

  auto ch = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>))) std::shared_ptr<Endpoint>();
  luaL_getmetatable(L, name_endpoint);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, u, psk, resolver = std::move(resolver), ch](this auto self, lua_State* L,
                                                                         int nres) -> Omni::Fiber::Coroutine<int> {
    if (resolver) {
      *ch = co_await u->CreateChannel(psk, resolver);
    } else {
      *ch = co_await u->CreateChannel(psk);
    }
    co_return 1;
  });
}

static void udp_dyn_mux_stop(lua_State* L) {
  auto& u = *(std::shared_ptr<UdpDynMux>*)luaL_checkudata(L, 1, name_udp_dyn_mux);
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface, u](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await u->Stop();
    co_return 0;
  });
}

static const struct luaL_Reg udp_dyn_mux_metatable[] = {
    {"__gc", safe_call<gc<UdpDynMux, name_udp_dyn_mux>>},
    {"create_channel", safe_yield<udp_dyn_mux_create_channel>},
    {"stop", safe_yield<udp_dyn_mux_stop>},
    {NULL, NULL}};

static void udp_dyn_mux_new(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));
  std::shared_ptr<UdpDynMux>* udp;
  switch (lua_gettop(L)) {
  case 0:
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpDynMux>)))
        std::shared_ptr<UdpDynMux>(new UdpDynMux(interface.GetContext()));
    break;
  case 1: {
    auto bind = boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v6(), (int)lua_tonumber(L, 1));
    udp = new (lua_newuserdata(L, sizeof(std::shared_ptr<UdpDynMux>)))
        std::shared_ptr<UdpDynMux>(new UdpDynMux(interface.GetContext(), bind));
    break;
  }
  default:
    throw std::runtime_error("udp_dyn_mux: not enough arguments");
  }

  luaL_getmetatable(L, name_udp_dyn_mux);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, udp](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*udp)->Start();
    if (err) {
      throw boost::system::system_error(err, "udp_dyn_mux start error");
    }
    co_return 1;
  });
}

// =========================== tun ===========================
static void tun_new(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  if (lua_gettop(L) != 1) {
    throw std::runtime_error("tun: not enough arguments");
  }

  auto tun = new (lua_newuserdata(L, sizeof(std::shared_ptr<Endpoint>)))
      std::shared_ptr<Endpoint>(new Tun(interface.GetContext(), lua_tostring(L, 1)));
  luaL_getmetatable(L, name_endpoint);
  lua_setmetatable(L, -2);

  interface.Schedule([&interface, tun](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    ErrorCode err = co_await (*tun)->Start();
    if (err) {
      throw boost::system::system_error(err, "tun start error");
    }
    co_return 1;
  });
}

static void hole_wait_for_exit(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  interface.Schedule([&interface](this auto self, lua_State* L, int nres) -> Omni::Fiber::Coroutine<int> {
    co_await interface.GetStopSignal();
    co_return 0;
  });
}

// =========================== pipeline ===========================
static auto hole = std::to_array<const struct luaL_Reg>({
    {.name = "filter_xor", .func = safe_call<filter_xor_new>},
    {.name = NULL, .func = NULL},
});

// =========================== pipe io object ===========================
static auto hole_io_object = std::to_array<const struct luaL_Reg>({
    {.name = "wait_for_exit", .func = safe_yield<hole_wait_for_exit>},
    {.name = "pipeline", .func = safe_yield<pipeline_new>},
    {.name = "tun", .func = safe_yield<tun_new>},
    {.name = "udp", .func = safe_yield<udp_new>},
    {.name = "udp_mux_server", .func = safe_yield<udp_mux_server_new>},
    {.name = "fec_shared_state", .func = safe_yield<fec_shared_state_new>},
    {.name = "fec_pipeline", .func = safe_yield<fec_pipeline_new>},
    {.name = "udp_dyn_mux", .func = safe_yield<udp_dyn_mux_new>},
    {.name = "fec_stats", .func = safe_yield<fec_stats_new>},
    {.name = NULL, .func = NULL},
});

static int hole_open(lua_State* L) {
  auto& interface = *(LuaInterface*)lua_touserdata(L, lua_upvalueindex(1));

  luaL_checkversion(L);
  auto size = hole.size() - 1 + hole_io_object.size() - 1;
  lua_createtable(L, 0, size);
  luaL_setfuncs(L, hole.data(), 0);
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, hole_io_object.data(), 1);

  luaL_newmetatable(L, name_udp);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, udp_metatable, 1);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_udp_mux_server);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, udp_mux_server_metatable, 1);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_udp_dyn_mux);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, udp_dyn_mux_metatable, 1);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_endpoint);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, endpoint_metatable, 1);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_pipeline);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, pipeline_metatable.data(), 1);
  luaL_newmetatable(L, name_fec_pipeline);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &interface);
  luaL_setfuncs(L, fec_pipeline_metatable.data(), 1);
  lua_pop(L, 1);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_fec_shared_state);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, fec_shared_state_metatable.data(), 0);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_fec_stats);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  luaL_setfuncs(L, fec_stats_metatable.data(), 0);
  lua_pop(L, 1);

  luaL_newmetatable(L, name_filter);
  lua_pushvalue(L, -1);           /* push metatable */
  lua_setfield(L, -2, "__index"); /* metatable.__index = metatable */
  luaL_setfuncs(L, filter_metatable, 0);
  lua_pop(L, 1);

  return 1;
}

static const char module_name[] = "hole";

void luaopen_hole(lua_State* L, LuaInterface& interface) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "_LOADED");
  lua_getfield(L, -1, module_name); /* _LOADED[modname] */
  if (!lua_toboolean(L, -1)) {      /* package not already loaded? */
    lua_pop(L, 1);                  /* remove field */
    lua_pushlightuserdata(L, &interface);
    lua_pushcclosure(L, hole_open, 1);
    lua_call(L, 0, 1);                /* call 'openf' to open module */
    lua_pushvalue(L, -1);             /* make copy of module (call result) */
    lua_setfield(L, -3, module_name); /* _LOADED[modname] = module */
  }
  lua_remove(L, -2);             /* remove _LOADED table */
  lua_setglobal(L, module_name); /* _G[modname] = module */
}

} // namespace gh
#include "DebugOutputSink.h"
#include <mutex>

namespace DX12Engine::Logger {

// ========================================================================
// 显式模板实例化
// ========================================================================

// 实例化多线程版本 (std::mutex)
// 这行代码告诉编译器：请为 std::mutex 生成一份 debug_output_sink_impl 的代码
template class debug_output_sink_impl<std::mutex>;

// 如果你未来需要使用单线程版本，也可以在这里实例化：
// template class debug_output_sink_impl<spdlog::details::null_mutex>;

} // namespace DX12Engine::Logger
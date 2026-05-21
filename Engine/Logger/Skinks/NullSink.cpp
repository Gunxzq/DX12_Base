#include "NullSink.h"

namespace DX12Engine::Logger {

// 显式实例化多线程版本
template class null_sink_impl<std::mutex>;

} // namespace DX12Engine::Logger
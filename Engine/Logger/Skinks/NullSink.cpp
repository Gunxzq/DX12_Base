#include "NullSink.h"

namespace DX12Engine {
namespace Core {

// 显式实例化多线程版本
template class null_sink_impl<std::mutex>;

} // namespace Core
} // namespace DX12Engine
// 公共头文件

// 取消定义 Windows API 宏，避免与方法名冲突
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef GetWindowLong
#undef GetWindowLong
#endif
#ifdef GetWindowText
#undef GetWindowText
#endif
#ifdef SetWindowText
#undef SetWindowText
#endif
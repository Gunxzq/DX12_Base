# 配置文件架构

## 概述

配置文件在物理上拆分为多个 JSON 文件，运行时被合并为单一配置树。

**设计目标**：拆分渲染、物理、音频、网络、日志等配置，提高可维护性。

---

## 配置划分

| 文件 | 用途 |
|------|------|
| `app.json` | 窗口、基础路径、模块开关等 |
| `render.json` | 渲染配置 (DX12 核心) |
| `logging.json` | 日志配置 |
| `gameplay.json` | 游戏性配置 (输入、物理等) |

---

## 文件结构

### app.json - 应用基础配置

```json
{
  "app": {
    "name": "DX12_Base",
    "version": "1.0"
  },
  "window": {
    "width": 1920,
    "height": 1080,
    "title": "DX12 Engine"
  },
  "paths": {
    "content": "Content",
    "shaders": "Shaders"
  }
}
```

### render.json - 渲染配置

```json
{
  "render": {
    "api": "DirectX12",
    "resolution": {
      "width": 1920,
      "height": 1080
    },
    "quality": {
      "shadows": true,
      "volumetric_fog": false,
      "msaa": 4
    },
    "debug": {
      "show_gpu_timing": true
    }
  }
}
```

### logging.json - 日志配置

```json
{
  "logging": {
    "global_level": "debug",
    "sinks": {
      "console": { "level": "info" },
      "file": { "path": "logs/engine.log" }
    }
  }
}
```

### gameplay.json - 游戏性配置

```json
{
  "input": {
    "mouse_sensitivity": 2.5
  },
  "physics": {
    "gravity": 9.8
  }
}
```

---

## 使用建议

1. **修改配置前**：先了解对应模块的结构
2. **运行时配置**：通过 `ConfigManager` 访问，禁止直接操作 JSON
3. **新增配置项**：
   - 在对应 JSON 文件中添加字段
   - 在 `ConfigManager` 中注册访问接口

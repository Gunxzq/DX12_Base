#pragma once

// ========================================================================
// ECS Components — 聚合头文件
//
// 按域拆分到 Components/ 子目录，此文件仅做统一 include。
// 各模块可按需包含具体的组件头文件，减少编译依赖。
// ========================================================================

#include "Common/Common.h"

#include "Components/Animation.h"
#include "Components/Grid.h"
#include "Components/Light.h"
#include "Components/Misc.h"
#include "Components/Name.h"
#include "Components/ReflectionProbe.h"
#include "Components/Render.h"
#include "Components/Tags.h"
#include "Components/Transform.h"
#include "Components/Water.h"

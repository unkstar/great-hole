# UE5 代码审核要点（Timi/MHA 项目）

## 通用 UE5 C++ 规范

- 整型优先用 `int32` 而非 `int`（跨平台一致）
- UPROPERTY 数值类型加 `ClampMin`/`ClampMax`，布尔 Interp 无意义
- 命名：`bXxx` for bool，`FXxx` for structs，`UXxx` for UObjects
- 未使用的变量/常量及时删除，避免噪音
- 注释代码（`/* ... */`）不应长期入库，加 TODO 注明时间线

## Shader 文件（.usf/.ush）

- 避免在 shader 内动态分支（`if`）；用 `step()`/`lerp()` 替代
- `half` vs `float`：移动端优先 `half`，但函数返回类型需明确声明
- 硬编码 magic number 需加注释说明来源（物理参数、美术参数等）
- IBL normalization、SH 系数等能量守恒验证

## DaySequence 插件特有知识

- `GTimeOfDayDefaultInterval`（短）vs `GDefaultLongInterval`（长）：TOD 更新频率权衡
- `HasLongTimeInterval()`：根据 Modifier 状态决定使用哪个间隔，不能无条件替换
- `OverrideUpdateIntervalHandle`：Smooth Blending 过程中临时提高更新频率，禁用会导致过渡跳帧
- `UserBlendWeight > 0.999f`：激活阈值，跨越时触发天气广播
- `OnGlobalWeatherChanged`：全局天气变化委托，需绑定后才能触发 `WeatherChanged` 回调
- TODEffect 对象池：`ParticleComponents[]` 数组 + `CurrentSpawnIndex` 循环，头文件与实现必须同步

## 常见 Critical 模式

### 除零风险
```cpp
// ❌ MaxSpawnCount 可能为 0
CurrentSpawnIndex = (CurrentSpawnIndex + 1) % MaxSpawnCount;

// ✅ 加保护
if (MaxSpawnCount <= 0) continue;
CurrentSpawnIndex = (CurrentSpawnIndex + 1) % MaxSpawnCount;
```

### 接口实现脱节
- 头文件改了字段名（如 `ParticleComponent` → `ParticleComponents`），.cpp 没同步
- `UFUNCTION()` 声明了回调但未绑定到对应委托

### 未保护的指针/数组访问
```cpp
// ❌ 未检查数组有效性
ParticleComponents[CurrentSpawnIndex]->ActivateSystem();

// ✅ 
if (ParticleComponents.IsValidIndex(CurrentSpawnIndex) && ParticleComponents[CurrentSpawnIndex])
    ParticleComponents[CurrentSpawnIndex]->ActivateSystem();
```

## 审核评级标准

| 级别 | 标志 | 含义 | 处理方式 |
|------|------|------|----------|
| 🔴 Critical | 编译错误、崩溃、数据损坏 | 必须修复才能合入 | Request Changes |
| 🟠 High | 回归风险、逻辑错误、无注释删除 | 强烈建议修复 | Conditional Approve 或 Request Changes |
| 🟡 Medium | 代码质量、TODO 遗留、命名规范 | 建议修复 | Conditional Approve |
| 🟢 Low | 风格、可读性 | 可选 | Approve with note |
| ✅ OK | 符合预期的正确改动 | 无需修改 | 标注为 OK |

**总体评级逻辑**：
- 有任何 🔴 Critical → Request Changes
- 只有 🟠 High，无 Critical → Conditional Approve（说明风险，由 owner 决定）
- 只有 Medium/Low → Approve with notes

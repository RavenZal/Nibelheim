# 🎨 Dx12Renderer

基于 DirectX 12 与 HLSL 的 Windows 实时渲染器，实现显式 GPU 资源管理、静态 glTF 场景、Metallic-Roughness PBR、方向光阴影以及 HDR Tone Mapping。

## 🖼️ 效果概览

渲染器当前展示一个带完整 glTF 材质的动态 Cube 和一个独立的 Shadow Receiver。场景采用固定世界空间方向光，Cube 持续旋转，各表面的光照响应与地面投影会实时更新。

```text
Directional Shadow Pass
        ↓
HDR Forward PBR Pass
        ↓
Exposure + ACES Tone Mapping
        ↓
Linear-to-sRGB
        ↓
Flip-Model Swap Chain
```

## ✨ 核心特性

### ⚙️ DirectX 12 基础设施

- 显式枚举并选择支持 Direct3D 12 的硬件适配器
- Debug 配置启用 Direct3D 12 Debug Layer 与 DXGI Debug
- 双缓冲 `FLIP_DISCARD` Swap Chain
- 每个 Back Buffer 独立拥有 Command Allocator 与 Fence 值
- 按帧资源所有权执行条件等待，无逐帧全局 GPU 阻塞
- Resize、最小化和恢复期间安全重建 Back Buffer、Depth 与 HDR 资源
- HRESULT、Win32 和设备移除错误检查

### 📦 几何与资源上传

- Indexed Drawing 与 `DXGI_FORMAT_R32_UINT` Index Buffer
- Position、Normal、UV、Tangent 组成的 48-byte Vertex Layout
- 静态 Vertex/Index 数据通过 Upload Heap 复制至 Default Heap
- Texture 使用 `GetCopyableFootprints` 对齐并通过 `CopyTextureRegion` 上传
- Shader-visible SRV Descriptor Table 与静态 Sampler

### 🧊 glTF 2.0 材质

- 静态 Mesh、Node Transform 和 Indexed Triangle Primitive
- glTF 右手坐标到渲染器左手坐标转换
- Base Color Texture 与 Factor
- Tangent-space Normal Map 与 Normal Scale
- Metallic-Roughness Texture 与 Factor
- Base Color 使用 sRGB SRV，Normal/MR 使用 Linear SRV
- WIC 解码嵌入式 PNG Data URI

### 💡 Forward Metallic-Roughness PBR

- World-space TBN 与 Normal Matrix
- GGX Normal Distribution
- Smith Geometry Term
- Schlick Fresnel
- 能量守恒的 Diffuse/Specular 权重
- 固定世界空间 Directional Light
- 独立 Cube 与 Ground 材质参数

### 🌑 Directional Shadow Map

- `2048 × 2048` Typeless Shadow Resource
- D32 DSV 与 R32_FLOAT SRV
- Depth-only Shadow PSO
- Constant Depth Bias 与 Slope-scaled Depth Bias
- `SampleCmpLevelZero` Comparison Sampling
- 3×3 PCF 阴影边缘过滤
- Cube 自身遮挡与 Ground 投影接收

### 🌈 HDR 与最终输出

- Client-size `R16G16B16A16_FLOAT` HDR Render Target
- Forward PBR 输出保持线性且不截断高光
- Fullscreen Triangle，无额外 Vertex Buffer
- 可交互 Exposure
- ACES Fitted Tone Mapping
- 显式 Linear-to-sRGB 输出至 `R8G8B8A8_UNORM` Swap Chain

## 🎮 操作方式

| 输入 | 功能 |
| --- | --- |
| `W / S` | 沿相机前后方向移动 |
| `A / D` | 沿相机左右方向移动 |
| `E / Q` | 沿世界空间上下方向移动 |
| 按住鼠标右键并移动 | 相机视角旋转 |
| `[` | 降低曝光 |
| `]` | 提高曝光 |
| `Home` | 曝光恢复为 `1.0` |
| 调整窗口大小 | 重建尺寸相关渲染资源 |

输入只在渲染窗口处于前台时生效。

## 🧰 环境要求

- Windows 10 或 Windows 11
- 支持 DirectX 12 Feature Level 11_0 的独立或集成 GPU
- Visual Studio 2026，包含 MSVC x64 C++ 工具链
- Windows 10/11 SDK，包含 DXC
- CMake 4.2 或更高版本（用于仓库内置 Preset）

项目只依赖 Windows SDK 组件，不需要额外下载第三方运行库。

## 🔨 构建

在仓库根目录执行：

```powershell
cmake --preset msvc-x64
cmake --build --preset release
```

运行 Release 版本：

```powershell
.\build\Release\DX12EnvironmentCheck.exe
```

构建并运行 Debug 版本：

```powershell
cmake --build --preset debug
.\build\Debug\DX12EnvironmentCheck.exe
```

Shader 由 CMake 调用 Windows SDK 中的 DXC 编译，生成的 CSO 与 glTF 资产会自动复制到对应配置的输出目录。

## 🔄 每帧渲染流程

1. 等待当前 Back Buffer 对应的 Fence（仅在 GPU 尚未完成该帧槽时）。
2. 更新 Camera、Cube/Ground Transform、PBR、Shadow 与 Exposure 数据。
3. 将 Shadow Map 转换至 `DEPTH_WRITE`，绘制 Cube 与 Ground 深度。
4. 将 Shadow Map 转换至 `PIXEL_SHADER_RESOURCE`。
5. 将 HDR Target 转换至 `RENDER_TARGET`，执行 Shadowed Forward PBR。
6. 将 HDR Target 转换至 `PIXEL_SHADER_RESOURCE`。
7. 将 Back Buffer 转换至 `RENDER_TARGET`，绘制 Tone Mapping Fullscreen Triangle。
8. 将 Back Buffer 转换至 `PRESENT`，提交、显示并记录 Fence 值。

Shadow、Forward 和 Tone Mapping Pass 位于同一个 Direct Command List 中，通过命令顺序与显式 Resource Barrier 建立 GPU 依赖，不插入 Pass 间 CPU Wait。

## 🗂️ 项目结构

```text
Dx12Renderer/
├─ assets/models/       glTF 场景与嵌入式材质数据
├─ docs/                渲染管线与实现记录
├─ shaders/             Forward PBR、Shadow 与 Tone Mapping HLSL
├─ src/                 Win32、DirectX 12、glTF 与 WIC 实现
├─ CMakeLists.txt       Target、Shader 编译与资产部署规则
└─ CMakePresets.json    MSVC x64 Configure/Build Preset
```

## 📐 关键实现约定

- CPU 与 HLSL 均采用明确的 Row-vector Matrix 约定。
- 每个帧槽的 Constant Buffer 只在其 Fence 完成后更新。
- 写入一次的几何与纹理数据存放于 Default Heap。
- Base Color 在采样时转换至 Linear；Normal 与 Metallic-Roughness 保持数据纹理语义。
- PBR 计算、HDR 存储、Tone Mapping 与显示编码严格分离。
- Ground 使用稳定的中性常量材质，避免接收面复用 Cube 材质造成伪阴影。

## ✅ 验证状态

- MSVC x64 Debug 与 Release 构建通过
- HLSL Shader 编译无警告
- Resize、最小化/恢复、Camera 与 Exposure 交互通过
- Direct3D 12 Debug Layer 正常关闭记录为 0 条消息
- PIX 已验证关键 Draw、Descriptor Binding、Resource Barrier、Shadow Pass、HDR Target 与 Fullscreen Tone Mapping Pass

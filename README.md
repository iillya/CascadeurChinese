# CascadeurChinese

当前仅提供 **Inno Setup 安装器**，文件名为 `CascadeurChineseInstaller.exe`。安装、更新、迁移及卸载说明见 [安装指南](docs/installer.md)。卸载请使用 Windows“已安装的应用”，不再使用旧安装器。

Cascadeur 的简体中文显示层汉化。支持 **Cascadeur 2024.1.0（Qt 6.5.1）** 和 **2026.1.2（Qt 6.5.3）**，均为 Windows x64。双版本验证范围见 [兼容性记录](docs/compatibility-2024.md)。

## 纯显示层原则

本项目只在 Qt Quick 把文字生成画面节点时，将原 `QTextLayout` 复制成临时布局并替换其中的显示文字。它不会修改 QML 的 `text` 属性、控件状态、场景数据、命令参数或 `.casc` 文件，也不会覆盖 Cascadeur 的任何原始文件。

实现入口是 `QQuickTextNode::addTextLayout`。启动器只负责以挂起状态启动 `cascadeur.exe`、加载汉化 DLL，再继续运行。关闭 Cascadeur 后，Hook 随进程消失。

## 使用

运行发布包中的 `CascadeurChineseInstaller.exe`，选择包含 `cascadeur.exe` 的目录（通常为 `C:\Program Files\Cascadeur`）。文件只安装到其下的 `ChineseLauncher` 独立目录，并创建“Cascadeur 中文版”快捷方式。

工程关联选项接管经过验证的官方打开命令，不强制更改默认应用。卸载仅恢复仍属于补丁的值，保留后来的外部修改。快捷方式面向所有用户，关联只针对当前桌面用户。

卸载只删除安装包自有文件，不递归清空 `ChineseLauncher`；额外文件和已修改的词典会保留。安装过程若中途失败，已完成的文件替换不会整体回滚，修复后可重试安装。

- `F3`：切换中文/英文显示；右键“中/英”可录入新单键，点击“确定”后至 `%LOCALAPPDATA%\CascadeurChinese\settings.json`，下次启动读取。
- `Shift` + `~`：增量采集并导出到桌面；自动展开已识别的对象属性分组补采，结束后恢复折叠状态和滚动位置。重复执行只追加新词条，不覆盖已填写的译文。采集期间暂时拦截编辑操作，按 `Esc` 中止并进入恢复；不新增快捷键。

自动展开是独立的手动 UI 采集辅助模块，不属于只读显示翻译核心。适配依据为 Cascadeur 2026.1.2 内嵌 QML 的 `view::PropertyEditor` 分组结构，仅调用分组原有的 `switchExpand`，不调用图钉或参数修改方法。不匹配该结构时仅只读采集；不能保证覆盖其他面板、虚拟化列表或尚未加载的对象类型。展开阶段上限为 60 秒/128 组，诊断写入临时目录 `Cascadeur_deep_capture.json`；无法恢复的分组会提示。该自动展开行为是当前项目的显式采集扩展，不宣称符合统一规范的“只读嗅探不展开”条款。
- 卸载：通过 Windows“已安装的应用”卸载“Cascadeur 中文补丁”

## 项目结构

- `source/`：显示层 Hook、启动器、安装器和构建脚本
- `source/detours/`：Microsoft Detours 注入/挂钩组件
- `translations/dictionary_zh.json`：清洗、复核并合并后的正式中文词典
- `scripts/`：词典检查和候选文字扫描工具
- `icon/`：程序图标资源

## 构建

准备 Visual Studio 2022 C++ 工具链，以及 Qt 6.5.3 MSVC 2019 x64 开发文件。项目优先使用工作区统一目录 `_ThirdParty\Qt\6.5.3\msvc2019_64`，也可放到仓库的 `third_party\qt6sdk`。随后运行 `source\build.bat`。

产物位于 `build\out`，最终安装包位于 `dist`。

自有 C++ 使用 `/W4 /WX`（警告视为构建失败）。`source\build.bat --analyze` 额外执行 MSVC 静态分析；构建后运行 `scripts\test_all.bat` 执行独立回归检查，不启动或注入 Cascadeur。测试依赖当前 Qt 6.5.3 环境、用于拒绝测试的 Qt 6.6.0，以及 `scripts/requirements-ui-extraction.txt` 和 `scripts/requirements-text-utils.txt` 中的 Python 依赖（当前位于 `build/extraction-deps`）。

`source/translation_policy.h` 集中管理过滤、归一化及有界缓存；`source/file_association.h` 管理用户级工程关联备份/恢复。最新结果见 [完整复审记录](docs/code-audit-2026-08-31-followup.md)，此前记录仅作为历史依据保留。

后台不开启常驻轮询或自动嗅探：菜单安装依靠事件并合并 200ms 内的请求；绘制诊断探针仅在初始化后 5 秒内记录，窗口诊断在第 2/5 秒各写一次。深度采集的 300ms 定时器仅在手动嗅探期间运行。关闭汉化时绘制直接透传，但用于后续重新开启的节点生命周期跟踪仍保留，不宣称零开销。

## 已知边界

只处理经过 Qt Quick 普通文本布局渲染的文字。图标内文字、画布自行绘制的字、网页内容或特殊富文本可能保持英文。带选区、格式范围、输入法预编辑内容或指定行片段的布局保持原样；译文无法在原有行数内完整布局时也回退原文。

私有绘制入口当前验证 **Qt 6.5.1 / 6.5.3 x64**。其他版本、缺少组件或混装不同 Qt 补丁版本会被拒绝；不宣称兼容所有 Qt 6.5 软件。引擎与宿主进程同寿命，不支持运行中 `FreeLibrary` 热卸载，需退出软件后卸载安装文件。

“不改宿主数据”不等于“已经可靠识别所有业务文本”：当前字符串级绘制/度量入口无法区分所有同名资源或非编辑状态的输入值。主动嗅探也尚未对白名单之外的 Model 做完整的业务来源识别；输出只作为人工候选，不应直接批量并入正式词典。

`scripts/import_capture_translations.py` 默认离线生成 `build/capture_review.json`，不再直接覆盖正式词典；只有显式使用 `--machine-translate` 才会向外部翻译服务发送候选文字。静态扫描默认不读取示例工程，`--include-samples` 仅用于人工调查。

`scripts/extract_ui_sources.py` 补充提取内嵌压缩/未压缩 QML、设置/动作名称表和 Python 属性来源；候选只输出供复核，不直接合并词典。用法、结果与动态名称边界见 [界面来源提取记录](docs/ui-source-extraction.md)。

## 许可

项目本身采用 GPL-3.0 License。Microsoft Detours 保留其原始 MIT 许可与版权声明。

## 项目

Cascadeur 主窗口顶部 `Help` 右侧依次提供“中/英”、[Bilibili 神说要凑数汉化](https://space.bilibili.com/281243426?spm_id_from=333.1007.0.0) 与 [GitHub 仓库](https://github.com/iillya/CascadeurChinese)。作者和仓库链接使用蓝色文字及手形光标。

只读兼容性与窗口信息写入临时目录的 `Cascadeur_window_diagnostics.json`；该诊断不调用 `winId()`，也不修改窗口状态。

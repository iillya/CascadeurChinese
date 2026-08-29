# CascadeurChinese

Cascadeur 的简体中文显示层汉化。当前基线为 **Cascadeur 2026.1.2（Qt 6.5.3）**。

## 纯显示层原则

本项目只在 Qt Quick 把文字生成画面节点时，将原 `QTextLayout` 复制成临时布局并替换其中的显示文字。它不会修改 QML 的 `text` 属性、控件状态、场景数据、命令参数或 `.casc` 文件，也不会覆盖 Cascadeur 的任何原始文件。

实现入口是 `QQuickTextNode::addTextLayout`。启动器只负责以挂起状态启动 `cascadeur.exe`、加载汉化 DLL，再继续运行。关闭 Cascadeur 后，Hook 随进程消失。

## 使用

运行发布包中的 `CascadeurChineseSetup.exe`，选择包含 `cascadeur.exe` 的目录（通常为 `C:\Program Files\Cascadeur`）。文件只安装到其下的 `ChineseLauncher` 独立目录，并创建“Cascadeur 中文版”快捷方式。

安装时会把 Cascadeur 工程格式 `.casc` 关联到中文启动器，双击工程即可注入汉化后打开。安装器会先保存原有关联；卸载时删除中文关联并恢复原来的 Cascadeur 打开方式。

- `F3`：临时切换中文/英文显示
- `Shift` + `~`：增量嗅探运行时出现的未翻译词条并导出到桌面；重复执行只追加新词条，不覆盖已填写的译文
- 卸载：再次运行安装程序，选择“卸载”

## 项目结构

- `source/`：显示层 Hook、启动器、安装器和构建脚本
- `source/detours/`：Microsoft Detours 注入/挂钩组件
- `translations/dictionary_zh.json`：清洗、复核并合并后的正式中文词典
- `scripts/`：词典检查和候选文字扫描工具
- `icon/`：程序图标资源

## 构建

准备 Visual Studio 2022 C++ 工具链，以及 Qt 6.5.3 MSVC x64 开发文件。项目优先使用同级 `_ThirdParty\qt6sdk`，也可放到仓库的 `third_party\qt6sdk`。随后运行 `source\build.bat`。

产物位于 `build\out`，最终安装包位于 `dist`。

## 已知边界

只处理经过 Qt Quick 普通文本布局渲染的文字。图标内文字、画布自行绘制的字、网页内容或特殊富文本可能保持英文。输入框编辑时使用原始英文数据是刻意设计：翻译只发生在最终显示阶段。

## 许可

项目本身采用 GPL-3.0 License。Microsoft Detours 保留其原始 MIT 许可与版权声明。

## 项目

Cascadeur 主窗口顶部 `Help` 右侧提供 [Bilibili神说要凑数汉化](https://space.bilibili.com/281243426?spm_id_from=333.1007.0.0) 与 [Github仓库](https://github.com/iillya/CascadeurChinese) 两个可点击署名链接。

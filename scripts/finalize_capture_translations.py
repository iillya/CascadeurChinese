import json
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "translations" / "dictionary_zh.json"
doc = json.loads(path.read_text(encoding="utf-8"))
d = doc["translations"]

manual = {
"Adds  a transform dummy \u2014 an empty object with Position, Rotation and Scale transform values":"添加变换虚拟对象——一个具有位置、旋转和缩放变换值的空对象",
"Adds joint behavior to selected objects that do not already have it, along with extra joint attributes":"为尚无关节行为的选定对象添加关节行为及额外关节属性",
"All component type visible except Guid":"显示除 GUID 外的所有组件类型",
"Attract To Average Position Current Frame (Hard)":"强力吸引到当前帧平均位置",
"Attract To Inverse Inertial Position Current Frame (Hard)":"强力吸引到当前帧反向惯性位置",
"AutoPosing precision":"自动摆姿精度",
"Bezier On Current Frame":"当前帧使用贝塞尔插值",
"Bezier Viscous Bezier Viscous On Selected Interval":"选定区间使用黏性贝塞尔插值",
"Binds direction controllers from the selected objects to the center of mass":"将选定对象的方向控制器绑定到质心",
"Change To Fulcrum Key On Interval":"在区间内改为支点关键帧",
"Clean Dynamic Collisions":"清理动态碰撞",
"Combat":"战斗",
"Creates a transform constraint between selected objects, allowing one to follow the other's position and rotation. Only independent transform objects are supported.Select at least two objects. The last selected object will be the target.":"在选定对象之间创建变换约束，使一个对象跟随另一个对象的位置和旋转。仅支持独立变换对象。请至少选择两个对象，最后选择的对象将作为目标。",
"Exports in-place animation by resetting the root joint's position or both position and rotation while maintaining relative motion for rigged points. Supports exporting selected joints within a chosen frame interval":"通过重置根关节的位置或同时重置位置与旋转来导出原地动画，同时保持绑定点的相对运动。支持在指定帧区间内导出选定关节",
"Fix Scene":"修复场景",
"Frequency of the displayed frames in the current scene, expressed as frames per second.":"当前场景显示帧的频率，以每秒帧数表示。",
"Geometrical constraint weight":"几何约束权重",
"Ghost outlined mode":"残影轮廓模式",
"Goes to the model pose":"转到模型姿势",
"Golden spiral flip":"翻转黄金螺旋",
"Golden spiral mirror H":"水平镜像黄金螺旋",
"If enabled, the trajectory is rendered relative to the pivot.":"启用后，将相对于枢轴渲染轨迹。",
"If enabled, Trajectory Tangent handles are linked i.e. adjusting one of them affects the other\u2019s angle. If disabled, tangents can be edited independently from one another.":"启用后，轨迹切线手柄会相互联动，调整其中一个会影响另一个的角度。禁用后，可分别编辑两侧切线。",
"If this is enabled, animation for fingers is unbaked. Otherwise, it is ignored.":"启用后将对手指动画解除烘焙，否则忽略手指动画。",
"IK":"IK",
"Import Scene To Current...":"将场景导入当前场景…",
"Interpolator reloading...":"正在重新加载插值器…",
"Jerk weight":"加加速度权重",
"Key frame weight":"关键帧权重",
"Linear On Current Frame":"当前帧使用线性插值",
"Max velocity":"最大速度",
"Mirrors the golden spiral horizontally.":"水平镜像黄金螺旋。",
"PositionVelocityScale":"位置速度缩放",
"Preserve tangent in the ballistic first and last (start and end) frames.":"在弹道的首尾帧（开始和结束）保持切向分量。",
"Primitives":"基本体",
"Prints the default export path from a stored file":"从保存的文件中读取并输出默认导出路径",
"Ragdoll":"布娃娃",
"Reference force":"参考力",
"Relaxation":"松弛",
"Reload commands":"重新加载命令",
"Remove Frames":"移除帧",
"Removes every N keyframe on the selected timeline interval, from the selected animation tracks":"从选定动画轨道的时间轴区间内每隔 N 个关键帧移除一个关键帧",
"Removes the joint behavior and its extra attributes from selected objects if they have a joint behavior":"从具有该行为的选定对象中移除关节行为及其额外属性",
"Removes the parent relationship from the selected objects, making them top-level objects in the scene hierarchy":"移除选定对象的父级关系，使其成为场景层级中的顶层对象",
"Renders the selected coordinate axis of the selected Rotation Trajectories.":"渲染选定旋转轨迹的指定坐标轴。",
"Repeat Last action":"重复上次操作",
"Report A Bug":"报告错误",
"Reset All To Factory Settings":"全部恢复出厂设置",
"Resets selected box controllers to their T-pose position based on stored T Pose Position and T Pose Rotation data":"根据保存的 T 姿势位置和旋转数据，将选定的盒控制器重置到 T 姿势",
"Resets selected joints to their bind pose at the current frame. Uses inverse bind matrices from skinned meshes.":"在当前帧将选定关节重置为绑定姿势，并使用蒙皮网格的逆绑定矩阵。",
"Retargeting Copy":"重定向复制",
"Retargeting Paste":"重定向粘贴",
"Reverse animation":"反转动画",
"Reverses animation by flipping keyframes and swapping non-static data values over the selected frame range":"通过翻转关键帧并交换选定帧范围内的非静态数据值来反转动画",
"Rig additional":"附加绑定",
"Rig info":"绑定信息",
"Root motion":"根运动",
"Rotate hierarchy":"旋转层级",
"Rotated axis":"旋转轴",
"Rotation axis weight":"旋转轴权重",
"Rotation cost weight":"旋转代价权重",
"Rotation cut off %":"旋转截断百分比",
"Rotation plane":"旋转平面",
"Rotation point weight":"旋转点权重",
"Rotator sensitivity":"旋转器灵敏度",
"Run Root Motion":"运行根运动",
"Run tests":"运行测试",
"Same as above, but for the instantaneous rotation axis.":"与上项相同，但用于瞬时旋转轴。",
"Same, but for the angular momentum. Increasing this parameter smoothes the overall angular momentum for the character.":"用于角动量。提高此参数会使角色的整体角动量更加平滑。",
"Save As New Version":"另存为新版本",
"Save As... (no assets)":"另存为…（不含资源）",
"Scene settings":"场景设置",
"Secondary alpha":"次级运动 Alpha",
"Set how much the algorithm should try to preserve a parabolic trajectory. When the Physics Corrector tool is enabled, Smooth Trajectory generates a ballistic trajectory for non-fulcrum keyframes.":"设置算法保持抛物线轨迹的程度。启用物理校正器后，平滑轨迹会为非支点关键帧生成弹道轨迹。",
"Set key":"设置关键帧",
}
d.update(manual)

for key in ("Alt+R, Ctrl+R", "Ctrl+=, Ctrl++", "Ctrl+Z, F14", "IK"):
    d.pop(key, None)
for key in list(d):
    if "\ufffd" in key:
        d.pop(key, None)

fixes = {
    "Change IK|FK Key":"更改 IK/FK 关键帧",
    "Change IK|FK Key On Interval":"在区间内更改 IK/FK 关键帧",
    "IK FK Type IK FK Type On Current Frame":"在当前帧设置 IK/FK 类型",
    "IK FK Type IK FK Type On Selected Interval":"在选定区间设置 IK/FK 类型",
    "Planes XZ":"XZ 平面",
    "Planes XY":"XY 平面",
    "Planes YZ":"YZ 平面",
}
d.update(fixes)
doc["translations"] = dict(sorted(d.items(), key=lambda item: item[0].casefold()))
path.write_text(json.dumps(doc, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"formal_total={len(d)} manual_added={len(manual)}")

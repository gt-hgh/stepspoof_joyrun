#!/system/bin/sh
# KernelSU / Magisk 模块开机脚本：给应用双开（999 空间）做一次性配置初始化。
#
# 背景：
#   Zygisk 按包名注入，user 0 和 999 空间里的闪动校园都会被注入，hook 都能装上。
#   v1.3.2 起，配置的权威写端已经挪到目标 App 进程内部（save_conf_native()），
#   "/data/data/<pkg>/files/stepspoof.conf" 在那个进程里会按它自己的 user 解析，
#   所以两个空间各写各的，天然互不干扰。
#
#   这个脚本只剩下一个职责：999 空间第一次装上/第一次启动、还没有自己的 conf 时，
#   把 user 0 的那份抄一份当初始值，省得用户手动调一遍。
#
# 为什么不再做循环强同步：
#   v1.3.2 的实现是每 2 秒 cmp + cp。只要两边步频不一样，或者用户就想让一个空间
#   跑、另一个停着，这个循环会在 2 秒内把状态强行拉回 user 0 的值，
#   "双开独立控制"就失效了。初始化一次就退出，运行期交给各实例自己管。
#
# 安全边界：
#   - 只用 -d 判定目录存在，绝不 mkdir。以 root 身份创建 app 数据目录会把属主
#     和 SELinux 标签搞错，目标 App 之后自己写文件会 Permission Denied。
#   - 目标 conf 已存在就什么都不做，不覆盖用户在新版本里调好的参数。
#   - chmod 666：root 写出的文件属主是 root，App 的 uid 只能靠权限位读。
#     SELinux 标签由父目录继承为 app_data_file，不需要额外 restorecon。

PKG="com.huachenjie.shandong_school"
SRC="/data/user/0/$PKG/files/stepspoof.conf"
DST_DIR="/data/user/999/$PKG/files"
DST="$DST_DIR/stepspoof.conf"

# 没有 999 空间 / 没有双开过的目标包，直接退出
[ -d "$DST_DIR" ] || exit 0
[ -f "$SRC" ] || exit 0
# 999 空间已经有自己的配置，尊重它，不覆盖
[ -f "$DST" ] && exit 0

cp "$SRC" "$DST" 2>/dev/null
chmod 666 "$DST" 2>/dev/null
exit 0

#pragma once
/* HFR 轻量 i18n：主界面关键文案中英双语（缺省中文）。
 * 说明：仅主导播坞的导航/主按钮/标签做双语，专业对话框暂保持中文（v1 范围）。
 * 语言设置持久化在 QSettings("HuafeiRongOBS", "ui")，切换后需重启生效。 */
#include <QString>

namespace hfrt {

enum class Lang { Zh = 0, En = 1 };

/* 读取已保存语言（首次调用时懒加载） */
Lang GetLang();
/* 设置并保存（下次启动生效） */
void SetLang(Lang lang);

/* 取当前语言文案：key 可空（仅用于调试），zh/en 为两种语言的默认文案 */
QString T(const char *key, const char *zh, const char *en);

} // namespace hfrt

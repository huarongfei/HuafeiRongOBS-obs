#pragma once

/* PPT 来源类型注册入口与访问器。
 * 声明统一放在 HFRPpt.hpp（跨文件调用点都从那里引用），这里做转发包含，
 * 并额外提供"取来源内部文档/标记重绘"的访问器，供讲者视图同步使用。 */
#include "HFRPpt.hpp"

#include <obs.hpp>

class HFRPptDocument;

/* 取某个 PPT 来源内部使用的文档实例；非本类型来源返回 nullptr */
HFRPptDocument *HfrPptSourceGetDoc(obs_source_t *source);

/* UI 线程：立即按当前页渲染并标记上传（讲者视图翻页后调用） */
void HfrPptSourceRenderNow(obs_source_t *source);

/* 让该来源在下一帧重新上传纹理（画布/推流随即可见） */
void HfrPptSourceMarkDirty(obs_source_t *source);

/* 找到第一个 PPT 来源（不增加引用，仅用于临时使用）；无则 nullptr */
obs_source_t *HfrFindFirstPptSource();
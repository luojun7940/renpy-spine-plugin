/* spine_renpy.c
 * =============
 * Ren'Py bridge layer. Compiled into each spine{ver}.dll to expose a
 * version-independent stable ABI (spR_*) to Python/ctypes. Version
 * differences (updateWorldTransform physics arg, computeWorldVertices
 * slot/bone arg, ...) are absorbed here via the SPINE_RENPY_VER macro,
 * so the Python middleware only needs to load the right DLL by version.
 *
 * Build with -DSPINE_RENPY_VER=35|36|37|38|40|41|42
 */
#include <spine/spine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#define SP_R_API __declspec(dllexport)
#else
#define SP_R_API
#endif

/* ---------------- version differences ---------------- */

#if SPINE_RENPY_VER >= 42
#define SP_R_UPDATE_WORLD(s)  spSkeleton_updateWorldTransform((s), SP_PHYSICS_UPDATE)
#else
#define SP_R_UPDATE_WORLD(s)  spSkeleton_updateWorldTransform((s))
#endif

#if SPINE_RENPY_VER >= 41
#define SP_R_COMPUTE_REGION(a, s, v)  spRegionAttachment_computeWorldVertices((a), (s), (v), 0, 2)
#else
#define SP_R_COMPUTE_REGION(a, s, v)  spRegionAttachment_computeWorldVertices((a), (s)->bone, (v), 0, 2)
#endif

/* ---------------- data structures ---------------- */

/* Python 侧通过 ctypes 注册的回调：
 * userData      : spR_setListener 传入的 userdata（Python 侧无需关心）
 * type          : spEventType 的整数值（0=START 1=INTERRUPT 2=END 3=COMPLETE 4=DISPOSE 5=EVENT）
 * animationName : 触发事件的动画名（可能为 NULL）
 * eventName     : 事件名（仅 type==EVENT 时有值，可能为 NULL）
 * eventTime     : 事件在动画中的时间点（秒）
 * intValue      : 事件的 int 数据（无则 0）
 * floatValue    : 事件的 float 数据（无则 0）
 * stringValue   : 事件的 string 数据（无则 NULL）
 */
typedef void (*spRListenerCallback)(void *userData, int type,
                                    const char *animationName,
                                    const char *eventName,
                                    float eventTime,
                                    int intValue,
                                    float floatValue,
                                    const char *stringValue);

typedef struct spRContext {
    spAtlas *atlas;
    spSkeletonData *skeletonData;
    spAnimationStateData *stateData;
    spAnimationState *state;
    spSkeleton *skeleton;
    char error[512];
    spRListenerCallback userListener;   /* Python 注册的回调，NULL 表示不转发 */
    void *listenerUserData;
    spSkin **combinedSkins;             /* 组合皮肤缓存（同名复用；4.x 可安全释放） */
    int combinedSkinsCount;
    float *tmpVerts;                    /* 顶点临时缓冲（buildMesh/collectBounds 复用，按需扩展） */
    int tmpVertsCap;
    /* 本帧 mesh 附件公共缓冲（collectDrawItems 动态扩容，不截断；
     * 由 spR_getMeshBufs 返回，随下一次收集失效） */
    float *meshVertsBuf;
    int meshVertsCap;
    float *meshUVsBuf;
    int meshUVsCap;
    unsigned short *meshTrisBuf;
    int meshTrisCap;
    int meshVertsUsed;
    int meshUVsUsed;
    int meshTrisUsed;
    /* 裁剪器（spSkeletonClipping）：3.5~4.2 各版本结构体与 API 完全一致，
     * 渲染循环中 clipStart / clipTriangles / clipEnd 驱动裁剪附件。 */
    spSkeletonClipping *clipper;
} spRContext;

/* spRDrawItem 的 mesh 数据写入本帧公共缓冲（动态扩容），结构体内只记偏移；
 * 256 顶点上限时代的固定数组已移除，超大 mesh 附件不再截断。
 * 公共缓冲由 spR_getMeshBufs 返回，随下一次 spR_collectDrawItems 失效。 */

typedef struct spRDrawItem {
    int slotIndex;     /* slot index in skeleton */
    int texIndex;      /* atlas page index (which png) */
    float vertices[8]; /* 4 corners in world x,y (order br, bl, ul, ur) - region only */
    float uvs[8];      /* 4 corners in texture u,v - region only */
    float color[4];    /* r,g,b,a in [0,1] (skeleton x slot x attachment) */
    /* mesh 附件数据（region 附件时 vertsCount/trianglesCount 为 0） */
    int vertsCount;                 /* 顶点 float 数（mesh only，每顶点 2 个） */
    int trianglesCount;             /* 三角形数（mesh only） */
    int vertsOffset;                /* meshVerts/meshUVs 在本帧公共缓冲的 float 偏移（两数组同步推进，偏移相同） */
    int trisOffset;                 /* meshTris 在本帧公共缓冲的 unsigned short 偏移 */
} spRDrawItem;

/* 本帧 mesh 公共缓冲扩容（实现见下方，spR_collectDrawItems 先于定义使用） */
static float *_spR_meshVerts(spRContext *ctx, int n);
static float *_spR_meshUVs(spRContext *ctx, int n);
static unsigned short *_spR_meshTris(spRContext *ctx, int n);
static float *_spR_tmpVerts(spRContext *ctx, int n);

/* 把 clipper 裁剪结果写入本帧 mesh 公共缓冲并设置 item 偏移/计数；
 * 返回 1 成功，0 表示完全被裁掉（不产出渲染项）。实现见下方。 */
static int _spR_emitClipped(spRContext *ctx, spRDrawItem *item, spSkeletonClipping *clipper);

/* ---------------- ABI ---------------- */

/* 前置声明：spR_create 中挂接，实现在本文件下方 */
static void _spR_forwardListener(spAnimationState *state, spEventType type,
                                spTrackEntry *entry, spEvent *event);

SP_R_API const char *spR_version(void) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d", SPINE_RENPY_VER / 10);
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ".%d", SPINE_RENPY_VER % 10);
    return buf;
}

/* 判断骨架文件是否为 JSON 文本：跳过开头空白（含 UTF-8 BOM）后以 '{' 开头。
 * 其余情况一律视为 skel 二进制（3.7+ 文件头自带 hash+版本字符串，
 * 由 spSkeletonBinary 内部解析，桥接层无需关心）。
 * 注意：不以 spR_ 开头命名，避免被 gen_def.py 误收集为导出符号。 */
static int is_skeleton_json_file(const char *path) {
    FILE *f = fopen(path, "rb");
    int c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == 0xEF || c == 0xBB || c == 0xBF)
            continue; /* 空白与 UTF-8 BOM 字节 */
        fclose(f);
        return c == '{';
    }
    fclose(f);
    return 0;
}

/* 内存版 JSON 判定：与 is_skeleton_json_file 同规则，作用于内存字节。 */
static int is_skeleton_json_data(const unsigned char *data, int len) {
    int i;
    for (i = 0; i < len; i++) {
        int c = data[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == 0xEF || c == 0xBB || c == 0xBF)
            continue; /* 空白与 UTF-8 BOM 字节 */
        return c == '{';
    }
    return 0;
}

SP_R_API void *spR_create(const char *jsonPath, const char *atlasPath, float scale) {
    spRContext *ctx = calloc(1, sizeof(spRContext));
    int idx;
    spAtlasPage *page;

    if (!ctx) return NULL;

    /* world Y points down, matches Ren'Py screen coordinates */
    spBone_setYDown(1);

    ctx->atlas = spAtlas_createFromFile(atlasPath, NULL);
    if (!ctx->atlas) {
        snprintf(ctx->error, sizeof(ctx->error), "cannot load atlas: %s", atlasPath);
        return ctx;
    }

    /* store page index in rendererObject for texIndex */
    idx = 0;
    for (page = ctx->atlas->pages; page; page = page->next)
        page->rendererObject = (void *)(intptr_t)(idx++);

    /* spSkeletonJson_create(atlas) internally builds the atlas attachment loader */
    if (is_skeleton_json_file(jsonPath)) {
        spSkeletonJson *json = spSkeletonJson_create(ctx->atlas);
        json->scale = scale;

        ctx->skeletonData = spSkeletonJson_readSkeletonDataFile(json, jsonPath);
        if (!ctx->skeletonData) {
            if (json->error)
                snprintf(ctx->error, sizeof(ctx->error), "%s", json->error);
            else
                snprintf(ctx->error, sizeof(ctx->error), "cannot read skeleton json: %s", jsonPath);
            spSkeletonJson_dispose(json);
            return ctx;
        }
        spSkeletonJson_dispose(json);
    } else {
        /* skel 二进制：spSkeletonBinary 内部处理 hash/版本等文件头（3.7+） */
        spSkeletonBinary *binary = spSkeletonBinary_create(ctx->atlas);
        binary->scale = scale;

        ctx->skeletonData = spSkeletonBinary_readSkeletonDataFile(binary, jsonPath);
        if (!ctx->skeletonData) {
            if (binary->error)
                snprintf(ctx->error, sizeof(ctx->error), "%s", binary->error);
            else
                snprintf(ctx->error, sizeof(ctx->error), "cannot read skeleton skel: %s", jsonPath);
            spSkeletonBinary_dispose(binary);
            return ctx;
        }
        spSkeletonBinary_dispose(binary);
    }

    ctx->stateData = spAnimationStateData_create(ctx->skeletonData);
    ctx->state = spAnimationState_create(ctx->stateData);
    ctx->skeleton = spSkeleton_create(ctx->skeletonData);
    spSkeleton_setToSetupPose(ctx->skeleton);
    SP_R_UPDATE_WORLD(ctx->skeleton);

    /* 挂上事件转发 listener：所有版本 spAnimationState 均有 listener / rendererObject
     * 字段（3.5 无 userData，故用 rendererObject 反查 context）。 */
    ctx->state->listener = _spR_forwardListener;
    ctx->state->rendererObject = ctx;
    /* 裁剪器：渲染循环（collectDrawItems/buildMesh）复用一个实例 */
    ctx->clipper = spSkeletonClipping_create();
    return ctx;
}

/* 内存版 spR_create：json/skel 与 atlas 全部由调用方读好字节直接传入，
 * C 层不再访问文件系统（安卓 asset 虚拟文件系统无法 fopen，需 renpy 读取）。
 * atlas 的 dir 传空串：page 的图片路径不需要在此拼目录（渲染层自行
 * 按 atlas 同目录 + 页名查找图片），与原 spAtlas_createFromFile 行为等价。
 * 注意：atlasData/jsonData 必须保证在调用期间有效（解析完成后不再引用）。 */
SP_R_API void *spR_createMem(const unsigned char *skeletonData, int skeletonLen,
                             const unsigned char *atlasData, int atlasLen, float scale) {
    spRContext *ctx = calloc(1, sizeof(spRContext));
    int idx;
    spAtlasPage *page;

    if (!ctx) return NULL;

    /* world Y points down, matches Ren'Py screen coordinates */
    spBone_setYDown(1);

    ctx->atlas = spAtlas_create((const char *)atlasData, atlasLen, "", NULL);
    if (!ctx->atlas) {
        snprintf(ctx->error, sizeof(ctx->error), "cannot load atlas from memory");
        return ctx;
    }

    /* store page index in rendererObject for texIndex */
    idx = 0;
    for (page = ctx->atlas->pages; page; page = page->next)
        page->rendererObject = (void *)(intptr_t)(idx++);

    if (is_skeleton_json_data(skeletonData, skeletonLen)) {
        spSkeletonJson *json = spSkeletonJson_create(ctx->atlas);
        json->scale = scale;

        ctx->skeletonData = spSkeletonJson_readSkeletonData(json, (const char *)skeletonData);
        if (!ctx->skeletonData) {
            if (json->error)
                snprintf(ctx->error, sizeof(ctx->error), "%s", json->error);
            else
                snprintf(ctx->error, sizeof(ctx->error), "cannot read skeleton json from memory");
            spSkeletonJson_dispose(json);
            return ctx;
        }
        spSkeletonJson_dispose(json);
    } else {
        /* skel 二进制：spSkeletonBinary 内部处理 hash/版本等文件头（3.7+） */
        spSkeletonBinary *binary = spSkeletonBinary_create(ctx->atlas);
        binary->scale = scale;

        ctx->skeletonData = spSkeletonBinary_readSkeletonData(binary, skeletonData, skeletonLen);
        if (!ctx->skeletonData) {
            if (binary->error)
                snprintf(ctx->error, sizeof(ctx->error), "%s", binary->error);
            else
                snprintf(ctx->error, sizeof(ctx->error), "cannot read skeleton skel from memory");
            spSkeletonBinary_dispose(binary);
            return ctx;
        }
        spSkeletonBinary_dispose(binary);
    }

    ctx->stateData = spAnimationStateData_create(ctx->skeletonData);
    ctx->state = spAnimationState_create(ctx->stateData);
    ctx->skeleton = spSkeleton_create(ctx->skeletonData);
    spSkeleton_setToSetupPose(ctx->skeleton);
    SP_R_UPDATE_WORLD(ctx->skeleton);

    /* 挂上事件转发 listener（同 spR_create） */
    ctx->state->listener = _spR_forwardListener;
    ctx->state->rendererObject = ctx;
    /* 裁剪器：渲染循环（collectDrawItems/buildMesh）复用一个实例 */
    ctx->clipper = spSkeletonClipping_create();
    return ctx;
}

SP_R_API const char *spR_error(void *vctx) {
    spRContext *ctx = vctx;
    return ctx ? ctx->error : "";
}

SP_R_API void spR_dispose(void *vctx) {
    spRContext *ctx = vctx;
    int i;
    if (!ctx) return;
    /* 组合皮肤：3.8+ 的 setAttachment 带引用计数，可安全释放；
     * 3.5~3.7 组合皮肤与源皮肤共享 attachment 且无 copy API，释放会 double-free，仅缓存复用。 */
#if SPINE_RENPY_VER >= 38
    for (i = 0; i < ctx->combinedSkinsCount; i++)
        if (ctx->combinedSkins[i]) spSkin_dispose(ctx->combinedSkins[i]);
#endif
    if (ctx->combinedSkins) free(ctx->combinedSkins);
    if (ctx->tmpVerts) free(ctx->tmpVerts);
    if (ctx->meshVertsBuf) free(ctx->meshVertsBuf);
    if (ctx->meshUVsBuf) free(ctx->meshUVsBuf);
    if (ctx->meshTrisBuf) free(ctx->meshTrisBuf);
    if (ctx->clipper) spSkeletonClipping_dispose(ctx->clipper);
    if (ctx->state) spAnimationState_dispose(ctx->state);
    if (ctx->stateData) spAnimationStateData_dispose(ctx->stateData);
    if (ctx->skeleton) spSkeleton_dispose(ctx->skeleton);
    if (ctx->skeletonData) spSkeletonData_dispose(ctx->skeletonData);
    if (ctx->atlas) spAtlas_dispose(ctx->atlas);
    free(ctx);
}

/* 清理与当前附件不匹配的 deform 残留（动画切换后旧 deform 时间线写入的
 * deformCount 若小于当前 mesh 需要的顶点数，computeWorldVertices 会越界读
 * slot->deform，产生巨大坐标/崩溃）。规则：deformCount 必须等于当前 mesh 的
 * worldVerticesLength 才算有效；附件不是 mesh 时 deform 一律无效。
 *
 * 字段名随版本变化：3.8+ 用 slot->deform*，3.5~3.7 用 slot->attachmentVertices*，
 * 二者语义一致（见各版本 Animation.c 的 DeformTimeline 与 VertexAttachment.c
 * 的 computeWorldVertices）。 */
static void _spR_clearStaleDeform(spRContext *ctx) {
    spSkeleton *skel;
    int i;
    if (!ctx) return;
    skel = ctx->skeleton;
    if (!skel) return;
    for (i = 0; i < skel->slotsCount; i++) {
        spSlot *slot = skel->slots[i];
        spAttachment *att;
#if SPINE_RENPY_VER >= 38
        if (slot->deformCount == 0) continue;
#else
        if (slot->attachmentVerticesCount == 0) continue;
#endif
        att = slot->attachment;
        if (!att) {
#if SPINE_RENPY_VER >= 38
            slot->deformCount = 0;
#else
            slot->attachmentVerticesCount = 0;
#endif
            continue;
        }
        if (att->type == SP_ATTACHMENT_MESH || att->type == SP_ATTACHMENT_LINKED_MESH) {
            spVertexAttachment *va = (spVertexAttachment *)att;
#if SPINE_RENPY_VER >= 38
            if (slot->deformCount != va->worldVerticesLength) slot->deformCount = 0;
#else
            if (slot->attachmentVerticesCount != va->worldVerticesLength) slot->attachmentVerticesCount = 0;
#endif
        } else {
#if SPINE_RENPY_VER >= 38
            slot->deformCount = 0;
#else
            slot->attachmentVerticesCount = 0;
#endif
        }
    }
}

/* advance one frame: update animation state -> apply -> recompute world */
SP_R_API void spR_update(void *vctx, float delta) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return;
    spAnimationState_update(ctx->state, delta);
    spAnimationState_apply(ctx->state, ctx->skeleton);
    _spR_clearStaleDeform(ctx);
    SP_R_UPDATE_WORLD(ctx->skeleton);
}

/* ---------------- animation events (listener) ---------------- */

/* spine-c 的 spAnimationState_listener：在 spAnimationState_update 内同步调用，
 * 事件类型包括 START / INTERRUPT / END / COMPLETE / DISPOSE / EVENT。 */
static void _spR_forwardListener(spAnimationState *state, spEventType type,
                                spTrackEntry *entry, spEvent *event) {
    spRContext *ctx = (spRContext *)state->rendererObject;
    if (!ctx || !ctx->userListener) return;
    ctx->userListener(ctx->listenerUserData,
                      (int)type,
                      (entry && entry->animation) ? entry->animation->name : NULL,
                      (event && event->data) ? event->data->name : NULL,
                      event ? event->time : 0.0f,
                      event ? event->intValue : 0,
                      event ? event->floatValue : 0.0f,
                      (event && event->stringValue) ? event->stringValue : NULL);
}

/* 注册事件回调。cb 为 NULL 时停止转发。userData 原样回传给回调。 */
SP_R_API void spR_setListener(void *vctx, spRListenerCallback cb, void *userData) {
    spRContext *ctx = vctx;
    if (!ctx) return;
    ctx->userListener = cb;
    ctx->listenerUserData = userData;
}

/* 设置全局动画速率（对应 spine-unity 的 AnimationState.TimeScale），作用于所有轨道。 */
SP_R_API void spR_setTimeScale(void *vctx, float scale) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return;
    ctx->state->timeScale = scale;
}

/* 设置单轨道动画速率（对应 spine-unity 的 TrackEntry.TimeScale）。
 * 在全局速率基础上再乘；默认 1（不变速），0 表示该轨道时间冻结。
 * 该轨道当前无动画时返回 0。 */
SP_R_API int spR_setTrackTimeScale(void *vctx, int track, float scale) {
    spRContext *ctx = vctx;
    spTrackEntry *entry;
    if (!ctx || !ctx->state) return 0;
    entry = spAnimationState_getCurrent(ctx->state, track);
    if (!entry) return 0;
    entry->timeScale = scale;
    return 1;
}

/* 在指定轨道上播放动画（track 0 起，多轨可并行叠加）。 */
SP_R_API int spR_setAnimation(void *vctx, int track, const char *name, int loop) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return 0;
    /* 动画名不存在时 setAnimationByName 内部会解引用 NULL animation 崩溃，
     * 调用前先预检查（3.5~4.2 均有 spSkeletonData_findAnimation）。 */
    if (!spSkeletonData_findAnimation(ctx->skeletonData, name)) return 0;
    if (!spAnimationState_setAnimationByName(ctx->state, track, name, loop)) return 0;
    spAnimationState_apply(ctx->state, ctx->skeleton);
    _spR_clearStaleDeform(ctx);
    SP_R_UPDATE_WORLD(ctx->skeleton);
    return 1;
}

/* 在指定轨道上把动画加入播放队列（当前动画播完后按 delay 延迟接续）。 */
SP_R_API int spR_addAnimation(void *vctx, int track, const char *name, int loop, float delay) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return 0;
    if (!spSkeletonData_findAnimation(ctx->skeletonData, name)) return 0;
    if (!spAnimationState_addAnimationByName(ctx->state, track, name, loop, delay)) return 0;
    return 1;
}

/* 查询指定轨道当前动画名（spine-unity 的 AnimationState.GetCurrent(track).Animation.Name）；
 * 轨道无动画返回 NULL。 */
SP_R_API const char *spR_getCurrentAnimationName(void *vctx, int track) {
    spRContext *ctx = vctx;
    spTrackEntry *entry;
    if (!ctx || !ctx->state) return NULL;
    entry = spAnimationState_getCurrent(ctx->state, track);
    if (!entry || !entry->animation) return NULL;
    return entry->animation->name;
}

/* 查询指定轨道当前动画是否循环；轨道无动画返回 -1。
 * 热重载存档恢复时需区分 loop 与一次性动画。 */
SP_R_API int spR_getCurrentLoop(void *vctx, int track) {
    spRContext *ctx = vctx;
    spTrackEntry *entry;
    if (!ctx || !ctx->state) return -1;
    entry = spAnimationState_getCurrent(ctx->state, track);
    if (!entry) return -1;
    return entry->loop ? 1 : 0;
}

/* 查询指定轨道播放队列中第 idx 项（0 = 当前动画播完后的下一项）的
 * name/loop/delay。name 拷贝到 buf（与 spR_getAnimationName 同风格）。
 * 成功返回 1；无该队列项返回 0。loop/delay 为可空输出参数。 */
SP_R_API int spR_getQueuedAnimation(void *vctx, int track, int idx,
                                    char *buf, int buflen, int *loop, float *delay) {
    spRContext *ctx = vctx;
    spTrackEntry *entry;
    if (!ctx || !ctx->state || !buf || buflen <= 0) return 0;
    entry = spAnimationState_getCurrent(ctx->state, track);
    if (!entry) return 0;
    /* 队列从当前 entry 的 next 开始：先跳到队首，再沿链表走 idx 步 */
    entry = entry->next;
    while (idx-- > 0 && entry) entry = entry->next;
    if (!entry || !entry->animation || !entry->animation->name) return 0;
    strncpy(buf, entry->animation->name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    if (loop) *loop = entry->loop ? 1 : 0;
    if (delay) *delay = entry->delay;
    return 1;
}

/* 全局动画速率（AnimationState.timeScale），热重载存档恢复用。 */
SP_R_API float spR_getTimeScale(void *vctx) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return 1.0f;
    return ctx->state->timeScale;
}

/* 轨道第 pos 个 entry（0=当前，1..=队列项）的完整运行时状态，热重载存档恢复用。
 * entry 可为空动画（is_empty=1，此时动画名无效、buf 置空）。
 * 返回 1 存在；0 无该 entry。 */
SP_R_API int spR_getTrackEntryState(void *vctx, int track, int pos,
                                    char *buf, int buflen,
                                    int *is_empty, int *loop,
                                    float *time_scale, float *mix_duration,
                                    float *mix_time, float *track_time,
                                    float *delay) {
    spRContext *ctx = vctx;
    spTrackEntry *entry;
    if (!ctx || !ctx->state) return 0;
    entry = spAnimationState_getCurrent(ctx->state, track);
    if (!entry) return 0;
    while (pos-- > 0 && entry) entry = entry->next;
    if (!entry) return 0;
    if (buf && buflen > 0) {
        if (entry->animation && entry->animation->name)
            strncpy(buf, entry->animation->name, (size_t)(buflen - 1));
        buf[buflen - 1] = '\0';
    }
    if (is_empty) *is_empty = entry->animation ? 0 : 1;
    if (loop) *loop = entry->loop ? 1 : 0;
    if (time_scale) *time_scale = entry->timeScale;
    if (mix_duration) *mix_duration = entry->mixDuration;
    if (mix_time) *mix_time = entry->mixTime;
    if (track_time) *track_time = entry->trackTime;
    if (delay) *delay = entry->delay;
    return 1;
}

/* 设置轨道第 pos 个 entry 的速率（热重载恢复用）。返回 1 成功。 */
SP_R_API int spR_setTrackEntryTimeScale(void *vctx, int track, int pos, float scale) {
    spRContext *ctx = vctx;
    spTrackEntry *entry;
    if (!ctx || !ctx->state) return 0;
    entry = spAnimationState_getCurrent(ctx->state, track);
    if (!entry) return 0;
    while (pos-- > 0 && entry) entry = entry->next;
    if (!entry) return 0;
    entry->timeScale = scale;
    return 1;
}

/* 设置轨道第 pos 个 entry 的播放进度（trackTime/mixTime，热重载恢复用）。
 * 返回 1 成功。 */
SP_R_API int spR_setTrackEntryTime(void *vctx, int track, int pos,
                                   float track_time, float mix_time) {
    spRContext *ctx = vctx;
    spTrackEntry *entry;
    if (!ctx || !ctx->state) return 0;
    entry = spAnimationState_getCurrent(ctx->state, track);
    if (!entry) return 0;
    while (pos-- > 0 && entry) entry = entry->next;
    if (!entry) return 0;
    entry->trackTime = track_time;
    entry->mixTime = mix_time;
    return 1;
}

/* 设置轨道第 pos 个 entry 的队列延迟（热重载恢复用，精确还原 addAnimation 的
 * delay 参数；addAnimation 内部会改写 entry->delay，直接重放会有偏差）。
 * 返回 1 成功。 */
SP_R_API int spR_setTrackEntryDelay(void *vctx, int track, int pos, float delay) {
    spRContext *ctx = vctx;
    spTrackEntry *entry;
    if (!ctx || !ctx->state) return 0;
    entry = spAnimationState_getCurrent(ctx->state, track);
    if (!entry) return 0;
    while (pos-- > 0 && entry) entry = entry->next;
    if (!entry) return 0;
    entry->delay = delay;
    return 1;
}

/* 查询 slot 运行时状态：当前 attachment 名与颜色。与 setup 附件/颜色一致时
 * 返回 0（无需恢复）；否则返回 1（buf/颜色带出实际值）。 */
SP_R_API int spR_getSlotState(void *vctx, int slotIndex,
                              char *buf, int buflen,
                              float *r, float *g, float *b, float *a) {
    spRContext *ctx = vctx;
    spSlot *slot;
    const char *cur, *setup;
    int dirty = 0;
    if (!ctx || !ctx->skeleton || slotIndex < 0 ||
        slotIndex >= ctx->skeleton->slotsCount) return 0;
    slot = ctx->skeleton->slots[slotIndex];
    cur = slot->attachment ? slot->attachment->name : NULL;
    if (buf && buflen > 0) {
        if (cur) strncpy(buf, cur, (size_t)(buflen - 1));
        buf[buflen - 1] = '\0';
    }
    if (r) *r = slot->color.r;
    if (g) *g = slot->color.g;
    if (b) *b = slot->color.b;
    if (a) *a = slot->color.a;
    /* 与 setup 附件/颜色比较，判定是否需要恢复 */
    setup = slot->data->attachmentName;
    if ((cur && (!setup || strcmp(cur, setup) != 0)) || (!cur && setup)) dirty = 1;
    if (slot->color.r != slot->data->color.r ||
        slot->color.g != slot->data->color.g ||
        slot->color.b != slot->data->color.b ||
        slot->color.a != slot->data->color.a) dirty = 1;
    return dirty;
}

/* 骨架颜色（热重载存档恢复用）。 */
SP_R_API void spR_getSkeletonColor(void *vctx,
                                   float *r, float *g, float *b, float *a) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return;
    if (r) *r = ctx->skeleton->color.r;
    if (g) *g = ctx->skeleton->color.g;
    if (b) *b = ctx->skeleton->color.b;
    if (a) *a = ctx->skeleton->color.a;
}

/* 查询动画时长（秒）；动画不存在返回 -1。
 * 用于按动画时长动态采样参考包围盒（覆盖完整循环即可）。 */
SP_R_API float spR_getAnimationDuration(void *vctx, const char *name) {
    spRContext *ctx = vctx;
    spAnimation *a;
    if (!ctx || !ctx->skeletonData || !name) return -1.0f;
    a = spSkeletonData_findAnimation(ctx->skeletonData, name);
    return a ? a->duration : -1.0f;
}

/* 骨架定义的全部动画数（按定义顺序）。供 Python 全局并集采样固定视口
 * （SpineViewer 式固定 viewport：遍历所有动画求包围盒并集，创建后不重算）。 */
SP_R_API int spR_getAnimationCount(void *vctx) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeletonData) return 0;
    return ctx->skeletonData->animationsCount;
}

/* 拷贝第 index 个动画名到 buf（与 spR_getPageName 同风格）。成功返回 1。 */
SP_R_API int spR_getAnimationName(void *vctx, int index, char *buf, int buflen) {
    spRContext *ctx = vctx;
    spAnimation *a;
    if (!ctx || !ctx->skeletonData || !buf || buflen <= 0) return 0;
    if (index < 0 || index >= ctx->skeletonData->animationsCount) return 0;
    a = ctx->skeletonData->animations[index];
    if (!a || !a->name) return 0;
    strncpy(buf, a->name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    return 1;
}

/* 返回 slot 总数（skeletonData 的 setup pose 顺序）。 */
SP_R_API int spR_getSlotCount(void *vctx) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeletonData) return 0;
    return ctx->skeletonData->slotsCount;
}

/* 拷贝第 index 个 slot 名到 buf（与 spR_getAnimationName 同风格）。成功返回 1。 */
SP_R_API int spR_getSlotName(void *vctx, int index, char *buf, int buflen) {
    spRContext *ctx = vctx;
    spSlotData *sd;
    if (!ctx || !ctx->skeletonData || !buf || buflen <= 0) return 0;
    if (index < 0 || index >= ctx->skeletonData->slotsCount) return 0;
    sd = ctx->skeletonData->slots[index];
    if (!sd || !sd->name) return 0;
    strncpy(buf, sd->name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    return 1;
}

/* 空动画（对应 spine-unity 的 SetEmptyAnimation）：让指定轨道在 mixDuration 内
 * 淡出到绑定姿势（setup pose），不播放任何动画。 */
SP_R_API void spR_setEmptyAnimation(void *vctx, int track, float mixDuration) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return;
    spAnimationState_setEmptyAnimation(ctx->state, track, mixDuration);
    spAnimationState_apply(ctx->state, ctx->skeleton);
    SP_R_UPDATE_WORLD(ctx->skeleton);
}

/* 对应 spine-unity 的 AddEmptyAnimation：把淡出到绑定姿势加入轨道播放队列。 */
SP_R_API void spR_addEmptyAnimation(void *vctx, int track, float mixDuration, float delay) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return;
    spAnimationState_addEmptyAnimation(ctx->state, track, mixDuration, delay);
}

/* 立即清空指定轨道 / 全部轨道（对应 spine-unity 的 clearTrack / clearTracks）。 */
SP_R_API void spR_clearTrack(void *vctx, int track) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return;
    spAnimationState_clearTrack(ctx->state, track);
}

SP_R_API void spR_clearTracks(void *vctx) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->state) return;
    spAnimationState_clearTracks(ctx->state);
}

SP_R_API int spR_setSkinByName(void *vctx, const char *name) {
    spRContext *ctx = vctx;
    spSkin *skin;
    int i;
    if (!ctx || !ctx->skeleton || !name) return 0;
    skin = spSkeletonData_findSkin(ctx->skeletonData, name);
    if (!skin) {
        /* 组合皮肤不在 skeletonData 的皮肤列表里，回退查组合缓存 */
        for (i = 0; i < ctx->combinedSkinsCount; i++) {
            if (ctx->combinedSkins[i] && strcmp(ctx->combinedSkins[i]->name, name) == 0) {
                skin = ctx->combinedSkins[i];
                break;
            }
        }
        if (!skin) return 0;
    }
    spSkeleton_setSkin(ctx->skeleton, skin);
    return 1;
}

/* 组合皮肤（mix-and-match）：把 skinNames[0..count-1] 合并为一个名为 combinedName
 * 的皮肤并应用到骨架。同名重新组合会重建（3.x 旧对象仅缓存复用，不释放）。
 * 返回 1 成功；任一皮肤不存在返回 0（不改变当前皮肤）。 */
SP_R_API int spR_combineSkins(void *vctx, const char *combinedName,
                              const char **skinNames, int count) {
    spRContext *ctx = vctx;
    spSkin *combined = NULL;
    int i, idx = -1;

    if (!ctx || !ctx->skeleton || !combinedName || !skinNames || count <= 0) return 0;

    for (i = 0; i < ctx->combinedSkinsCount; i++) {
        if (strcmp(ctx->combinedSkins[i]->name, combinedName) == 0) {
            idx = i;
            combined = ctx->combinedSkins[i];
            break;
        }
    }

    if (!combined) {
        combined = spSkin_create(combinedName);
        if (!combined) return 0;
        ctx->combinedSkins = (spSkin **)realloc(ctx->combinedSkins,
                                                (ctx->combinedSkinsCount + 1) * sizeof(spSkin *));
        if (!ctx->combinedSkins) { spSkin_dispose(combined); return 0; }
        idx = ctx->combinedSkinsCount;
        ctx->combinedSkins[idx] = combined;
        ctx->combinedSkinsCount++;
    } else {
#if SPINE_RENPY_VER >= 38
        /* 同名重建：3.8+ 引用计数安全，可释放旧对象后重建 */
        spSkin_dispose(combined);
        combined = spSkin_create(combinedName);
        if (!combined) return 0;
        ctx->combinedSkins[idx] = combined;
#else
        /* 3.5~3.7 无法安全释放共享引用的旧皮肤：保留旧对象，另建新对象接管缓存 */
        combined = spSkin_create(combinedName);
        if (!combined) return 0;
        ctx->combinedSkins[idx] = combined;
#endif
    }

    for (i = 0; i < count; i++) {
        spSkin *src = spSkeletonData_findSkin(ctx->skeletonData, skinNames[i]);
        if (!src) return 0;
#if SPINE_RENPY_VER >= 38
        /* 3.8+：spSkin_addSkin 内部 setAttachment 会递增附件引用计数 */
        spSkin_addSkin(combined, src);
#else
        /* 3.5~3.7：无 spSkin_addSkin，按 slot 枚举附件逐个加入 */
        {
            int slotIndex;
            for (slotIndex = 0; slotIndex < ctx->skeletonData->slotsCount; slotIndex++) {
                int ai = 0;
                const char *aname;
                while ((aname = spSkin_getAttachmentName(src, slotIndex, ai)) != NULL) {
                    spAttachment *att = spSkin_getAttachment(src, slotIndex, aname);
                    if (att) spSkin_addAttachment(combined, slotIndex, aname, att);
                    ai++;
                }
            }
        }
#endif
    }

    spSkeleton_setSkin(ctx->skeleton, combined);
    spSkeleton_setSlotsToSetupPose(ctx->skeleton);
    return 1;
}

/* pass NULL as attachmentName to hide the slot */
SP_R_API int spR_setAttachment(void *vctx, const char *slotName, const char *attachmentName) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return 0;
    return spSkeleton_setAttachment(ctx->skeleton, slotName, attachmentName);
}

SP_R_API void spR_setMix(void *vctx, const char *from, const char *to, float duration) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->stateData) return;
    spAnimationStateData_setMixByName(ctx->stateData, from, to, duration);
}

/* ---------------- slot / skeleton color & setup pose ---------------- */

/* 设置指定 slot 的 RGBA 颜色（0~1，与 spine-unity 的 Slot.SetColor 对齐）。
 * 注意：动画含 color 关键帧时，apply 会覆盖手动设置（spine-unity 同样行为）。 */
SP_R_API int spR_setSlotColor(void *vctx, const char *slotName,
                              float r, float g, float b, float a) {
    spRContext *ctx = vctx;
    spSlot *slot;
    if (!ctx || !ctx->skeleton || !slotName) return 0;
    slot = spSkeleton_findSlot(ctx->skeleton, slotName);
    if (!slot) return 0;
    slot->color.r = r;
    slot->color.g = g;
    slot->color.b = b;
    slot->color.a = a;
    return 1;
}

/* 只改 slot 透明度（保留 RGB），对应 spine-unity 的 slot.A = x。 */
SP_R_API int spR_setSlotAlpha(void *vctx, const char *slotName, float a) {
    spRContext *ctx = vctx;
    spSlot *slot;
    if (!ctx || !ctx->skeleton || !slotName) return 0;
    slot = spSkeleton_findSlot(ctx->skeleton, slotName);
    if (!slot) return 0;
    slot->color.a = a;
    return 1;
}

/* 设置整个骨骼的 RGBA 染色/透明度（与 spine-unity 的 Skeleton.SetColor 对齐）。 */
SP_R_API int spR_setSkeletonColor(void *vctx, float r, float g, float b, float a) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return 0;
    ctx->skeleton->color.r = r;
    ctx->skeleton->color.g = g;
    ctx->skeleton->color.b = b;
    ctx->skeleton->color.a = a;
    return 1;
}

/* 只改整体透明度（保留 RGB），常用于整体淡入淡出。 */
SP_R_API int spR_setSkeletonAlpha(void *vctx, float a) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return 0;
    ctx->skeleton->color.a = a;
    return 1;
}

/* 恢复骨骼/插槽到 setup pose（spine-unity 的 SetToSetupPose 系列）。
 * 仅恢复姿势，不停止动画（动画会在下一帧 apply 重新驱动，同 spine-unity）。 */
SP_R_API void spR_setToSetupPose(void *vctx) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return;
    spSkeleton_setToSetupPose(ctx->skeleton);
    SP_R_UPDATE_WORLD(ctx->skeleton);
}

SP_R_API void spR_setSlotsToSetupPose(void *vctx) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return;
    spSkeleton_setSlotsToSetupPose(ctx->skeleton);
}

SP_R_API void spR_setBonesToSetupPose(void *vctx) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return;
    spSkeleton_setBonesToSetupPose(ctx->skeleton);
    SP_R_UPDATE_WORLD(ctx->skeleton);
}

/* ---------------- slot 下标访问 & 查询（对齐原工程的按下标管理方式） ---------------- */

/* 设置全局默认混合时间（对应 spine-unity 的 AnimationState.Data.DefaultMix）。 */
SP_R_API void spR_setDefaultMix(void *vctx, float duration) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->stateData) return;
    ctx->stateData->defaultMix = duration;
}

/* 按名查 slot 下标，未找到返回 -1（对应 spine-unity 的 skeleton.FindSlotIndex）。
 * 自实现遍历：spSkeleton_findSlotIndex 在 4.0 被移除，而 slots[i]->data->name 全版本可用。 */
static int _spR_findSlotIndex(const spSkeleton *skeleton, const char *name) {
    int i;
    if (!skeleton || !name) return -1;
    for (i = 0; i < skeleton->slotsCount; i++)
        if (skeleton->slots[i]->data->name &&
            strcmp(skeleton->slots[i]->data->name, name) == 0)
            return i;
    return -1;
}

SP_R_API int spR_findSlotIndex(void *vctx, const char *slotName) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return -1;
    return _spR_findSlotIndex(ctx->skeleton, slotName);
}

/* 按下标设置 slot 附件（对应 slot.Attachment = skeleton.GetAttachment(idx, name)；
 * attachmentName 为 NULL/空 表示隐藏该 slot）。 */
SP_R_API int spR_setSlotAttachmentByIndex(void *vctx, int slotIndex, const char *attachmentName) {
    spRContext *ctx = vctx;
    spAttachment *att = NULL;
    if (!ctx || !ctx->skeleton) return 0;
    if (slotIndex < 0 || slotIndex >= ctx->skeleton->slotsCount) return 0;
    if (attachmentName && attachmentName[0]) {
        att = spSkeleton_getAttachmentForSlotIndex(ctx->skeleton, slotIndex, attachmentName);
        if (!att) return 0;
    }
    /* 3.5~3.7 的 attachment 字段是 const 指针不能直接赋值；
     * spSlot_setAttachment 全版本可用（4.0+ 同样安全） */
    spSlot_setAttachment(ctx->skeleton->slots[slotIndex], att);
    return 1;
}

/* 按下标设置 slot 透明度（slot.A = x）。 */
SP_R_API int spR_setSlotAlphaByIndex(void *vctx, int slotIndex, float a) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return 0;
    if (slotIndex < 0 || slotIndex >= ctx->skeleton->slotsCount) return 0;
    ctx->skeleton->slots[slotIndex]->color.a = a;
    return 1;
}

/* 单 slot 复位到 setup pose（slot.SetToSetupPose）。 */
SP_R_API int spR_setSlotToSetupPose(void *vctx, int slotIndex) {
    spRContext *ctx = vctx;
    if (!ctx || !ctx->skeleton) return 0;
    if (slotIndex < 0 || slotIndex >= ctx->skeleton->slotsCount) return 0;
    spSlot_setToSetupPose(ctx->skeleton->slots[slotIndex]);
    return 1;
}

/* 读取 slot 当前附件名（无附件返回 0，成功返回 1 并写入 buf）。 */
SP_R_API int spR_getSlotAttachmentName(void *vctx, int slotIndex, char *buf, int buflen) {
    spRContext *ctx = vctx;
    spAttachment *att;
    if (!ctx || !ctx->skeleton || !buf || buflen <= 0) return 0;
    if (slotIndex < 0 || slotIndex >= ctx->skeleton->slotsCount) return 0;
    att = ctx->skeleton->slots[slotIndex]->attachment;
    if (!att || !att->name) return 0;
    strncpy(buf, att->name, buflen - 1);
    buf[buflen - 1] = '\0';
    return 1;
}

/* 读取 slot 的 setup 附件名（slot.Data.AttachmentName，无则返回 0）。 */
SP_R_API int spR_getSlotSetupAttachmentName(void *vctx, int slotIndex, char *buf, int buflen) {
    spRContext *ctx = vctx;
    const char *name;
    if (!ctx || !ctx->skeleton || !buf || buflen <= 0) return 0;
    if (slotIndex < 0 || slotIndex >= ctx->skeleton->slotsCount) return 0;
    name = ctx->skeleton->slots[slotIndex]->data->attachmentName;
    if (!name) return 0;
    strncpy(buf, name, buflen - 1);
    buf[buflen - 1] = '\0';
    return 1;
}

/* 皮肤存在性检查（Data.FindSkin；组合皮肤缓存也纳入检查）。 */
SP_R_API int spR_hasSkin(void *vctx, const char *name) {
    spRContext *ctx = vctx;
    int i;
    if (!ctx || !ctx->skeletonData || !name) return 0;
    if (spSkeletonData_findSkin(ctx->skeletonData, name)) return 1;
    for (i = 0; i < ctx->combinedSkinsCount; i++)
        if (ctx->combinedSkins[i] && strcmp(ctx->combinedSkins[i]->name, name) == 0)
            return 1;
    return 0;
}

/* collect visible region & mesh attachments in draw order. returns count.
 * 容量不足时返回 -所需数量（与 spR_buildMesh 扩容约定一致），调用方扩容后重试。
 * 裁剪附件（SP_ATTACHMENT_CLIPPING）不产出渲染项，而是驱动 clipper：
 * clipStart 在裁剪附件所在 slot 处启用裁剪，其后 region/mesh 附件经
 * clipTriangles 裁出实际可见部分（写公共缓冲），clipEnd 在 endSlot 处结束。 */
SP_R_API int spR_collectDrawItems(void *vctx, spRDrawItem *items, int maxItems) {
    spRContext *ctx = vctx;
    spSkeleton *skel;
    int i, count = 0;
    if (!ctx || !ctx->skeleton) return 0;
    /* 上一帧若存在 endSlot 在 clipStart 之前的裁剪附件（如 end 指向前面 slot），
     * clipEnd 永远匹配不上，clipper 残留开启；下一帧从第一项起就全被裁剪，
     * 只剩裁剪多边形内一小块。每帧收集前无条件闭合上一帧的残留裁剪。 */
    spSkeletonClipping_clipEnd2(ctx->clipper);
    skel = ctx->skeleton;
    /* 第一遍：统计可见 region/mesh 附件总数（clipping 附件不产出渲染项），
     * 超限返回负值触发调用方扩容 */
    for (i = 0; i < skel->slotsCount; ++i) {
        spAttachment *att = skel->drawOrder[i]->attachment;
        if (att && att->type != SP_ATTACHMENT_CLIPPING) ++count;
    }
    if (count > maxItems) return -count;
    /* 第二遍：按 drawOrder 写入渲染项 */
    count = 0;
    for (i = 0; i < skel->slotsCount; ++i) {
        spSlot *slot = skel->drawOrder[i];
        spAttachment *att = slot->attachment;
        spAtlasRegion *atl;
        spRDrawItem *item;
        const spColor *ac, *sc, *kc;

        if (!att) {
            spSkeletonClipping_clipEnd(ctx->clipper, slot);
            continue;
        }

        /* 裁剪附件：启用裁剪，不产出渲染项（嵌套裁剪返回 0，忽略即可） */
        if (att->type == SP_ATTACHMENT_CLIPPING) {
            spSkeletonClipping_clipStart(ctx->clipper, slot, (spClippingAttachment *)att);
            continue;
        }

        sc = &slot->color;
        kc = &skel->color;
        item = &items[count];
        item->slotIndex = i;
        item->vertsCount = 0;
        item->trianglesCount = 0;

        if (att->type == SP_ATTACHMENT_REGION) {
            spRegionAttachment *region = (spRegionAttachment *)att;

            /* loader stores spAtlasRegion* in rendererObject in every version */
            atl = (spAtlasRegion *)region->rendererObject;
            item->texIndex = (atl && atl->page) ? (int)(intptr_t)atl->page->rendererObject : -1;

            SP_R_COMPUTE_REGION(region, slot, item->vertices);
            memcpy(item->uvs, region->uvs, sizeof(item->uvs));

            if (spSkeletonClipping_isClipping(ctx->clipper)) {
                /* 裁剪中的 region 按 4 顶点 mesh 参与裁剪，结果写公共缓冲 */
                unsigned short rtris[6] = {0, 1, 2, 0, 2, 3};
                spSkeletonClipping_clipTriangles(ctx->clipper, item->vertices, 8,
                                                 rtris, 6, item->uvs, 2);
                if (!_spR_emitClipped(ctx, item, ctx->clipper)) {
                    spSkeletonClipping_clipEnd(ctx->clipper, slot);
                    continue; /* 完全被裁掉，不产出渲染项 */
                }
            }

            ac = &region->color;
        } else if (att->type == SP_ATTACHMENT_MESH || att->type == SP_ATTACHMENT_LINKED_MESH) {
            spMeshAttachment *mesh = (spMeshAttachment *)att;
            int vlen = mesh->super.worldVerticesLength; /* float 数，每顶点 2 个 */
            int tcount = mesh->trianglesCount;          /* unsigned short 数，每三角形 3 个 */

            atl = (spAtlasRegion *)mesh->rendererObject;
            item->texIndex = (atl && atl->page) ? (int)(intptr_t)atl->page->rendererObject : -1;

            if (spSkeletonClipping_isClipping(ctx->clipper)) {
                /* 裁剪中的 mesh：先算世界顶点，再裁剪，结果写公共缓冲 */
                float *wv = _spR_tmpVerts(ctx, vlen);
                if (!wv) return 0;
                spVertexAttachment_computeWorldVertices((spVertexAttachment *)mesh, slot, 0,
                                                        vlen, wv, 0, 2);
                spSkeletonClipping_clipTriangles(ctx->clipper, wv, vlen,
                                                 mesh->triangles, tcount, mesh->uvs, 2);
                if (!_spR_emitClipped(ctx, item, ctx->clipper)) {
                    spSkeletonClipping_clipEnd(ctx->clipper, slot);
                    continue; /* 完全被裁掉，不产出渲染项 */
                }
            } else {
                /* 非裁剪：原逻辑（写入本帧公共缓冲，不截断） */
                float *vbuf, *ubuf;
                unsigned short *tbuf;

                /* 按累计用量扩容：一次扩容到位，避免中途 realloc 使已写入数据失效 */
                vbuf = _spR_meshVerts(ctx, ctx->meshVertsUsed + vlen);
                ubuf = _spR_meshUVs(ctx, ctx->meshUVsUsed + vlen);
                tbuf = _spR_meshTris(ctx, ctx->meshTrisUsed + tcount);
                if (!vbuf || !ubuf || !tbuf) {
                    spSkeletonClipping_clipEnd(ctx->clipper, slot);
                    continue; /* 扩容失败：跳过该附件 */
                }

                item->vertsOffset = ctx->meshVertsUsed; /* == ctx->meshUVsUsed */
                item->trisOffset = ctx->meshTrisUsed;
                ctx->meshVertsUsed += vlen;
                ctx->meshUVsUsed += vlen;
                ctx->meshTrisUsed += tcount;

                /* 所有版本签名一致：(self, slot, start, count=float数, out, offset=0, stride=2) */
                spVertexAttachment_computeWorldVertices((spVertexAttachment *)mesh, slot, 0,
                                                        vlen, vbuf + item->vertsOffset, 0, 2);
                memcpy(ubuf + item->vertsOffset, mesh->uvs, sizeof(float) * vlen);
                memcpy(tbuf + item->trisOffset, mesh->triangles, sizeof(unsigned short) * tcount);
                item->vertsCount = vlen;
                item->trianglesCount = tcount;
            }

            ac = &mesh->color;
        } else {
            continue; /* 其余附件类型（bounding box/path/point）不渲染 */
        }

        item->color[0] = ac->r * sc->r * kc->r;
        item->color[1] = ac->g * sc->g * kc->g;
        item->color[2] = ac->b * sc->b * kc->b;
        item->color[3] = ac->a * sc->a * kc->a;
        count++;
        spSkeletonClipping_clipEnd(ctx->clipper, slot);
    }
    return count;
}

SP_R_API int spR_getPageCount(void *vctx) {
    spRContext *ctx = vctx;
    spAtlasPage *p;
    int n = 0;
    if (!ctx || !ctx->atlas) return 0;
    for (p = ctx->atlas->pages; p; p = p->next) n++;
    return n;
}

/* ---------------- 合并 mesh 构建（高频渲染路径，C 层下沉） ---------------- */

/* 保证 ctx->tmpVerts 至少有 n 个 float 容量；失败返回 NULL。 */
static float *_spR_tmpVerts(spRContext *ctx, int n) {
    float *p;
    if (ctx->tmpVertsCap >= n) return ctx->tmpVerts;
    p = (float *)realloc(ctx->tmpVerts, sizeof(float) * n);
    if (!p) return NULL;
    ctx->tmpVerts = p;
    ctx->tmpVertsCap = n;
    return p;
}

/* 本帧 mesh 公共缓冲扩容（collectDrawItems 专用）。三缓冲独立扩容，
 * 均按累计用量一次到位，避免中途 realloc 使已写入数据失效。 */
static float *_spR_meshVerts(spRContext *ctx, int n) {
    float *p;
    if (ctx->meshVertsCap >= n) return ctx->meshVertsBuf;
    p = (float *)realloc(ctx->meshVertsBuf, sizeof(float) * n);
    if (!p) return NULL;
    ctx->meshVertsBuf = p;
    ctx->meshVertsCap = n;
    return p;
}

static float *_spR_meshUVs(spRContext *ctx, int n) {
    float *p;
    if (ctx->meshUVsCap >= n) return ctx->meshUVsBuf;
    p = (float *)realloc(ctx->meshUVsBuf, sizeof(float) * n);
    if (!p) return NULL;
    ctx->meshUVsBuf = p;
    ctx->meshUVsCap = n;
    return p;
}

static unsigned short *_spR_meshTris(spRContext *ctx, int n) {
    unsigned short *p;
    if (ctx->meshTrisCap >= n) return ctx->meshTrisBuf;
    p = (unsigned short *)realloc(ctx->meshTrisBuf, sizeof(unsigned short) * n);
    if (!p) return NULL;
    ctx->meshTrisBuf = p;
    ctx->meshTrisCap = n;
    return p;
}

/* 把 clipper 裁剪结果写入本帧 mesh 公共缓冲并设置 item 偏移/计数。
 * 裁剪输出与 mesh 附件同布局：clippedVertices=(x,y) 对、clippedUVs=(u,v) 对、
 * clippedTriangles=索引。返回 1 成功；0 表示完全被裁掉或扩容失败（不产出渲染项）。 */
static int _spR_emitClipped(spRContext *ctx, spRDrawItem *item, spSkeletonClipping *clipper) {
    spFloatArray *cv = clipper->clippedVertices;
    spFloatArray *cu = clipper->clippedUVs;
    spUnsignedShortArray *ct = clipper->clippedTriangles;
    int vlen = cv->size;   /* float 数（xy 对） */
    int tcount = ct->size; /* unsigned short 数（索引） */
    float *vbuf, *ubuf;
    unsigned short *tbuf;
    if (tcount == 0) return 0; /* 完全被裁掉 */
    /* 按累计用量扩容：一次扩容到位，避免中途 realloc 使已写入数据失效 */
    vbuf = _spR_meshVerts(ctx, ctx->meshVertsUsed + vlen);
    ubuf = _spR_meshUVs(ctx, ctx->meshUVsUsed + vlen);
    tbuf = _spR_meshTris(ctx, ctx->meshTrisUsed + tcount);
    if (!vbuf || !ubuf || !tbuf) return 0;
    item->vertsOffset = ctx->meshVertsUsed; /* == ctx->meshUVsUsed */
    item->trisOffset = ctx->meshTrisUsed;
    ctx->meshVertsUsed += vlen;
    ctx->meshUVsUsed += vlen;
    ctx->meshTrisUsed += tcount;
    memcpy(vbuf + item->vertsOffset, cv->items, sizeof(float) * vlen);
    memcpy(ubuf + item->vertsOffset, cu->items, sizeof(float) * vlen);
    memcpy(tbuf + item->trisOffset, ct->items, sizeof(unsigned short) * tcount);
    item->vertsCount = vlen;
    item->trianglesCount = tcount;
    return 1;
}

/* 返回最近一次 spR_collectDrawItems 写入的 mesh 公共缓冲及其用量。
 * 缓冲随下一次收集复用/失效，调用方不得长期持有。 */
SP_R_API void spR_getMeshBufs(void *vctx, float **verts, float **uvs,
                              unsigned short **tris, int *vertsCount,
                              int *uvsCount, int *trisCount) {
    spRContext *ctx = vctx;
    if (verts) *verts = ctx ? ctx->meshVertsBuf : NULL;
    if (uvs) *uvs = ctx ? ctx->meshUVsBuf : NULL;
    if (tris) *tris = ctx ? ctx->meshTrisBuf : NULL;
    if (vertsCount) *vertsCount = ctx ? ctx->meshVertsUsed : 0;
    if (uvsCount) *uvsCount = ctx ? ctx->meshUVsUsed : 0;
    if (trisCount) *trisCount = ctx ? ctx->meshTrisUsed : 0;
}

/* 计算当前帧全部可见 region/mesh 附件的世界坐标包围盒。
 * 成功返回 1 并写出 (minX, minY, maxX, maxY)；无可见附件返回 0。
 * 采样固定参考包围盒用（10fps 采样动画全程，C 层直接遍历，无需经结构体拷贝）。 */
SP_R_API int spR_collectBounds(void *vctx, float *minX, float *minY, float *maxX, float *maxY) {
    spRContext *ctx = vctx;
    spSkeleton *skel;
    int i, found = 0;
    float mnx, mny, mxx, mxy;
    if (!ctx || !ctx->skeleton) return 0;
    skel = ctx->skeleton;
    mnx = mny = 1e30f;
    mxx = mxy = -1e30f;
    for (i = 0; i < skel->slotsCount; ++i) {
        spSlot *slot = skel->drawOrder[i];
        spAttachment *att = slot->attachment;
        int j;
        if (!att) continue;
        if (att->type == SP_ATTACHMENT_REGION) {
            spRegionAttachment *region = (spRegionAttachment *)att;
            float verts[8];
            SP_R_COMPUTE_REGION(region, slot, verts);
            for (j = 0; j < 4; j++) {
                if (verts[j * 2] < mnx) mnx = verts[j * 2];
                if (verts[j * 2] > mxx) mxx = verts[j * 2];
                if (verts[j * 2 + 1] < mny) mny = verts[j * 2 + 1];
                if (verts[j * 2 + 1] > mxy) mxy = verts[j * 2 + 1];
            }
            found = 1;
        } else if (att->type == SP_ATTACHMENT_MESH || att->type == SP_ATTACHMENT_LINKED_MESH) {
            spMeshAttachment *mesh = (spMeshAttachment *)att;
            int vlen = mesh->super.worldVerticesLength; /* float 数 */
            float *wv = _spR_tmpVerts(ctx, vlen);
            if (!wv) return 0;
            spVertexAttachment_computeWorldVertices((spVertexAttachment *)mesh, slot, 0, vlen, wv, 0, 2);
            for (j = 0; j < vlen; j += 2) {
                if (wv[j] < mnx) mnx = wv[j];
                if (wv[j] > mxx) mxx = wv[j];
                if (wv[j + 1] < mny) mny = wv[j + 1];
                if (wv[j + 1] > mxy) mxy = wv[j + 1];
            }
            found = 1;
        }
    }
    if (!found) return 0;
    *minX = mnx; *minY = mny; *maxX = mxx; *maxY = mxy;
    return 1;
}

/* 把所有可见附件合并进"单 Mesh2 数据缓冲"（一次 draw call 的顶点布局），
 * 顶点/uv/颜色/索引全部在 C 层算好，Python 端只做一次 memcpy 进 Mesh2。
 *
 * 输入（合成图集信息，与 Python _ensure_atlas 的结果对应）：
 *   atlasOffsets[i]  : 第 i 页在合成图中的水平偏移 x
 *   atlasPageW/H[i]  : 第 i 页宽高
 *   atlasW/atlasH    : 合成图总宽高
 * 输出布局（每顶点 2+6 个 float，与 Python shader 的 a_position/a_tex_coord/a_color 对应）：
 *   geo[vi*2]        = (世界x - minX) * zoom
 *   geo[vi*2+1]      = (世界y - minY) * zoom
 *   attrs[vi*6]      = (uv_u * 页宽 + 页偏移) / 合成图宽
 *   attrs[vi*6+1]    = uv_v * 页高 / 合成图高
 *   attrs[vi*6+2..5] = 附件 x slot x skeleton 乘积颜色 RGBA
 *   tris             : mesh 附件原样 + 顶点偏移；region 附件补四边形 [0,1,2, 0,2,3]
 *
 * 返回顶点数（>=0）；无可见附件返回 0；缓冲不足返回 -(所需顶点数)。
 * outTriangles 写出实际三角形数。 */
SP_R_API int spR_buildMesh(void *vctx, float minX, float minY, float zoom,
                           const float *atlasOffsets, const float *atlasPageW,
                           const float *atlasPageH, int pageCount,
                           float atlasW, float atlasH,
                           float *geo, float *attrs, unsigned short *tris,
                           int maxVerts, int maxTris, int *outTriangles) {
    spRContext *ctx = vctx;
    spSkeleton *skel;
    int i, vi = 0, ti = 0;
    if (!ctx || !ctx->skeleton) return 0;
    /* 与 spR_collectDrawItems 相同：清掉上一帧残留的未闭合裁剪（见该函数注释） */
    spSkeletonClipping_clipEnd2(ctx->clipper);
    skel = ctx->skeleton;
    for (i = 0; i < skel->slotsCount; ++i) {
        spSlot *slot = skel->drawOrder[i];
        spAttachment *att = slot->attachment;
        spAtlasRegion *atl;
        const spColor *ac, *sc, *kc;
        int page, j;
        float offX, pw, ph;
        if (!att) {
            spSkeletonClipping_clipEnd(ctx->clipper, slot);
            continue;
        }
        /* 裁剪附件：启用裁剪，不产出几何（嵌套裁剪返回 0，忽略即可） */
        if (att->type == SP_ATTACHMENT_CLIPPING) {
            spSkeletonClipping_clipStart(ctx->clipper, slot, (spClippingAttachment *)att);
            continue;
        }
        sc = &slot->color;
        kc = &skel->color;
        if (att->type == SP_ATTACHMENT_REGION) {
            spRegionAttachment *region = (spRegionAttachment *)att;
            float verts[8];
            atl = (spAtlasRegion *)region->rendererObject;
            page = (atl && atl->page) ? (int)(intptr_t)atl->page->rendererObject : -1;
            if (page < 0 || page >= pageCount) {
                spSkeletonClipping_clipEnd(ctx->clipper, slot);
                continue;
            }
            offX = atlasOffsets[page];
            pw = atlasPageW[page];
            ph = atlasPageH[page];
            SP_R_COMPUTE_REGION(region, slot, verts);
            ac = &region->color;
            if (spSkeletonClipping_isClipping(ctx->clipper)) {
                /* 裁剪中的 region 按 4 顶点 mesh 参与裁剪，输出为顶点/uv/索引数组 */
                unsigned short rtris[6] = {0, 1, 2, 0, 2, 3};
                spFloatArray *cv, *cu;
                spUnsignedShortArray *ct;
                int vlen, tcount, vcount, ttri;
                spSkeletonClipping_clipTriangles(ctx->clipper, verts, 8, rtris, 6, region->uvs, 2);
                cv = ctx->clipper->clippedVertices;
                cu = ctx->clipper->clippedUVs;
                ct = ctx->clipper->clippedTriangles;
                vlen = cv->size;
                tcount = ct->size;
                if (tcount == 0) { /* 完全被裁掉 */
                    spSkeletonClipping_clipEnd(ctx->clipper, slot);
                    continue;
                }
                vcount = vlen / 2;
                ttri = tcount / 3;
                if (vi + vcount > maxVerts || ti + ttri > maxTris) return -(vi + vcount);
                for (j = 0; j < vcount; j++) {
                    geo[vi * 2] = (cv->items[j * 2] - minX) * zoom;
                    geo[vi * 2 + 1] = (cv->items[j * 2 + 1] - minY) * zoom;
                    attrs[vi * 6] = (cu->items[j * 2] * pw + offX) / atlasW;
                    attrs[vi * 6 + 1] = cu->items[j * 2 + 1] * ph / atlasH;
                    attrs[vi * 6 + 2] = ac->r * sc->r * kc->r;
                    attrs[vi * 6 + 3] = ac->g * sc->g * kc->g;
                    attrs[vi * 6 + 4] = ac->b * sc->b * kc->b;
                    attrs[vi * 6 + 5] = ac->a * sc->a * kc->a;
                    vi++;
                }
                for (j = 0; j < tcount; j++)
                    tris[ti * 3 + j] = (unsigned short)(ct->items[j] + (vi - vcount));
                ti += ttri;
            } else {
                /* 原逻辑：region 补四边形 [0,1,2, 0,2,3] */
                if (vi + 4 > maxVerts || ti + 2 > maxTris) return -(vi + 4);
                for (j = 0; j < 4; j++) {
                    geo[vi * 2] = (verts[j * 2] - minX) * zoom;
                    geo[vi * 2 + 1] = (verts[j * 2 + 1] - minY) * zoom;
                    attrs[vi * 6] = (region->uvs[j * 2] * pw + offX) / atlasW;
                    attrs[vi * 6 + 1] = region->uvs[j * 2 + 1] * ph / atlasH;
                    attrs[vi * 6 + 2] = ac->r * sc->r * kc->r;
                    attrs[vi * 6 + 3] = ac->g * sc->g * kc->g;
                    attrs[vi * 6 + 4] = ac->b * sc->b * kc->b;
                    attrs[vi * 6 + 5] = ac->a * sc->a * kc->a;
                    vi++;
                }
                tris[ti * 3] = (unsigned short)(vi - 4);
                tris[ti * 3 + 1] = (unsigned short)(vi - 3);
                tris[ti * 3 + 2] = (unsigned short)(vi - 2);
                tris[ti * 3 + 3] = (unsigned short)(vi - 4);
                tris[ti * 3 + 4] = (unsigned short)(vi - 2);
                tris[ti * 3 + 5] = (unsigned short)(vi - 1);
                ti += 2;  /* 每个 region 贡献 2 个三角形（ti 按三角形计数，与 mesh 分支一致） */
            }
            spSkeletonClipping_clipEnd(ctx->clipper, slot);
        } else if (att->type == SP_ATTACHMENT_MESH || att->type == SP_ATTACHMENT_LINKED_MESH) {
            spMeshAttachment *mesh = (spMeshAttachment *)att;
            int vlen = mesh->super.worldVerticesLength; /* float 数，每顶点 2 个 */
            int vcount = vlen / 2;
            int tcount = mesh->trianglesCount;
            int ttri = tcount / 3;
            float *wv;
            atl = (spAtlasRegion *)mesh->rendererObject;
            page = (atl && atl->page) ? (int)(intptr_t)atl->page->rendererObject : -1;
            if (page < 0 || page >= pageCount) {
                spSkeletonClipping_clipEnd(ctx->clipper, slot);
                continue;
            }
            offX = atlasOffsets[page];
            pw = atlasPageW[page];
            ph = atlasPageH[page];
            wv = _spR_tmpVerts(ctx, vlen);
            if (!wv) return 0;
            /* 所有版本签名一致：(self, slot, start, count=float数, out, offset=0, stride=2) */
            spVertexAttachment_computeWorldVertices((spVertexAttachment *)mesh, slot, 0, vlen, wv, 0, 2);
            ac = &mesh->color;
            if (spSkeletonClipping_isClipping(ctx->clipper)) {
                /* 裁剪中的 mesh：世界顶点喂给 clipTriangles，输出为顶点/uv/索引数组 */
                spFloatArray *cv, *cu;
                spUnsignedShortArray *ct;
                int cvlen, ctcount, cvcount, cttri;
                spSkeletonClipping_clipTriangles(ctx->clipper, wv, vlen,
                                                 mesh->triangles, tcount, mesh->uvs, 2);
                cv = ctx->clipper->clippedVertices;
                cu = ctx->clipper->clippedUVs;
                ct = ctx->clipper->clippedTriangles;
                cvlen = cv->size;
                ctcount = ct->size;
                if (ctcount == 0) { /* 完全被裁掉 */
                    spSkeletonClipping_clipEnd(ctx->clipper, slot);
                    continue;
                }
                cvcount = cvlen / 2;
                cttri = ctcount / 3;
                if (vi + cvcount > maxVerts || ti + cttri > maxTris) return -(vi + cvcount);
                for (j = 0; j < cvcount; j++) {
                    geo[vi * 2] = (cv->items[j * 2] - minX) * zoom;
                    geo[vi * 2 + 1] = (cv->items[j * 2 + 1] - minY) * zoom;
                    attrs[vi * 6] = (cu->items[j * 2] * pw + offX) / atlasW;
                    attrs[vi * 6 + 1] = cu->items[j * 2 + 1] * ph / atlasH;
                    attrs[vi * 6 + 2] = ac->r * sc->r * kc->r;
                    attrs[vi * 6 + 3] = ac->g * sc->g * kc->g;
                    attrs[vi * 6 + 4] = ac->b * sc->b * kc->b;
                    attrs[vi * 6 + 5] = ac->a * sc->a * kc->a;
                    vi++;
                }
                for (j = 0; j < ctcount; j++)
                    tris[ti * 3 + j] = (unsigned short)(ct->items[j] + (vi - cvcount));
                ti += cttri;
            } else {
                /* 原逻辑：mesh 附件原样 + 顶点偏移 */
                if (vi + vcount > maxVerts || ti + ttri > maxTris) return -(vi + vcount);
                for (j = 0; j < vcount; j++) {
                    geo[vi * 2] = (wv[j * 2] - minX) * zoom;
                    geo[vi * 2 + 1] = (wv[j * 2 + 1] - minY) * zoom;
                    attrs[vi * 6] = (mesh->uvs[j * 2] * pw + offX) / atlasW;
                    attrs[vi * 6 + 1] = mesh->uvs[j * 2 + 1] * ph / atlasH;
                    attrs[vi * 6 + 2] = ac->r * sc->r * kc->r;
                    attrs[vi * 6 + 3] = ac->g * sc->g * kc->g;
                    attrs[vi * 6 + 4] = ac->b * sc->b * kc->b;
                    attrs[vi * 6 + 5] = ac->a * sc->a * kc->a;
                    vi++;
                }
                for (j = 0; j < tcount; j++)
                    tris[ti * 3 + j] = (unsigned short)(mesh->triangles[j] + (vi - vcount));
                ti += ttri;
            }
            spSkeletonClipping_clipEnd(ctx->clipper, slot);
        }
        /* 其余附件类型（bounding box/path/point）不渲染 */
    }
    if (outTriangles) *outTriangles = ti;
    return vi;
}

/* copy the image file name of the index-th page (relative to atlas dir). returns 1 on success. */
SP_R_API int spR_getPageName(void *vctx, int index, char *buf, int buflen) {
    spRContext *ctx = vctx;
    spAtlasPage *p;
    if (!ctx || !ctx->atlas || !buf || buflen <= 0) return 0;
    for (p = ctx->atlas->pages; p && index > 0; p = p->next) index--;
    if (!p || !p->name) return 0;
    strncpy(buf, p->name, buflen - 1);
    buf[buflen - 1] = '\0';
    return 1;
}


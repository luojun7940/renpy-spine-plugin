/* spine_renpy43.cpp
 * =================
 * Spine 4.3 bridge layer.
 *
 * Spine 4.3 的 spine-c 是"spine-cpp 的 C 包裹层"（src/generated/*.cpp +
 * src/extensions.cpp），API 前缀为 spine_*，与 3.5~4.2 的旧 C API 完全不同。
 * 本文件用该 4.3 API 实现与 spine_renpy.c 完全一致的 40 个 spR_* 稳定 ABI 函数，
 * 使 Python/ctypes 中间层（spine_core.py）无需改动即可加载 4.3 数据。
 *
 * 4.3 关键差异（本文件内部消化，对 ABI 透明）：
 *   - slot 状态位于 poses：渲染读取 slot 的 appliedPose（未受约束时即 pose）。
 *   - 附件 color / UV / 顶点：region 与 mesh 都通过 sequence 取（spine_sequence_*），
 *     sequence 的 UV 为页内 0..1 坐标，与旧 region->uvs / mesh->uvs 语义一致。
 *   - texIndex：texture loader 把 atlas 页索引写入 page.texture，
 *     Atlas 解析时 region._rendererObject = page.texture，故
 *     spine_texture_region_get_renderer_object(region) 即页索引。
 *   - 事件枚举：4.3 为 START=0 INTERRUPT=1 END=2 DISPOSE=3 COMPLETE=4 EVENT=5，
 *     而 ABI 期望 COMPLETE=3 DISPOSE=4，转发时交换。
 *   - updateWorldTransform 需要 physics 参数：使用 SPINE_PHYSICS_UPDATE。
 *   - atlas：spine_atlas_load(文本) 只解析不读盘（loader 把页索引当 texture），
 *     与旧 spAtlas_createFromFile 的"只取页面信息"行为等价。
 *
 * 编译：与 spine-cpp 全部源码 + spine-c generated 源码 + extensions.cpp 一起
 * 以 C++ 编译为 spine4.3.dll（见 build_one43.ps1）。
 */
#include <spine-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* C++ 下必须 extern "C"，否则导出的是 C++ 修饰名（?spR_xxx@@...），
 * Python ctypes 按未修饰名 GetProcAddress 会找不到。 */
#if defined(_WIN32)
#define SP_R_API extern "C" __declspec(dllexport)
#else
#define SP_R_API extern "C"
#endif

/* spR_buildMeshEx 段容量不足时的哨兵返回值（负数，不会与 -(所需顶点数) 混淆） */
#define SP_R_NEED_SEGMENTS (-0x10000000)

/* 4.3 的 updateWorldTransform 需要 physics 参数（Physics_Update） */
#define SP_R_UPDATE_WORLD(s) spine_skeleton_update_world_transform((s), SPINE_PHYSICS_UPDATE)

/* ---------------- data structures ---------------- */

/* 与 spine_renpy.c 完全一致的 listener 回调（Python RListenerCB 的镜像） */
typedef void (*spRListenerCallback)(void *userData, int type,
                                    const char *animationName,
                                    const char *eventName,
                                    float eventTime,
                                    int intValue,
                                    float floatValue,
                                    const char *stringValue);

typedef struct spRContext {
    spine_atlas atlas;
    spine_skeleton_data skeletonData;
    spine_animation_state_data stateData;
    spine_animation_state state;
    spine_skeleton skeleton;
    char error[512];
    spRListenerCallback userListener;   /* Python 注册的回调，NULL 表示不转发 */
    void *listenerUserData;
    spine_skin *combinedSkins;          /* 组合皮肤缓存（同名复用；4.3 引用计数安全可释放） */
    int combinedSkinsCount;
    float *tmpVerts;                    /* 顶点临时缓冲（buildMesh/collectBounds 复用） */
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
    /* 裁剪器（4.3 spine_skeleton_clipping）：渲染循环中
     * clip_start / clip_triangles_2 / clip_end_1 驱动裁剪附件。 */
    spine_skeleton_clipping clipper;
    /* 是否独占 atlas/skeletonData：spR_create 旧路径置 1（dispose 时连带释放），
     * spR_createSkeleton 共享路径置 0（data 归共享层 spR_disposeData 管理）。 */
    int ownsData;
} spRContext;

/* 共享数据层句柄：atlas + skeletonData 只解析一次，由多个运行时 spRContext
 * 共享（spR_createSkeleton）。生命周期由 Python 侧引用计数管理：
 * spR_loadData / spR_loadDataMem 创建，spR_disposeData 释放。 */
typedef struct spRData {
    spine_atlas atlas;
    spine_skeleton_data skeletonData;
    char error[512];
} spRData;

/* spRDrawItem 的 mesh 数据写入本帧公共缓冲（动态扩容），结构体内只记偏移；
 * 256 顶点上限时代的固定数组已移除，超大 mesh 附件不再截断。
 * 公共缓冲由 spR_getMeshBufs 返回，随下一次 spR_collectDrawItems 失效。 */

typedef struct spRDrawItem {
    int slotIndex;     /* drawOrder 中的下标（与 spine_renpy.c 行为一致） */
    int texIndex;      /* atlas page index (which png) */
    float vertices[8]; /* 4 corners in world x,y (order br, bl, ul, ur) - region only */
    float uvs[8];      /* 4 corners in texture u,v - region only */
    float color[4];    /* r,g,b,a in [0,1] (skeleton x slot x attachment) */
    /* mesh 附件数据（region 附件时 vertsCount/trianglesCount 为 0） */
    int vertsCount;                 /* 顶点 float 数（mesh only，每顶点 2 个） */
    int trianglesCount;             /* 三角形 ushort 数（mesh only） */
    int vertsOffset;                /* meshVerts/meshUVs 在本帧公共缓冲的 float 偏移（两数组同步推进，偏移相同） */
    int trisOffset;                 /* meshTris 在本帧公共缓冲的 unsigned short 偏移 */
} spRDrawItem;

/* ---------------- forward declarations ---------------- */

static void _spR_forwardListener(spine_animation_state state, spine_event_type type,
                                 spine_track_entry entry, spine_event event, void *user_data);

/* ---------------- small helpers ---------------- */

/* 判断骨架文件是否为 JSON 文本：跳过开头空白（含 UTF-8 BOM）后以 '{' 开头。 */
static int is_skeleton_json_file(const char *path) {
    FILE *f = fopen(path, "rb");
    int c;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == 0xEF || c == 0xBB || c == 0xBF)
            continue;
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
            continue;
        return c == '{';
    }
    return 0;
}

/* 读整个文本文件（atlas 文本），失败返回 NULL。 */
static char *read_file_text(const char *path) {
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* 保证 ctx->tmpVerts 至少有 n 个 float 容量；失败返回 NULL。 */
static float *_spR_tmpVerts(spRContext *ctx, int n) {
    float *p;
    if (ctx->tmpVertsCap >= n) return ctx->tmpVerts;
    p = (float *)realloc(ctx->tmpVerts, sizeof(float) * (size_t)n);
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
    p = (float *)realloc(ctx->meshVertsBuf, sizeof(float) * (size_t)n);
    if (!p) return NULL;
    ctx->meshVertsBuf = p;
    ctx->meshVertsCap = n;
    return p;
}

static float *_spR_meshUVs(spRContext *ctx, int n) {
    float *p;
    if (ctx->meshUVsCap >= n) return ctx->meshUVsBuf;
    p = (float *)realloc(ctx->meshUVsBuf, sizeof(float) * (size_t)n);
    if (!p) return NULL;
    ctx->meshUVsBuf = p;
    ctx->meshUVsCap = n;
    return p;
}

static unsigned short *_spR_meshTris(spRContext *ctx, int n) {
    unsigned short *p;
    if (ctx->meshTrisCap >= n) return ctx->meshTrisBuf;
    p = (unsigned short *)realloc(ctx->meshTrisBuf, sizeof(unsigned short) * (size_t)n);
    if (!p) return NULL;
    ctx->meshTrisBuf = p;
    ctx->meshTrisCap = n;
    return p;
}

/* 把 clipper 裁剪结果写入本帧 mesh 公共缓冲并设置 item 偏移/计数。
 * 裁剪输出与 mesh 附件同布局：clippedVertices=(x,y) 对、clippedUVs=(u,v) 对、
 * clippedTriangles=索引。返回 1 成功；0 表示完全被裁掉或扩容失败（不产出渲染项）。 */
static int _spR_emitClipped(spRContext *ctx, spRDrawItem *item, spine_skeleton_clipping clipper) {
    spine_array_float cv = spine_skeleton_clipping_get_clipped_vertices(clipper);
    spine_array_float cu = spine_skeleton_clipping_get_clipped_u_vs(clipper);
    spine_array_unsigned_short ct = spine_skeleton_clipping_get_clipped_triangles(clipper);
    int vlen = (int)spine_array_float_size(cv);   /* float 数（xy 对） */
    int tcount = (int)spine_array_unsigned_short_size(ct); /* unsigned short 数（索引） */
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
    memcpy(vbuf + item->vertsOffset, spine_array_float_buffer(cv), sizeof(float) * (size_t)vlen);
    memcpy(ubuf + item->vertsOffset, spine_array_float_buffer(cu), sizeof(float) * (size_t)vlen);
    memcpy(tbuf + item->trisOffset, spine_array_unsigned_short_buffer(ct), sizeof(unsigned short) * (size_t)tcount);
    item->vertsCount = vlen;
    item->trianglesCount = tcount;
    return 1;
}

/* 返回最近一次 spR_collectDrawItems 写入的 mesh 公共缓冲及其用量。
 * 缓冲随下一次收集复用/失效，调用方不得长期持有。 */
SP_R_API void spR_getMeshBufs(void *vctx, float **verts, float **uvs,
                              unsigned short **tris, int *vertsCount,
                              int *uvsCount, int *trisCount) {
    spRContext *ctx = (spRContext *)vctx;
    if (verts) *verts = ctx ? ctx->meshVertsBuf : NULL;
    if (uvs) *uvs = ctx ? ctx->meshUVsBuf : NULL;
    if (tris) *tris = ctx ? ctx->meshTrisBuf : NULL;
    if (vertsCount) *vertsCount = ctx ? ctx->meshVertsUsed : 0;
    if (uvsCount) *uvsCount = ctx ? ctx->meshUVsUsed : 0;
    if (trisCount) *trisCount = ctx ? ctx->meshTrisUsed : 0;
}

/* 按名查 slot 下标，未找到返回 -1（4.3 无 findSlotIndex，遍历 slots 数组）。 */
static int _spR_findSlotIndex(const spRContext *ctx, const char *name) {
    spine_array_slot arr;
    spine_slot *slots;
    size_t n, i;
    if (!ctx || !ctx->skeleton || !name) return -1;
    arr = spine_skeleton_get_slots(ctx->skeleton);
    n = spine_array_slot_size(arr);
    slots = spine_array_slot_buffer(arr);
    for (i = 0; i < n; i++) {
        const char *sn = spine_slot_data_get_name(spine_slot_get_data(slots[i]));
        if (sn && strcmp(sn, name) == 0) return (int)i;
    }
    return -1;
}

/* 取 slot 当前附件（渲染路径读 appliedPose）。 */
static spine_attachment _spR_slotAttachment(const spRContext *ctx, int slotIndex) {
    spine_array_slot arr;
    spine_slot *slots;
    size_t n;
    if (!ctx || !ctx->skeleton || slotIndex < 0) return NULL;
    arr = spine_skeleton_get_slots(ctx->skeleton);
    n = spine_array_slot_size(arr);
    if ((size_t)slotIndex >= n) return NULL;
    slots = spine_array_slot_buffer(arr);
    return spine_slot_pose_get_attachment(spine_slot_get_applied_pose(slots[slotIndex]));
}

/* 清理与当前附件不匹配的 deform 残留（与 spine_renpy.c 的 _spR_clearStaleDeform 语义一致）。
 * 4.3 pose 架构：deform 存于 SlotPose（spine_slot_pose_get_deform），渲染读 appliedPose。
 * 动画切换后旧 deform 时间线写入的 deform 长度若小于当前 mesh 需要的顶点数
 * （worldVerticesLength），VertexAttachment::computeWorldVertices 的 weighted 分支会
 * 越界读 deformArray[f]，产生巨大坐标/崩溃。规则：deform 长度必须等于当前 mesh 的
 * worldVerticesLength 才算有效；附件不是 mesh 时 deform 一律无效。
 * pose 与 appliedPose 都可能持有残留，两者都清理。 */
static void _spR_clearStaleDeform(spRContext *ctx) {
    spine_array_slot arr;
    spine_slot *slots;
    size_t n, i;
    if (!ctx || !ctx->skeleton) return;
    arr = spine_skeleton_get_slots(ctx->skeleton);
    if (!arr) return;
    n = spine_array_slot_size(arr);
    slots = spine_array_slot_buffer(arr);
    for (i = 0; i < n; i++) {
        spine_slot slot = slots[i];
        spine_slot_pose poses[2];
        int p;
        if (!slot) continue;
        poses[0] = spine_slot_get_pose(slot);          /* 动画写入的 unconstrained pose */
        poses[1] = spine_slot_get_applied_pose(slot);  /* 渲染读取的 applied pose */
        for (p = 0; p < 2; p++) {
            spine_array_float deform;
            spine_attachment att;
            if (spine_array_float_size(spine_slot_pose_get_deform(poses[p])) == 0) continue;
            att = spine_slot_pose_get_attachment(poses[p]);
            if (!att) { spine_array_float_clear(spine_slot_pose_get_deform(poses[p])); continue; }
            if (spine_rtti_is_exactly(spine_attachment_get_rtti(att), spine_mesh_attachment_rtti())) {
                spine_vertex_attachment va = spine_attachment_cast_to_vertex_attachment(att);
                deform = spine_slot_pose_get_deform(poses[p]);
                /* 加权 mesh 的 deform 长度 = vertices.size()/3*2（>= worldVerticesLength），
                 * 非加权恰好相等；只有不足时才是残留，清 0 防越界。 */
                if (spine_array_float_size(deform) < spine_vertex_attachment_get_world_vertices_length(va))
                    spine_array_float_clear(deform);
            } else {
                spine_array_float_clear(spine_slot_pose_get_deform(poses[p]));
            }
        }
    }
}

/* ---------------- ABI: 生命周期 ---------------- */

SP_R_API const char *spR_version(void) {
    return "4.3";
}

/* ---------------- 共享数据层：atlas + skeletonData 解析 ---------------- */

/* 解析 atlas + skeletonData（文件版）。成功返回 1，失败写 data->error 返回 0。
 * 4.3 atlas 解析只依赖文本（texture loader 把页索引当 texture），不读图片文件。 */
static int _spR_parseDataFile(spRData *data, const char *jsonPath, const char *atlasPath, float scale) {
    char *atlasText = NULL;
    spine_atlas_result ar = NULL;

    atlasText = read_file_text(atlasPath);
    if (!atlasText) {
        snprintf(data->error, sizeof(data->error), "cannot read atlas: %s", atlasPath);
        return 0;
    }
    ar = spine_atlas_load(atlasText);
    free(atlasText);
    if (!ar || !spine_atlas_result_get_atlas(ar)) {
        const char *e = ar ? spine_atlas_result_get_error(ar) : NULL;
        snprintf(data->error, sizeof(data->error), "cannot load atlas: %s (%s)",
                 atlasPath, e ? e : "unknown error");
        if (ar) spine_atlas_result_dispose(ar);
        return 0;
    }
    data->atlas = spine_atlas_result_get_atlas(ar);
    spine_atlas_result_dispose(ar); /* 只释放 result 壳，atlas 本体归 data 管理 */

    if (is_skeleton_json_file(jsonPath)) {
        spine_skeleton_json json = spine_skeleton_json_create(data->atlas);
        spine_skeleton_json_set_scale(json, scale);
        data->skeletonData = spine_skeleton_json_read_skeleton_data_file(json, jsonPath);
        if (!data->skeletonData) {
            const char *e = spine_skeleton_json_get_error(json);
            snprintf(data->error, sizeof(data->error), "%s", e ? e : "cannot read skeleton json");
            spine_skeleton_json_dispose(json);
            return 0;
        }
        spine_skeleton_json_dispose(json);
    } else {
        spine_skeleton_binary binary = spine_skeleton_binary_create(data->atlas);
        spine_skeleton_binary_set_scale(binary, scale);
        data->skeletonData = spine_skeleton_binary_read_skeleton_data_file(binary, jsonPath);
        if (!data->skeletonData) {
            const char *e = spine_skeleton_binary_get_error(binary);
            snprintf(data->error, sizeof(data->error), "%s", e ? e : "cannot read skeleton skel");
            spine_skeleton_binary_dispose(binary);
            return 0;
        }
        spine_skeleton_binary_dispose(binary);
    }
    return 1;
}

/* 解析 atlas + skeletonData（内存版）。与 _spR_parseDataFile 同规则；
 * atlas 文本必须 \0 结尾（内部走 strlen）。
 * 注意：atlasData/skeletonData 必须保证在调用期间有效（解析完成后不再引用）。 */
static int _spR_parseDataMem(spRData *data, const unsigned char *skeletonData, int skeletonLen,
                             const unsigned char *atlasData, int atlasLen, float scale) {
    spine_atlas_result ar = NULL;

    ar = spine_atlas_load((const char *)atlasData);
    if (!ar || !spine_atlas_result_get_atlas(ar)) {
        const char *e = ar ? spine_atlas_result_get_error(ar) : NULL;
        snprintf(data->error, sizeof(data->error), "cannot load atlas from memory (%s)",
                 e ? e : "unknown error");
        if (ar) spine_atlas_result_dispose(ar);
        return 0;
    }
    data->atlas = spine_atlas_result_get_atlas(ar);
    spine_atlas_result_dispose(ar); /* 只释放 result 壳，atlas 本体归 data 管理 */

    if (is_skeleton_json_data(skeletonData, skeletonLen)) {
        spine_skeleton_json json = spine_skeleton_json_create(data->atlas);
        spine_skeleton_json_set_scale(json, scale);
        data->skeletonData = spine_skeleton_json_read_skeleton_data(json, (const char *)skeletonData);
        if (!data->skeletonData) {
            const char *e = spine_skeleton_json_get_error(json);
            snprintf(data->error, sizeof(data->error), "%s", e ? e : "cannot read skeleton json from memory");
            spine_skeleton_json_dispose(json);
            return 0;
        }
        spine_skeleton_json_dispose(json);
    } else {
        spine_skeleton_binary binary = spine_skeleton_binary_create(data->atlas);
        spine_skeleton_binary_set_scale(binary, scale);
        data->skeletonData = spine_skeleton_binary_read_skeleton_data(binary, skeletonData, skeletonLen);
        if (!data->skeletonData) {
            const char *e = spine_skeleton_binary_get_error(binary);
            snprintf(data->error, sizeof(data->error), "%s", e ? e : "cannot read skeleton skel from memory");
            spine_skeleton_binary_dispose(binary);
            return 0;
        }
        spine_skeleton_binary_dispose(binary);
    }
    return 1;
}

/* 运行时初始化：基于已解析的 skeletonData 创建 state/skeleton/裁剪器。
 * 事件转发：4.3 listener 带 user_data 参数，直接把 ctx 传进去。 */
static void _spR_createRuntime(spRContext *ctx) {
    ctx->stateData = spine_animation_state_data_create(ctx->skeletonData);
    ctx->state = spine_animation_state_create(ctx->stateData);
    ctx->skeleton = spine_skeleton_create(ctx->skeletonData);
    spine_skeleton_setup_pose(ctx->skeleton);
    SP_R_UPDATE_WORLD(ctx->skeleton);

    spine_animation_state_set_listener(ctx->state, _spR_forwardListener, ctx);
    /* 裁剪器：渲染循环（collectDrawItems/buildMesh）复用一个实例 */
    ctx->clipper = spine_skeleton_clipping_create();
}

SP_R_API void *spR_create(const char *jsonPath, const char *atlasPath, float scale) {
    spRContext *ctx = (spRContext *)calloc(1, sizeof(spRContext));
    spRData data;

    if (!ctx) return NULL;

    /* world Y points down, matches Ren'Py screen coordinates */
    spine_bone_set_y_down(true);

    /* 旧路径：解析失败返回带 error 的 ctx（与历史行为一致）。
     * 失败时 data 里可能已加载 atlas（skeletonData 失败），需手动释放。 */
    memset(&data, 0, sizeof(data));
    if (!_spR_parseDataFile(&data, jsonPath, atlasPath, scale)) {
        snprintf(ctx->error, sizeof(ctx->error), "%s", data.error);
        if (data.skeletonData) spine_skeleton_data_dispose(data.skeletonData);
        if (data.atlas) spine_atlas_dispose(data.atlas);
        return ctx;
    }
    ctx->ownsData = 1;
    ctx->atlas = data.atlas;
    ctx->skeletonData = data.skeletonData;
    _spR_createRuntime(ctx);
    return ctx;
}

/* 内存版 spR_create：json/skel 与 atlas 全部由调用方读好字节直接传入，
 * C 层不再访问文件系统（安卓 asset 虚拟文件系统无法 fopen，需 renpy 读取）。
 * 注意：atlasData/skeletonData 必须保证在调用期间有效（解析完成后不再引用）。 */
SP_R_API void *spR_createMem(const unsigned char *skeletonData, int skeletonLen,
                             const unsigned char *atlasData, int atlasLen, float scale) {
    spRContext *ctx = (spRContext *)calloc(1, sizeof(spRContext));
    spRData data;

    if (!ctx) return NULL;

    /* world Y points down, matches Ren'Py screen coordinates */
    spine_bone_set_y_down(true);

    /* 旧路径：解析失败返回带 error 的 ctx（与历史行为一致）。
     * 失败时 data 里可能已加载 atlas（skeletonData 失败），需手动释放。 */
    memset(&data, 0, sizeof(data));
    if (!_spR_parseDataMem(&data, skeletonData, skeletonLen, atlasData, atlasLen, scale)) {
        snprintf(ctx->error, sizeof(ctx->error), "%s", data.error);
        if (data.skeletonData) spine_skeleton_data_dispose(data.skeletonData);
        if (data.atlas) spine_atlas_dispose(data.atlas);
        return ctx;
    }
    ctx->ownsData = 1;
    ctx->atlas = data.atlas;
    ctx->skeletonData = data.skeletonData;
    _spR_createRuntime(ctx);
    return ctx;
}

/* 共享数据层导出：解析一次，供多个运行时共享（方案 B）。 */

SP_R_API void *spR_loadData(const char *jsonPath, const char *atlasPath, float scale) {
    spRData *data = (spRData *)calloc(1, sizeof(spRData));
    if (!data) return NULL;
    /* world Y points down, matches Ren'Py screen coordinates */
    spine_bone_set_y_down(true);
    if (!_spR_parseDataFile(data, jsonPath, atlasPath, scale)) return data; /* error 已写 */
    return data;
}

SP_R_API void *spR_loadDataMem(const unsigned char *skeletonData, int skeletonLen,
                               const unsigned char *atlasData, int atlasLen, float scale) {
    spRData *data = (spRData *)calloc(1, sizeof(spRData));
    if (!data) return NULL;
    /* world Y points down, matches Ren'Py screen coordinates */
    spine_bone_set_y_down(true);
    if (!_spR_parseDataMem(data, skeletonData, skeletonLen, atlasData, atlasLen, scale)) return data; /* error 已写 */
    return data;
}

SP_R_API const char *spR_dataError(void *vdata) {
    spRData *data = (spRData *)vdata;
    return data ? data->error : "";
}

/* 从共享 data 创建运行时 ctx（data 不归 ctx 所有，spR_dispose 不释放它）。 */
SP_R_API void *spR_createSkeleton(void *vdata) {
    spRData *data = (spRData *)vdata;
    spRContext *ctx = (spRContext *)calloc(1, sizeof(spRContext));
    if (!ctx) return NULL;
    if (!data || !data->atlas || !data->skeletonData) {
        snprintf(ctx->error, sizeof(ctx->error), "spine data not loaded or invalid");
        return ctx;
    }
    /* world Y points down, matches Ren'Py screen coordinates */
    spine_bone_set_y_down(true);
    ctx->ownsData = 0;
    ctx->atlas = data->atlas;
    ctx->skeletonData = data->skeletonData;
    _spR_createRuntime(ctx);
    return ctx;
}

SP_R_API void spR_disposeData(void *vdata) {
    spRData *data = (spRData *)vdata;
    if (!data) return;
    if (data->skeletonData) spine_skeleton_data_dispose(data->skeletonData);
    if (data->atlas) spine_atlas_dispose(data->atlas);
    free(data);
}

SP_R_API const char *spR_error(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    return ctx ? ctx->error : "";
}

SP_R_API void spR_dispose(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    int i;
    if (!ctx) return;
    for (i = 0; i < ctx->combinedSkinsCount; i++)
        if (ctx->combinedSkins[i]) spine_skin_dispose(ctx->combinedSkins[i]);
    if (ctx->combinedSkins) free(ctx->combinedSkins);
    if (ctx->tmpVerts) free(ctx->tmpVerts);
    if (ctx->meshVertsBuf) free(ctx->meshVertsBuf);
    if (ctx->meshUVsBuf) free(ctx->meshUVsBuf);
    if (ctx->meshTrisBuf) free(ctx->meshTrisBuf);
    if (ctx->clipper) spine_skeleton_clipping_dispose(ctx->clipper);
    if (ctx->state) spine_animation_state_dispose(ctx->state);
    if (ctx->stateData) spine_animation_state_data_dispose(ctx->stateData);
    if (ctx->skeleton) spine_skeleton_dispose(ctx->skeleton);
    /* atlas/skeletonData 属于共享数据层：只有 ownsData（spR_create 旧路径）
     * 才在此连带释放；spR_createSkeleton 路径由 spR_disposeData 统一管理。 */
    if (ctx->ownsData) {
        if (ctx->skeletonData) spine_skeleton_data_dispose(ctx->skeletonData);
        if (ctx->atlas) spine_atlas_dispose(ctx->atlas);
    }
    free(ctx);
}

/* advance one frame: update animation state -> apply -> recompute world */
SP_R_API void spR_update(void *vctx, float delta) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return;
    spine_animation_state_update(ctx->state, delta);
    spine_animation_state_apply(ctx->state, ctx->skeleton);
    _spR_clearStaleDeform(ctx);
    SP_R_UPDATE_WORLD(ctx->skeleton);
}

/* ---------------- animation events (listener) ---------------- */

/* spine-c 4.3 listener：(state, type, entry, event, user_data)。
 * 与 spine_renpy.c 的 ABI 一致地转发给 Python，事件枚举做 3<->4 交换
 * （4.3: DISPOSE=3 COMPLETE=4；ABI: COMPLETE=3 DISPOSE=4）。 */
static void _spR_forwardListener(spine_animation_state state, spine_event_type type,
                                 spine_track_entry entry, spine_event event, void *user_data) {
    spRContext *ctx = (spRContext *)user_data;
    spine_animation anim;
    const char *animName = NULL, *eventName = NULL;
    int t;
    if (!ctx || !ctx->userListener) return;
    t = (int)type;
    if (t == 3) t = 4;
    else if (t == 4) t = 3;
    anim = entry ? spine_track_entry_get_animation(entry) : NULL;
    if (anim) animName = spine_animation_get_name(anim);
    if (event && spine_event_get_data(event)) eventName = spine_event_data_get_name(spine_event_get_data(event));
    ctx->userListener(ctx->listenerUserData,
                      t,
                      animName,
                      eventName,
                      event ? spine_event_get_time(event) : 0.0f,
                      event ? spine_event_get_int(event) : 0,
                      event ? spine_event_get_float(event) : 0.0f,
                      (event && spine_event_get_string(event)) ? spine_event_get_string(event) : NULL);
    (void)state;
}

/* 注册事件回调。cb 为 NULL 时停止转发。userData 原样回传给回调。 */
SP_R_API void spR_setListener(void *vctx, spRListenerCallback cb, void *userData) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx) return;
    ctx->userListener = cb;
    ctx->listenerUserData = userData;
}

/* 设置全局动画速率（对应 spine-unity 的 AnimationState.TimeScale）。 */
SP_R_API void spR_setTimeScale(void *vctx, float scale) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return;
    spine_animation_state_set_time_scale(ctx->state, scale);
}

/* 设置单轨道动画速率（对应 spine-unity 的 TrackEntry.TimeScale）。
 * 该轨道当前无动画时返回 0。 */
SP_R_API int spR_setTrackTimeScale(void *vctx, int track, float scale) {
    spRContext *ctx = (spRContext *)vctx;
    spine_track_entry entry;
    if (!ctx || !ctx->state) return 0;
    entry = spine_animation_state_get_track(ctx->state, (size_t)track);
    if (!entry) return 0;
    spine_track_entry_set_time_scale(entry, scale);
    return 1;
}

/* ---------------- animation control ---------------- */

/* 在指定轨道上播放动画（track 0 起，多轨可并行叠加）。 */
SP_R_API int spR_setAnimation(void *vctx, int track, const char *name, int loop) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return 0;
    if (!spine_skeleton_data_find_animation(ctx->skeletonData, name)) return 0;
    if (!spine_animation_state_set_animation_1(ctx->state, (size_t)track, name, loop != 0)) return 0;
    spine_animation_state_apply(ctx->state, ctx->skeleton);
    _spR_clearStaleDeform(ctx);
    SP_R_UPDATE_WORLD(ctx->skeleton);
    return 1;
}

/* 在指定轨道上把动画加入播放队列（当前动画播完后按 delay 延迟接续）。 */
SP_R_API int spR_addAnimation(void *vctx, int track, const char *name, int loop, float delay) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return 0;
    if (!spine_skeleton_data_find_animation(ctx->skeletonData, name)) return 0;
    if (!spine_animation_state_add_animation_1(ctx->state, (size_t)track, name, loop != 0, delay)) return 0;
    return 1;
}

/* 查询指定轨道当前动画名；轨道无动画返回 NULL。 */
SP_R_API const char *spR_getCurrentAnimationName(void *vctx, int track) {
    spRContext *ctx = (spRContext *)vctx;
    spine_track_entry entry;
    spine_animation anim;
    if (!ctx || !ctx->state) return NULL;
    entry = spine_animation_state_get_track(ctx->state, (size_t)track);
    if (!entry) return NULL;
    anim = spine_track_entry_get_animation(entry);
    if (!anim) return NULL;
    return spine_animation_get_name(anim);
}

/* 查询指定轨道当前动画是否循环；轨道无动画返回 -1。
 * 热重载存档恢复时需区分 loop 与一次性动画。 */
SP_R_API int spR_getCurrentLoop(void *vctx, int track) {
    spRContext *ctx = (spRContext *)vctx;
    spine_track_entry entry;
    if (!ctx || !ctx->state) return -1;
    entry = spine_animation_state_get_track(ctx->state, (size_t)track);
    if (!entry) return -1;
    return spine_track_entry_get_loop(entry) ? 1 : 0;
}

/* 查询指定轨道播放队列中第 idx 项（0 = 当前动画播完后的下一项）的
 * name/loop/delay。name 拷贝到 buf（与 spR_getAnimationName 同风格）。
 * 成功返回 1；无该队列项返回 0。loop/delay 为可空输出参数。 */
SP_R_API int spR_getQueuedAnimation(void *vctx, int track, int idx,
                                    char *buf, int buflen, int *loop, float *delay) {
    spRContext *ctx = (spRContext *)vctx;
    spine_track_entry entry;
    spine_animation anim;
    const char *name;
    if (!ctx || !ctx->state || !buf || buflen <= 0) return 0;
    entry = spine_animation_state_get_track(ctx->state, (size_t)track);
    if (!entry) return 0;
    /* 队列从当前 entry 的 next 开始：先跳到队首，再沿链表走 idx 步 */
    entry = spine_track_entry_get_next(entry);
    while (idx-- > 0 && entry) entry = spine_track_entry_get_next(entry);
    if (!entry) return 0;
    anim = spine_track_entry_get_animation(entry);
    if (!anim) return 0;
    name = spine_animation_get_name(anim);
    if (!name) return 0;
    strncpy(buf, name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    if (loop) *loop = spine_track_entry_get_loop(entry) ? 1 : 0;
    if (delay) *delay = spine_track_entry_get_delay(entry);
    return 1;
}

/* 全局动画速率（AnimationState.timeScale），热重载存档恢复用。 */
SP_R_API float spR_getTimeScale(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return 1.0f;
    return spine_animation_state_get_time_scale(ctx->state);
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
    spRContext *ctx = (spRContext *)vctx;
    spine_track_entry entry;
    spine_animation anim;
    const char *name;
    if (!ctx || !ctx->state) return 0;
    entry = spine_animation_state_get_track(ctx->state, (size_t)track);
    if (!entry) return 0;
    while (pos-- > 0 && entry) entry = spine_track_entry_get_next(entry);
    if (!entry) return 0;
    anim = spine_track_entry_get_animation(entry);
    name = anim ? spine_animation_get_name(anim) : NULL;
    if (buf && buflen > 0) {
        if (name) strncpy(buf, name, (size_t)(buflen - 1));
        buf[buflen - 1] = '\0';
    }
    if (is_empty) *is_empty = anim ? 0 : 1;
    if (loop) *loop = spine_track_entry_get_loop(entry) ? 1 : 0;
    if (time_scale) *time_scale = spine_track_entry_get_time_scale(entry);
    if (mix_duration) *mix_duration = spine_track_entry_get_mix_duration(entry);
    if (mix_time) *mix_time = spine_track_entry_get_mix_time(entry);
    if (track_time) *track_time = spine_track_entry_get_track_time(entry);
    if (delay) *delay = spine_track_entry_get_delay(entry);
    return 1;
}

/* 设置轨道第 pos 个 entry 的速率（热重载恢复用）。返回 1 成功。 */
SP_R_API int spR_setTrackEntryTimeScale(void *vctx, int track, int pos, float scale) {
    spRContext *ctx = (spRContext *)vctx;
    spine_track_entry entry;
    if (!ctx || !ctx->state) return 0;
    entry = spine_animation_state_get_track(ctx->state, (size_t)track);
    if (!entry) return 0;
    while (pos-- > 0 && entry) entry = spine_track_entry_get_next(entry);
    if (!entry) return 0;
    spine_track_entry_set_time_scale(entry, scale);
    return 1;
}

/* 设置轨道第 pos 个 entry 的播放进度（trackTime/mixTime，热重载恢复用）。
 * 返回 1 成功。 */
SP_R_API int spR_setTrackEntryTime(void *vctx, int track, int pos,
                                   float track_time, float mix_time) {
    spRContext *ctx = (spRContext *)vctx;
    spine_track_entry entry;
    if (!ctx || !ctx->state) return 0;
    entry = spine_animation_state_get_track(ctx->state, (size_t)track);
    if (!entry) return 0;
    while (pos-- > 0 && entry) entry = spine_track_entry_get_next(entry);
    if (!entry) return 0;
    spine_track_entry_set_track_time(entry, track_time);
    spine_track_entry_set_mix_time(entry, mix_time);
    return 1;
}

/* 设置轨道第 pos 个 entry 的队列延迟（热重载恢复用，精确还原 addAnimation 的
 * delay 参数；addAnimation 内部会改写 entry->delay，直接重放会有偏差）。
 * 返回 1 成功。 */
SP_R_API int spR_setTrackEntryDelay(void *vctx, int track, int pos, float delay) {
    spRContext *ctx = (spRContext *)vctx;
    spine_track_entry entry;
    if (!ctx || !ctx->state) return 0;
    entry = spine_animation_state_get_track(ctx->state, (size_t)track);
    if (!entry) return 0;
    while (pos-- > 0 && entry) entry = spine_track_entry_get_next(entry);
    if (!entry) return 0;
    spine_track_entry_set_delay(entry, delay);
    return 1;
}

/* 查询 slot 运行时状态：当前 attachment 名与颜色。与 setup 附件/颜色一致时
 * 返回 0（无需恢复）；否则返回 1（buf/颜色带出实际值）。 */
SP_R_API int spR_getSlotState(void *vctx, int slotIndex,
                              char *buf, int buflen,
                              float *r, float *g, float *b, float *a) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_slot_data darr;
    spine_array_slot sarr;
    spine_slot *slots;
    spine_slot slot;
    spine_slot_pose pose, setup;
    spine_attachment att, setup_att;
    spine_color c, sc;
    const char *name;
    int dirty = 0;
    size_t count;
    if (!ctx || !ctx->skeleton || !ctx->skeletonData) return 0;
    darr = spine_skeleton_data_get_slots(ctx->skeletonData);
    if (!darr) return 0;
    count = spine_array_slot_data_size(darr);
    if (slotIndex < 0 || (size_t)slotIndex >= count) return 0;
    sarr = spine_skeleton_get_slots(ctx->skeleton);
    if (!sarr) return 0;
    slots = spine_array_slot_buffer(sarr);
    if (!slots) return 0;
    slot = slots[slotIndex];
    pose = spine_slot_get_pose(slot);
    setup = spine_slot_data_get_setup_pose(spine_slot_get_data(slot));
    att = spine_slot_pose_get_attachment(pose);
    setup_att = spine_slot_pose_get_attachment(setup);
    c = spine_slot_pose_get_color(pose);
    sc = spine_slot_pose_get_color(setup);
    name = att ? spine_attachment_get_name(att) : NULL;
    if (buf && buflen > 0) {
        if (name) strncpy(buf, name, (size_t)(buflen - 1));
        buf[buflen - 1] = '\0';
    }
    if (r) *r = spine_color_get_r(c);
    if (g) *g = spine_color_get_g(c);
    if (b) *b = spine_color_get_b(c);
    if (a) *a = spine_color_get_a(c);
    /* 与 setup 附件/颜色比较，判定是否需要恢复 */
    if (att != setup_att) dirty = 1;
    if (spine_color_get_r(c) != spine_color_get_r(sc) ||
        spine_color_get_g(c) != spine_color_get_g(sc) ||
        spine_color_get_b(c) != spine_color_get_b(sc) ||
        spine_color_get_a(c) != spine_color_get_a(sc)) dirty = 1;
    return dirty;
}

/* 骨架颜色（热重载存档恢复用）。 */
SP_R_API void spR_getSkeletonColor(void *vctx,
                                   float *r, float *g, float *b, float *a) {
    spRContext *ctx = (spRContext *)vctx;
    spine_color c;
    if (!ctx || !ctx->skeleton) return;
    c = spine_skeleton_get_color(ctx->skeleton);
    if (r) *r = spine_color_get_r(c);
    if (g) *g = spine_color_get_g(c);
    if (b) *b = spine_color_get_b(c);
    if (a) *a = spine_color_get_a(c);
}

/* 查询动画时长（秒）；动画不存在返回 -1。 */
SP_R_API float spR_getAnimationDuration(void *vctx, const char *name) {
    spRContext *ctx = (spRContext *)vctx;
    spine_animation a;
    if (!ctx || !ctx->skeletonData || !name) return -1.0f;
    a = spine_skeleton_data_find_animation(ctx->skeletonData, name);
    return a ? spine_animation_get_duration(a) : -1.0f;
}

/* 骨架定义的全部动画数（按定义顺序）。供 Python 全局并集采样固定视口
 * （SpineViewer 式固定 viewport：遍历所有动画求包围盒并集，创建后不重算）。 */
SP_R_API int spR_getAnimationCount(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_animation arr;
    if (!ctx || !ctx->skeletonData) return 0;
    arr = spine_skeleton_data_get_animations(ctx->skeletonData);
    if (!arr) return 0;
    return (int)spine_array_animation_size(arr);
}

/* 拷贝第 index 个动画名到 buf（与 spR_getPageName 同风格）。成功返回 1。 */
SP_R_API int spR_getAnimationName(void *vctx, int index, char *buf, int buflen) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_animation arr;
    spine_animation *items;
    const char *name;
    size_t n;
    if (!ctx || !ctx->skeletonData || !buf || buflen <= 0) return 0;
    arr = spine_skeleton_data_get_animations(ctx->skeletonData);
    if (!arr) return 0;
    n = spine_array_animation_size(arr);
    if (index < 0 || (size_t)index >= n) return 0;
    items = spine_array_animation_buffer(arr);
    if (!items) return 0;
    name = spine_animation_get_name(items[index]);
    if (!name) return 0;
    strncpy(buf, name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    return 1;
}

/* 返回 slot 总数（skeletonData 的 setup pose 顺序）。 */
SP_R_API int spR_getSlotCount(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_slot_data arr;
    if (!ctx || !ctx->skeletonData) return 0;
    arr = spine_skeleton_data_get_slots(ctx->skeletonData);
    if (!arr) return 0;
    return (int)spine_array_slot_data_size(arr);
}

/* 拷贝第 index 个 slot 名到 buf（与 spR_getAnimationName 同风格）。成功返回 1。 */
SP_R_API int spR_getSlotName(void *vctx, int index, char *buf, int buflen) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_slot_data arr;
    spine_slot_data *items;
    const char *name;
    size_t n;
    if (!ctx || !ctx->skeletonData || !buf || buflen <= 0) return 0;
    arr = spine_skeleton_data_get_slots(ctx->skeletonData);
    if (!arr) return 0;
    n = spine_array_slot_data_size(arr);
    if (index < 0 || (size_t)index >= n) return 0;
    items = spine_array_slot_data_buffer(arr);
    if (!items || !items[index]) return 0;
    name = spine_slot_data_get_name(items[index]);
    if (!name) return 0;
    strncpy(buf, name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    return 1;
}

/* 空动画：让指定轨道在 mixDuration 内淡出到绑定姿势。 */
SP_R_API void spR_setEmptyAnimation(void *vctx, int track, float mixDuration) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return;
    spine_animation_state_set_empty_animation(ctx->state, (size_t)track, mixDuration);
    spine_animation_state_apply(ctx->state, ctx->skeleton);
    _spR_clearStaleDeform(ctx);
    SP_R_UPDATE_WORLD(ctx->skeleton);
}

/* 对应 spine-unity 的 AddEmptyAnimation。 */
SP_R_API void spR_addEmptyAnimation(void *vctx, int track, float mixDuration, float delay) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return;
    spine_animation_state_add_empty_animation(ctx->state, (size_t)track, mixDuration, delay);
}

/* 立即清空指定轨道 / 全部轨道。 */
SP_R_API void spR_clearTrack(void *vctx, int track) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return;
    spine_animation_state_clear_track(ctx->state, (size_t)track);
}

SP_R_API void spR_clearTracks(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->state) return;
    spine_animation_state_clear_tracks(ctx->state);
}

/* ---------------- skin ---------------- */

SP_R_API int spR_setSkinByName(void *vctx, const char *name) {
    spRContext *ctx = (spRContext *)vctx;
    spine_skin skin = NULL;
    int i;
    if (!ctx || !ctx->skeleton || !name) return 0;
    skin = spine_skeleton_data_find_skin(ctx->skeletonData, name);
    if (!skin) {
        for (i = 0; i < ctx->combinedSkinsCount; i++) {
            if (ctx->combinedSkins[i] &&
                strcmp(spine_skin_get_name(ctx->combinedSkins[i]), name) == 0) {
                skin = ctx->combinedSkins[i];
                break;
            }
        }
        if (!skin) return 0;
    }
    spine_skeleton_set_skin_2(ctx->skeleton, skin);
    return 1;
}

/* 组合皮肤（mix-and-match）：把 skinNames[0..count-1] 合并为一个名为 combinedName
 * 的皮肤并应用到骨架。同名重新组合会重建（4.3 引用计数安全）。
 * 返回 1 成功；任一皮肤不存在返回 0（不改变当前皮肤）。 */
SP_R_API int spR_combineSkins(void *vctx, const char *combinedName,
                              const char **skinNames, int count) {
    spRContext *ctx = (spRContext *)vctx;
    spine_skin combined = NULL;
    int i, idx = -1;

    if (!ctx || !ctx->skeleton || !combinedName || !skinNames || count <= 0) return 0;

    for (i = 0; i < ctx->combinedSkinsCount; i++) {
        if (strcmp(spine_skin_get_name(ctx->combinedSkins[i]), combinedName) == 0) {
            idx = i;
            combined = ctx->combinedSkins[i];
            break;
        }
    }

    if (!combined) {
        combined = spine_skin_create(combinedName);
        if (!combined) return 0;
        ctx->combinedSkins = (spine_skin *)realloc(ctx->combinedSkins,
                                                   (size_t)(ctx->combinedSkinsCount + 1) * sizeof(spine_skin));
        if (!ctx->combinedSkins) { spine_skin_dispose(combined); return 0; }
        idx = ctx->combinedSkinsCount;
        ctx->combinedSkins[idx] = combined;
        ctx->combinedSkinsCount++;
    } else {
        /* 同名重建：4.3 引用计数安全，释放旧对象后重建 */
        spine_skin_dispose(combined);
        combined = spine_skin_create(combinedName);
        if (!combined) return 0;
        ctx->combinedSkins[idx] = combined;
    }

    for (i = 0; i < count; i++) {
        spine_skin src = spine_skeleton_data_find_skin(ctx->skeletonData, skinNames[i]);
        if (!src) return 0;
        spine_skin_add_skin(combined, src);
    }

    spine_skeleton_set_skin_2(ctx->skeleton, combined);
    spine_skeleton_setup_pose_slots(ctx->skeleton);
    return 1;
}

/* 皮肤存在性检查（Data.FindSkin；组合皮肤缓存也纳入检查）。 */
SP_R_API int spR_hasSkin(void *vctx, const char *name) {
    spRContext *ctx = (spRContext *)vctx;
    int i;
    if (!ctx || !ctx->skeletonData || !name) return 0;
    if (spine_skeleton_data_find_skin(ctx->skeletonData, name)) return 1;
    for (i = 0; i < ctx->combinedSkinsCount; i++)
        if (ctx->combinedSkins[i] &&
            strcmp(spine_skin_get_name(ctx->combinedSkins[i]), name) == 0)
            return 1;
    return 0;
}

/* ---------------- attachment / mix ---------------- */

/* pass NULL as attachmentName to hide the slot */
SP_R_API int spR_setAttachment(void *vctx, const char *slotName, const char *attachmentName) {
    spRContext *ctx = (spRContext *)vctx;
    int idx;
    spine_slot slot;
    spine_attachment att = NULL;
    if (!ctx || !ctx->skeleton || !slotName) return 0;
    idx = _spR_findSlotIndex(ctx, slotName);
    if (idx < 0) return 0;
    if (attachmentName && attachmentName[0]) {
        att = spine_skeleton_get_attachment_2(ctx->skeleton, idx, attachmentName);
        if (!att) return 0;
    }
    /* 渲染读 appliedPose，动画写 pose：两者都设，与旧 slot->attachment 语义一致 */
    slot = spine_skeleton_find_slot(ctx->skeleton, slotName);
    if (!slot) return 0;
    spine_slot_pose_set_attachment(spine_slot_get_pose(slot), att);
    spine_slot_pose_set_attachment(spine_slot_get_applied_pose(slot), att);
    return 1;
}

SP_R_API void spR_setMix(void *vctx, const char *from, const char *to, float duration) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->stateData) return;
    spine_animation_state_data_set_mix_1(ctx->stateData, from, to, duration);
}

SP_R_API void spR_setDefaultMix(void *vctx, float duration) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->stateData) return;
    spine_animation_state_data_set_default_mix(ctx->stateData, duration);
}

/* ---------------- slot / skeleton color & setup pose ---------------- */

/* 设置指定 slot 的 RGBA 颜色（0~1）。
 * 注意：动画含 color 关键帧时，apply 会覆盖手动设置（同 spine-unity）。 */
SP_R_API int spR_setSlotColor(void *vctx, const char *slotName,
                              float r, float g, float b, float a) {
    spRContext *ctx = (spRContext *)vctx;
    spine_slot slot;
    spine_color c;
    if (!ctx || !ctx->skeleton || !slotName) return 0;
    slot = spine_skeleton_find_slot(ctx->skeleton, slotName);
    if (!slot) return 0;
    c = spine_slot_pose_get_color(spine_slot_get_pose(slot));
    if (c) spine_color_set_1(c, r, g, b, a);
    c = spine_slot_pose_get_color(spine_slot_get_applied_pose(slot));
    if (c) spine_color_set_1(c, r, g, b, a);
    return 1;
}

/* 只改 slot 透明度（保留 RGB），对应 spine-unity 的 slot.A = x。 */
SP_R_API int spR_setSlotAlpha(void *vctx, const char *slotName, float a) {
    spRContext *ctx = (spRContext *)vctx;
    spine_slot slot;
    spine_color c;
    if (!ctx || !ctx->skeleton || !slotName) return 0;
    slot = spine_skeleton_find_slot(ctx->skeleton, slotName);
    if (!slot) return 0;
    c = spine_slot_pose_get_color(spine_slot_get_pose(slot));
    if (c) spine_color_set_a(c, a);
    c = spine_slot_pose_get_color(spine_slot_get_applied_pose(slot));
    if (c) spine_color_set_a(c, a);
    return 1;
}

/* 设置整个骨骼的 RGBA 染色/透明度（与 spine-unity 的 Skeleton.SetColor 对齐）。 */
SP_R_API int spR_setSkeletonColor(void *vctx, float r, float g, float b, float a) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->skeleton) return 0;
    spine_skeleton_set_color_2(ctx->skeleton, r, g, b, a);
    return 1;
}

/* 只改整体透明度（保留 RGB），常用于整体淡入淡出。 */
SP_R_API int spR_setSkeletonAlpha(void *vctx, float a) {
    spRContext *ctx = (spRContext *)vctx;
    spine_color c;
    if (!ctx || !ctx->skeleton) return 0;
    c = spine_skeleton_get_color(ctx->skeleton);
    if (c) spine_color_set_a(c, a);
    return 1;
}

/* 恢复骨骼/插槽到 setup pose。仅恢复姿势，不停止动画。 */
SP_R_API void spR_setToSetupPose(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->skeleton) return;
    spine_skeleton_setup_pose(ctx->skeleton);
    SP_R_UPDATE_WORLD(ctx->skeleton);
}

SP_R_API void spR_setSlotsToSetupPose(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->skeleton) return;
    spine_skeleton_setup_pose_slots(ctx->skeleton);
}

SP_R_API void spR_setBonesToSetupPose(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->skeleton) return;
    spine_skeleton_setup_pose_bones(ctx->skeleton);
    SP_R_UPDATE_WORLD(ctx->skeleton);
}

/* ---------------- slot 下标访问 & 查询 ---------------- */

SP_R_API int spR_findSlotIndex(void *vctx, const char *slotName) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx) return -1;
    return _spR_findSlotIndex(ctx, slotName);
}

/* 按下标设置 slot 附件（attachmentName 为 NULL/空 表示隐藏该 slot）。 */
SP_R_API int spR_setSlotAttachmentByIndex(void *vctx, int slotIndex, const char *attachmentName) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_slot arr;
    spine_slot *slots;
    size_t n;
    spine_attachment att = NULL;
    if (!ctx || !ctx->skeleton) return 0;
    arr = spine_skeleton_get_slots(ctx->skeleton);
    n = spine_array_slot_size(arr);
    if (slotIndex < 0 || (size_t)slotIndex >= n) return 0;
    slots = spine_array_slot_buffer(arr);
    if (attachmentName && attachmentName[0]) {
        att = spine_skeleton_get_attachment_2(ctx->skeleton, slotIndex, attachmentName);
        if (!att) return 0;
    }
    spine_slot_pose_set_attachment(spine_slot_get_pose(slots[slotIndex]), att);
    spine_slot_pose_set_attachment(spine_slot_get_applied_pose(slots[slotIndex]), att);
    return 1;
}

/* 按下标设置 slot 透明度（slot.A = x）。 */
SP_R_API int spR_setSlotAlphaByIndex(void *vctx, int slotIndex, float a) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_slot arr;
    spine_slot *slots;
    size_t n;
    spine_color c;
    if (!ctx || !ctx->skeleton) return 0;
    arr = spine_skeleton_get_slots(ctx->skeleton);
    n = spine_array_slot_size(arr);
    if (slotIndex < 0 || (size_t)slotIndex >= n) return 0;
    slots = spine_array_slot_buffer(arr);
    c = spine_slot_pose_get_color(spine_slot_get_pose(slots[slotIndex]));
    if (c) spine_color_set_a(c, a);
    c = spine_slot_pose_get_color(spine_slot_get_applied_pose(slots[slotIndex]));
    if (c) spine_color_set_a(c, a);
    return 1;
}

/* 单 slot 复位到 setup pose（slot.SetToSetupPose）。 */
SP_R_API int spR_setSlotToSetupPose(void *vctx, int slotIndex) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_slot arr;
    spine_slot *slots;
    size_t n;
    if (!ctx || !ctx->skeleton) return 0;
    arr = spine_skeleton_get_slots(ctx->skeleton);
    n = spine_array_slot_size(arr);
    if (slotIndex < 0 || (size_t)slotIndex >= n) return 0;
    slots = spine_array_slot_buffer(arr);
    spine_slot_setup_pose(slots[slotIndex]);
    return 1;
}

/* 读取 slot 当前附件名（无附件返回 0，成功返回 1 并写入 buf）。 */
SP_R_API int spR_getSlotAttachmentName(void *vctx, int slotIndex, char *buf, int buflen) {
    spRContext *ctx = (spRContext *)vctx;
    spine_attachment att;
    const char *name;
    if (!ctx || !ctx->skeleton || !buf || buflen <= 0) return 0;
    att = _spR_slotAttachment(ctx, slotIndex);
    if (!att) return 0;
    name = spine_attachment_get_name(att);
    if (!name) return 0;
    strncpy(buf, name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    return 1;
}

/* 读取 slot 的 setup 附件名（slot.Data.AttachmentName，无则返回 0）。 */
SP_R_API int spR_getSlotSetupAttachmentName(void *vctx, int slotIndex, char *buf, int buflen) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_slot arr;
    spine_slot *slots;
    size_t n;
    const char *name;
    if (!ctx || !ctx->skeleton || !buf || buflen <= 0) return 0;
    arr = spine_skeleton_get_slots(ctx->skeleton);
    n = spine_array_slot_size(arr);
    if (slotIndex < 0 || (size_t)slotIndex >= n) return 0;
    slots = spine_array_slot_buffer(arr);
    name = spine_slot_data_get_attachment_name(spine_slot_get_data(slots[slotIndex]));
    if (!name) return 0;
    strncpy(buf, name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    return 1;
}

/* ---------------- collect & render ---------------- */

/* collect visible region & mesh attachments in draw order. returns count.
 * 容量不足时返回 -所需数量（与 spR_buildMesh 扩容约定一致），调用方扩容后重试。
 * 裁剪附件（clipping）不产出渲染项，而是驱动 clipper：
 * clip_start 在裁剪附件所在 slot 处启用裁剪，其后 region/mesh 附件经
 * clip_triangles_2 裁出实际可见部分（写公共缓冲），clip_end_1 在 endSlot 处结束。 */
SP_R_API int spR_collectDrawItems(void *vctx, spRDrawItem *items, int maxItems) {
    spRContext *ctx = (spRContext *)vctx;
    spine_skeleton skel;
    spine_draw_order dor;
    spine_array_slot doArr;
    spine_slot *slots;
    size_t n;
    int i, count = 0;
    if (!ctx || !ctx->skeleton) return 0;
    /* 上一帧若存在 endSlot 在 clipStart 之前的裁剪附件（如 end 指向前面 slot），
     * clip_end_1 永远匹配不上，clipper 残留开启；下一帧从第一项起就全被裁剪，
     * 只剩裁剪多边形内一小块。每帧收集前无条件闭合上一帧的残留裁剪。 */
    spine_skeleton_clipping_clip_end_2(ctx->clipper);
    skel = ctx->skeleton;
    dor = spine_skeleton_get_draw_order(skel);
    doArr = spine_draw_order_get_applied_pose(dor);
    n = spine_array_slot_size(doArr);
    slots = spine_array_slot_buffer(doArr);
    /* 第一遍：统计可见 region/mesh 附件总数（clipping 附件不产出渲染项），
     * 超限返回负值触发调用方扩容 */
    for (i = 0; i < (int)n; ++i) {
        spine_slot slot = slots[i];
        spine_attachment att = spine_slot_pose_get_attachment(spine_slot_get_applied_pose(slot));
        if (att && !spine_rtti_is_exactly(spine_attachment_get_rtti(att), spine_clipping_attachment_rtti()))
            ++count;
    }
    if (count > maxItems) return -count;
    /* 第二遍：按 drawOrder 写入渲染项 */
    count = 0;
    for (i = 0; i < (int)n; ++i) {
        spine_slot slot = slots[i];
        spine_slot_pose pose = spine_slot_get_applied_pose(slot);
        spine_attachment att = spine_slot_pose_get_attachment(pose);
        spine_rtti rtti;
        spine_color ac, sc, kc;
        spRDrawItem *item;

        if (!att) {
            spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
            continue;
        }

        rtti = spine_attachment_get_rtti(att);

        /* 裁剪附件：启用裁剪，不产出渲染项（嵌套裁剪返回 0，忽略即可） */
        if (spine_rtti_is_exactly(rtti, spine_clipping_attachment_rtti())) {
            spine_skeleton_clipping_clip_start(ctx->clipper, skel, slot,
                                               spine_attachment_cast_to_clipping_attachment(att));
            continue;
        }

        sc = spine_slot_pose_get_color(pose);
        kc = spine_skeleton_get_color(skel);
        item = &items[count];
        item->slotIndex = i;
        item->vertsCount = 0;
        item->trianglesCount = 0;

        if (spine_rtti_is_exactly(rtti, spine_region_attachment_rtti())) {
            spine_region_attachment region = spine_attachment_cast_to_region_attachment(att);
            spine_sequence seq = spine_region_attachment_get_sequence(region);
            int seqIndex = spine_sequence_resolve_index(seq, pose);
            spine_texture_region treg = spine_sequence_get_region(seq, seqIndex);
            spine_array_float offs, uvs;

            item->texIndex = (treg) ? (int)(intptr_t)spine_texture_region_get_renderer_object(treg) : -1;

            /* 4.3 需要显式传 region offsets（sequence 已按当前 sequenceIndex 算好） */
            offs = spine_region_attachment_get_offsets(region, pose);
            spine_region_attachment_compute_world_vertices_1(region, slot,
                                                             spine_array_float_buffer(offs),
                                                             item->vertices, 0, 2);
            uvs = spine_sequence_get_u_vs(seq, seqIndex);

            if (spine_skeleton_clipping_is_clipping(ctx->clipper)) {
                /* 裁剪中的 region 按 4 顶点 mesh 参与裁剪，结果写公共缓冲 */
                unsigned short rtris[6] = {0, 1, 2, 0, 2, 3};
                spine_skeleton_clipping_clip_triangles_2(ctx->clipper, item->vertices, rtris, 6,
                                                         spine_array_float_buffer(uvs), 2);
                if (!_spR_emitClipped(ctx, item, ctx->clipper)) {
                    spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
                    continue; /* 完全被裁掉，不产出渲染项 */
                }
            } else {
                memcpy(item->uvs, spine_array_float_buffer(uvs), sizeof(item->uvs));
            }

            ac = spine_region_attachment_get_color(region);
        } else if (spine_rtti_is_exactly(rtti, spine_mesh_attachment_rtti())) {
            spine_mesh_attachment mesh = spine_attachment_cast_to_mesh_attachment(att);
            spine_vertex_attachment va = spine_mesh_attachment_cast_to_vertex_attachment(mesh);
            spine_sequence seq = spine_mesh_attachment_get_sequence(mesh);
            int seqIndex = spine_sequence_resolve_index(seq, pose);
            spine_texture_region treg = spine_sequence_get_region(seq, seqIndex);
            spine_array_float uvs;
            spine_array_unsigned_short tris;
            size_t vlen, tcount;

            if (treg)
                item->texIndex = (int)(intptr_t)spine_texture_region_get_renderer_object(treg);
            else
                item->texIndex = -1;

            /* 顶点/uv/三角形（float 数 / ushort 数） */
            vlen = spine_vertex_attachment_get_world_vertices_length(va); /* float 数 */
            tcount = spine_array_unsigned_short_size(spine_mesh_attachment_get_triangles(mesh));
            uvs = spine_sequence_get_u_vs(seq, seqIndex);
            tris = spine_mesh_attachment_get_triangles(mesh);

            if (spine_skeleton_clipping_is_clipping(ctx->clipper)) {
                /* 裁剪中的 mesh：先算世界顶点，再裁剪，结果写公共缓冲 */
                float *wv = _spR_tmpVerts(ctx, (int)vlen);
                if (!wv) return 0;
                spine_vertex_attachment_compute_world_vertices_1(va, skel, slot, 0, vlen, wv, 0, 2);
                spine_skeleton_clipping_clip_triangles_2(ctx->clipper, wv,
                                                         spine_array_unsigned_short_buffer(tris), tcount,
                                                         spine_array_float_buffer(uvs), 2);
                if (!_spR_emitClipped(ctx, item, ctx->clipper)) {
                    spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
                    continue; /* 完全被裁掉，不产出渲染项 */
                }
            } else {
                /* 非裁剪：原逻辑（写入本帧公共缓冲，不截断） */
                float *vbuf, *ubuf;
                unsigned short *tbuf;

                /* 按累计用量扩容：一次扩容到位，避免中途 realloc 使已写入数据失效 */
                vbuf = _spR_meshVerts(ctx, ctx->meshVertsUsed + (int)vlen);
                ubuf = _spR_meshUVs(ctx, ctx->meshUVsUsed + (int)vlen);
                tbuf = _spR_meshTris(ctx, ctx->meshTrisUsed + (int)tcount);
                if (!vbuf || !ubuf || !tbuf) {
                    spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
                    continue; /* 扩容失败：跳过该附件 */
                }

                item->vertsOffset = ctx->meshVertsUsed; /* == ctx->meshUVsUsed */
                item->trisOffset = ctx->meshTrisUsed;
                ctx->meshVertsUsed += (int)vlen;
                ctx->meshUVsUsed += (int)vlen;
                ctx->meshTrisUsed += (int)tcount;

                spine_vertex_attachment_compute_world_vertices_1(va, skel, slot, 0, vlen,
                                                                 vbuf + item->vertsOffset, 0, 2);
                memcpy(ubuf + item->vertsOffset, spine_array_float_buffer(uvs), sizeof(float) * vlen);
                memcpy(tbuf + item->trisOffset, spine_array_unsigned_short_buffer(tris), sizeof(unsigned short) * tcount);
                item->vertsCount = (int)vlen;
                item->trianglesCount = (int)tcount;
            }

            ac = spine_mesh_attachment_get_color(mesh);
        } else {
            continue; /* 其余附件类型（bounding box/path/point）不渲染 */
        }

        item->color[0] = spine_color_get_r(ac) * spine_color_get_r(sc) * spine_color_get_r(kc);
        item->color[1] = spine_color_get_g(ac) * spine_color_get_g(sc) * spine_color_get_g(kc);
        item->color[2] = spine_color_get_b(ac) * spine_color_get_b(sc) * spine_color_get_b(kc);
        item->color[3] = spine_color_get_a(ac) * spine_color_get_a(sc) * spine_color_get_a(kc);
        count++;
        spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
    }
    return count;
}

SP_R_API int spR_getPageCount(void *vctx) {
    spRContext *ctx = (spRContext *)vctx;
    if (!ctx || !ctx->atlas) return 0;
    return (int)spine_array_atlas_page_size(spine_atlas_get_pages(ctx->atlas));
}

/* copy the image file name of the index-th page (relative to atlas dir). returns 1 on success. */
SP_R_API int spR_getPageName(void *vctx, int index, char *buf, int buflen) {
    spRContext *ctx = (spRContext *)vctx;
    spine_array_atlas_page pages;
    spine_atlas_page *arr;
    size_t n;
    const char *name;
    if (!ctx || !ctx->atlas || !buf || buflen <= 0) return 0;
    if (index < 0) return 0;
    pages = spine_atlas_get_pages(ctx->atlas);
    n = spine_array_atlas_page_size(pages);
    if ((size_t)index >= n) return 0;
    arr = spine_array_atlas_page_buffer(pages);
    name = spine_atlas_page_get_name(arr[index]);
    if (!name) return 0;
    strncpy(buf, name, (size_t)(buflen - 1));
    buf[buflen - 1] = '\0';
    return 1;
}

/* ---------------- 合并 mesh 构建（高频渲染路径） ---------------- */

/* 计算当前帧全部可见 region/mesh 附件的世界坐标包围盒。
 * 成功返回 1 并写出 (minX, minY, maxX, maxY)；无可见附件返回 0。 */
SP_R_API int spR_collectBounds(void *vctx, float *minX, float *minY, float *maxX, float *maxY) {
    spRContext *ctx = (spRContext *)vctx;
    spine_skeleton skel;
    spine_draw_order dor;
    spine_array_slot doArr;
    spine_slot *slots;
    size_t n;
    int i, found = 0;
    float mnx, mny, mxx, mxy;
    if (!ctx || !ctx->skeleton) return 0;
    skel = ctx->skeleton;
    mnx = mny = 1e30f;
    mxx = mxy = -1e30f;
    dor = spine_skeleton_get_draw_order(skel);
    doArr = spine_draw_order_get_applied_pose(dor);
    n = spine_array_slot_size(doArr);
    slots = spine_array_slot_buffer(doArr);
    for (i = 0; i < (int)n; ++i) {
        spine_slot slot = slots[i];
        spine_slot_pose pose = spine_slot_get_applied_pose(slot);
        spine_attachment att = spine_slot_pose_get_attachment(pose);
        spine_rtti rtti;
        int j;
        if (!att) continue;
        rtti = spine_attachment_get_rtti(att);
        if (spine_rtti_is_exactly(rtti, spine_region_attachment_rtti())) {
            spine_region_attachment region = spine_attachment_cast_to_region_attachment(att);
            spine_sequence seq = spine_region_attachment_get_sequence(region);
            int seqIndex = spine_sequence_resolve_index(seq, pose);
            spine_array_float offs = spine_region_attachment_get_offsets(region, pose);
            float verts[8];
            spine_region_attachment_compute_world_vertices_1(region, slot,
                                                             spine_array_float_buffer(offs),
                                                             verts, 0, 2);
            for (j = 0; j < 4; j++) {
                if (verts[j * 2] < mnx) mnx = verts[j * 2];
                if (verts[j * 2] > mxx) mxx = verts[j * 2];
                if (verts[j * 2 + 1] < mny) mny = verts[j * 2 + 1];
                if (verts[j * 2 + 1] > mxy) mxy = verts[j * 2 + 1];
            }
            found = 1;
        } else if (spine_rtti_is_exactly(rtti, spine_mesh_attachment_rtti())) {
            spine_mesh_attachment mesh = spine_attachment_cast_to_mesh_attachment(att);
            spine_vertex_attachment va = spine_mesh_attachment_cast_to_vertex_attachment(mesh);
            size_t vlen = spine_vertex_attachment_get_world_vertices_length(va); /* float 数 */
            float *wv = _spR_tmpVerts(ctx, (int)vlen);
            if (!wv) return 0;
            spine_vertex_attachment_compute_world_vertices_1(va, skel, slot, 0, vlen, wv, 0, 2);
            for (j = 0; j < (int)vlen; j += 2) {
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

/* 把所有可见附件合并进"单 Mesh2 数据缓冲"（一次 draw call 的顶点布局）。
 * 布局与 spine_renpy.c 完全一致：
 *   geo[vi*2]        = (世界x - minX) * zoom
 *   geo[vi*2+1]      = (世界y - minY) * zoom
 *   attrs[vi*6]      = (uv_u * 页宽 + 页偏移) / 合成图宽
 *   attrs[vi*6+1]    = uv_v * 页高 / 合成图高
 *   attrs[vi*6+2..5] = 附件 x slot x skeleton 乘积颜色 RGBA
 *   tris             : mesh 附件原样 + 顶点偏移；region 附件补四边形 [0,1,2, 0,2,3]
 *
 * 返回顶点数（>=0）；无可见附件返回 0；缓冲不足返回 -(所需顶点数)。
 * outTriangles 写出实际三角形数。 */
SP_R_API int spR_buildMeshEx(void *vctx, float minX, float minY, float zoom,
                             const float *atlasOffsets, const float *atlasPageW,
                             const float *atlasPageH, int pageCount,
                             float atlasW, float atlasH,
                             float *geo, float *attrs, unsigned short *tris,
                             int maxVerts, int maxTris, int *outTriangles,
                             int *outBlendModes, int *outSegStartTriangles,
                             int maxSegments, int *outSegmentCount) {
    spRContext *ctx = (spRContext *)vctx;
    spine_skeleton skel;
    spine_draw_order dor;
    spine_array_slot doArr;
    spine_slot *slots;
    size_t n;
    int i, vi = 0, ti = 0;
    /* 分段状态：blendMode 相同的连续槽聚为一段（保 drawOrder 序，不重排）。
     * pending=1 表示下一个实际产出三角形的槽需要开新段（裁剪附件/空槽不算）。 */
    int do_segs = (maxSegments > 0 && outBlendModes && outSegStartTriangles);
    int seg_count = 0, cur_blend = -1, pending = do_segs ? 1 : 0;
    if (!ctx || !ctx->skeleton) return 0;
    /* 与 spR_collectDrawItems 相同：清掉上一帧残留的未闭合裁剪（见该函数注释） */
    spine_skeleton_clipping_clip_end_2(ctx->clipper);
    skel = ctx->skeleton;
    dor = spine_skeleton_get_draw_order(skel);
    doArr = spine_draw_order_get_applied_pose(dor);
    n = spine_array_slot_size(doArr);
    slots = spine_array_slot_buffer(doArr);
    for (i = 0; i < (int)n; ++i) {
        spine_slot slot = slots[i];
        spine_slot_pose pose = spine_slot_get_applied_pose(slot);
        spine_attachment att = spine_slot_pose_get_attachment(pose);
        spine_rtti rtti;
        spine_color ac, sc, kc;
        int page, j;
        float offX, pw, ph;
        if (!att) {
            spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
            continue;
        }
        sc = spine_slot_pose_get_color(pose);
        kc = spine_skeleton_get_color(skel);
        rtti = spine_attachment_get_rtti(att);

        /* 裁剪附件：启用裁剪，不产出几何（嵌套裁剪返回 0，忽略即可） */
        if (spine_rtti_is_exactly(rtti, spine_clipping_attachment_rtti())) {
            spine_skeleton_clipping_clip_start(ctx->clipper, skel, slot,
                                               spine_attachment_cast_to_clipping_attachment(att));
            continue;
        }
        /* blendMode 分段：槽的混合模式变化时，置 pending，下一个产出槽开新段 */
        if (do_segs) {
            spine_slot_data sd = spine_slot_get_data(slot);
            int bm = (int)spine_slot_data_get_blend_mode(sd);
            if (bm != cur_blend) { cur_blend = bm; pending = 1; }
        }

        if (spine_rtti_is_exactly(rtti, spine_region_attachment_rtti())) {
            spine_region_attachment region = spine_attachment_cast_to_region_attachment(att);
            spine_sequence seq = spine_region_attachment_get_sequence(region);
            int seqIndex = spine_sequence_resolve_index(seq, pose);
            spine_texture_region treg = spine_sequence_get_region(seq, seqIndex);
            spine_array_float offs, uvs;
            float verts[8];
            page = (treg) ? (int)(intptr_t)spine_texture_region_get_renderer_object(treg) : -1;
            if (page < 0 || page >= pageCount) {
                spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
                continue;
            }
            offX = atlasOffsets[page];
            pw = atlasPageW[page];
            ph = atlasPageH[page];
            if (vi + 4 > maxVerts || ti + 2 > maxTris) return -(vi + 4);
            if (pending) {
                if (seg_count >= maxSegments) return SP_R_NEED_SEGMENTS;
                outSegStartTriangles[seg_count] = ti;
                outBlendModes[seg_count] = cur_blend;
                seg_count++;
                pending = 0;
            }
            offs = spine_region_attachment_get_offsets(region, pose);
            spine_region_attachment_compute_world_vertices_1(region, slot,
                                                             spine_array_float_buffer(offs),
                                                             verts, 0, 2);
            uvs = spine_sequence_get_u_vs(seq, seqIndex);
            ac = spine_region_attachment_get_color(region);
            if (spine_skeleton_clipping_is_clipping(ctx->clipper)) {
                /* 裁剪中的 region 按 4 顶点 mesh 参与裁剪，输出为顶点/uv/索引数组 */
                spine_array_float cv, cu;
                spine_array_unsigned_short ct;
                int vlen, tcount, vcount, ttri;
                unsigned short rtris[6] = {0, 1, 2, 0, 2, 3};
                spine_skeleton_clipping_clip_triangles_2(ctx->clipper, verts, rtris, 6,
                                                         spine_array_float_buffer(uvs), 2);
                cv = spine_skeleton_clipping_get_clipped_vertices(ctx->clipper);
                cu = spine_skeleton_clipping_get_clipped_u_vs(ctx->clipper);
                ct = spine_skeleton_clipping_get_clipped_triangles(ctx->clipper);
                vlen = (int)spine_array_float_size(cv);
                tcount = (int)spine_array_unsigned_short_size(ct);
                if (tcount == 0) { /* 完全被裁掉 */
                    spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
                    continue;
                }
                vcount = vlen / 2;
                ttri = tcount / 3;
                if (vi + vcount > maxVerts || ti + ttri > maxTris) return -(vi + vcount);
                if (pending) {
                    if (seg_count >= maxSegments) return SP_R_NEED_SEGMENTS;
                    outSegStartTriangles[seg_count] = ti;
                    outBlendModes[seg_count] = cur_blend;
                    seg_count++;
                    pending = 0;
                }
                {
                    const float *cvb = spine_array_float_buffer(cv);
                    const float *cub = spine_array_float_buffer(cu);
                    const unsigned short *ctb = spine_array_unsigned_short_buffer(ct);
                    for (j = 0; j < vcount; j++) {
                        geo[vi * 2] = (cvb[j * 2] - minX) * zoom;
                        geo[vi * 2 + 1] = (cvb[j * 2 + 1] - minY) * zoom;
                        attrs[vi * 6] = (cub[j * 2] * pw + offX) / atlasW;
                        attrs[vi * 6 + 1] = cub[j * 2 + 1] * ph / atlasH;
                        attrs[vi * 6 + 2] = spine_color_get_r(ac) * spine_color_get_r(sc) * spine_color_get_r(kc);
                        attrs[vi * 6 + 3] = spine_color_get_g(ac) * spine_color_get_g(sc) * spine_color_get_g(kc);
                        attrs[vi * 6 + 4] = spine_color_get_b(ac) * spine_color_get_b(sc) * spine_color_get_b(kc);
                        attrs[vi * 6 + 5] = spine_color_get_a(ac) * spine_color_get_a(sc) * spine_color_get_a(kc);
                        vi++;
                    }
                    for (j = 0; j < tcount; j++)
                        tris[ti * 3 + j] = (unsigned short)(ctb[j] + (vi - vcount));
                    ti += ttri;
                }
            } else {
                /* 原逻辑：region 补四边形 [0,1,2, 0,2,3] */
                const float *uv = spine_array_float_buffer(uvs);
                for (j = 0; j < 4; j++) {
                    geo[vi * 2] = (verts[j * 2] - minX) * zoom;
                    geo[vi * 2 + 1] = (verts[j * 2 + 1] - minY) * zoom;
                    attrs[vi * 6] = (uv[j * 2] * pw + offX) / atlasW;
                    attrs[vi * 6 + 1] = uv[j * 2 + 1] * ph / atlasH;
                    attrs[vi * 6 + 2] = spine_color_get_r(ac) * spine_color_get_r(sc) * spine_color_get_r(kc);
                    attrs[vi * 6 + 3] = spine_color_get_g(ac) * spine_color_get_g(sc) * spine_color_get_g(kc);
                    attrs[vi * 6 + 4] = spine_color_get_b(ac) * spine_color_get_b(sc) * spine_color_get_b(kc);
                    attrs[vi * 6 + 5] = spine_color_get_a(ac) * spine_color_get_a(sc) * spine_color_get_a(kc);
                    vi++;
                }
                tris[ti * 3] = (unsigned short)(vi - 4);
                tris[ti * 3 + 1] = (unsigned short)(vi - 3);
                tris[ti * 3 + 2] = (unsigned short)(vi - 2);
                tris[ti * 3 + 3] = (unsigned short)(vi - 4);
                tris[ti * 3 + 4] = (unsigned short)(vi - 2);
                tris[ti * 3 + 5] = (unsigned short)(vi - 1);
                ti += 2;  /* 每个 region 贡献 2 个三角形（ti 按三角形计数） */
            }
            spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
        } else if (spine_rtti_is_exactly(rtti, spine_mesh_attachment_rtti())) {
            spine_mesh_attachment mesh = spine_attachment_cast_to_mesh_attachment(att);
            spine_vertex_attachment va = spine_mesh_attachment_cast_to_vertex_attachment(mesh);
            spine_sequence seq = spine_mesh_attachment_get_sequence(mesh);
            int seqIndex = spine_sequence_resolve_index(seq, pose);
            spine_texture_region treg = spine_sequence_get_region(seq, seqIndex);
            spine_array_float uvs;
            spine_array_unsigned_short trisArr;
            size_t vlen = spine_vertex_attachment_get_world_vertices_length(va); /* float 数，每顶点 2 个 */
            int vcount = (int)(vlen / 2);
            size_t tcount = spine_array_unsigned_short_size(spine_mesh_attachment_get_triangles(mesh));
            int ttri = (int)(tcount / 3);
            float *wv;
            page = (treg) ? (int)(intptr_t)spine_texture_region_get_renderer_object(treg) : -1;
            if (page < 0 || page >= pageCount) {
                spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
                continue;
            }
            offX = atlasOffsets[page];
            pw = atlasPageW[page];
            ph = atlasPageH[page];
            if (vi + vcount > maxVerts || ti + ttri > maxTris) return -(vi + vcount);
            if (pending) {
                if (seg_count >= maxSegments) return SP_R_NEED_SEGMENTS;
                outSegStartTriangles[seg_count] = ti;
                outBlendModes[seg_count] = cur_blend;
                seg_count++;
                pending = 0;
            }
            wv = _spR_tmpVerts(ctx, (int)vlen);
            if (!wv) return 0;
            spine_vertex_attachment_compute_world_vertices_1(va, skel, slot, 0, vlen, wv, 0, 2);
            uvs = spine_sequence_get_u_vs(seq, seqIndex);
            trisArr = spine_mesh_attachment_get_triangles(mesh);
            ac = spine_mesh_attachment_get_color(mesh);
            if (spine_skeleton_clipping_is_clipping(ctx->clipper)) {
                /* 裁剪中的 mesh：世界顶点喂给 clip_triangles_2，输出为顶点/uv/索引数组 */
                spine_array_float cv, cu;
                spine_array_unsigned_short ct;
                int cvlen, ctcount, cvcount, cttri;
                spine_skeleton_clipping_clip_triangles_2(ctx->clipper, wv,
                                                         spine_array_unsigned_short_buffer(trisArr), tcount,
                                                         spine_array_float_buffer(uvs), 2);
                cv = spine_skeleton_clipping_get_clipped_vertices(ctx->clipper);
                cu = spine_skeleton_clipping_get_clipped_u_vs(ctx->clipper);
                ct = spine_skeleton_clipping_get_clipped_triangles(ctx->clipper);
                cvlen = (int)spine_array_float_size(cv);
                ctcount = (int)spine_array_unsigned_short_size(ct);
                if (ctcount == 0) { /* 完全被裁掉 */
                    spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
                    continue;
                }
                cvcount = cvlen / 2;
                cttri = ctcount / 3;
                if (vi + cvcount > maxVerts || ti + cttri > maxTris) return -(vi + cvcount);
                if (pending) {
                    if (seg_count >= maxSegments) return SP_R_NEED_SEGMENTS;
                    outSegStartTriangles[seg_count] = ti;
                    outBlendModes[seg_count] = cur_blend;
                    seg_count++;
                    pending = 0;
                }
                {
                    const float *cvb = spine_array_float_buffer(cv);
                    const float *cub = spine_array_float_buffer(cu);
                    const unsigned short *ctb = spine_array_unsigned_short_buffer(ct);
                    for (j = 0; j < cvcount; j++) {
                        geo[vi * 2] = (cvb[j * 2] - minX) * zoom;
                        geo[vi * 2 + 1] = (cvb[j * 2 + 1] - minY) * zoom;
                        attrs[vi * 6] = (cub[j * 2] * pw + offX) / atlasW;
                        attrs[vi * 6 + 1] = cub[j * 2 + 1] * ph / atlasH;
                        attrs[vi * 6 + 2] = spine_color_get_r(ac) * spine_color_get_r(sc) * spine_color_get_r(kc);
                        attrs[vi * 6 + 3] = spine_color_get_g(ac) * spine_color_get_g(sc) * spine_color_get_g(kc);
                        attrs[vi * 6 + 4] = spine_color_get_b(ac) * spine_color_get_b(sc) * spine_color_get_b(kc);
                        attrs[vi * 6 + 5] = spine_color_get_a(ac) * spine_color_get_a(sc) * spine_color_get_a(kc);
                        vi++;
                    }
                    for (j = 0; j < ctcount; j++)
                        tris[ti * 3 + j] = (unsigned short)(ctb[j] + (vi - cvcount));
                    ti += cttri;
                }
            } else {
                /* 原逻辑：mesh 附件原样 + 顶点偏移 */
                const float *uv = spine_array_float_buffer(uvs);
                const unsigned short *tr = spine_array_unsigned_short_buffer(trisArr);
                for (j = 0; j < vcount; j++) {
                    geo[vi * 2] = (wv[j * 2] - minX) * zoom;
                    geo[vi * 2 + 1] = (wv[j * 2 + 1] - minY) * zoom;
                    attrs[vi * 6] = (uv[j * 2] * pw + offX) / atlasW;
                    attrs[vi * 6 + 1] = uv[j * 2 + 1] * ph / atlasH;
                    attrs[vi * 6 + 2] = spine_color_get_r(ac) * spine_color_get_r(sc) * spine_color_get_r(kc);
                    attrs[vi * 6 + 3] = spine_color_get_g(ac) * spine_color_get_g(sc) * spine_color_get_g(kc);
                    attrs[vi * 6 + 4] = spine_color_get_b(ac) * spine_color_get_b(sc) * spine_color_get_b(kc);
                    attrs[vi * 6 + 5] = spine_color_get_a(ac) * spine_color_get_a(sc) * spine_color_get_a(kc);
                    vi++;
                }
                for (j = 0; j < (int)tcount; j++)
                    tris[ti * 3 + j] = (unsigned short)(tr[j] + (vi - vcount));
                ti += ttri;
            }
            spine_skeleton_clipping_clip_end_1(ctx->clipper, slot);
        }
        /* 其余附件类型（bounding box/path/point）不渲染 */
    }
    if (outTriangles) *outTriangles = ti;
    if (do_segs) {
        if (outSegmentCount) *outSegmentCount = seg_count;
        if (seg_count > 0) outSegStartTriangles[seg_count] = ti;
    }
    return vi;
}

/* 旧 ABI 包装：不输出段信息（等价于 maxSegments=0 的 buildMeshEx，行为不变） */
SP_R_API int spR_buildMesh(void *vctx, float minX, float minY, float zoom,
                           const float *atlasOffsets, const float *atlasPageW,
                           const float *atlasPageH, int pageCount,
                           float atlasW, float atlasH,
                           float *geo, float *attrs, unsigned short *tris,
                           int maxVerts, int maxTris, int *outTriangles) {
    return spR_buildMeshEx(vctx, minX, minY, zoom,
                           atlasOffsets, atlasPageW, atlasPageH, pageCount,
                           atlasW, atlasH,
                           geo, attrs, tris, maxVerts, maxTris, outTriangles,
                           NULL, NULL, 0, NULL);
}

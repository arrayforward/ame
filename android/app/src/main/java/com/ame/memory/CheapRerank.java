/*
 * 所属模块：app——cheap 特征重排模型加载器（端侧默认重排档）。
 * 主体思路：
 *   - 模型 bench/models/cheap_model.json（11 特征逻辑回归，零 LLM）随 APK assets 打包；
 *   - 每个 session 的 Engine 创建后调用 ensureLoaded，把 JSON 注入 JNI；
 *     之后 recall 自动走 候选池60 → cheap 特征重排 → topN（约毫秒级，纯 CPU）；
 *   - 任何失败只记日志，recall 回退引擎原序（bm25 行为），功能不受影响；
 *   - 深度重排开关不受影响：开启时仍由 MemoryOrchestrator 走 M3 全池语义打分。
 * 依赖关系：AmeEngine（JNI nativeLoadCheapModel）、android.content.Context。
 */
package com.ame.memory;

import android.content.Context;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

public final class CheapRerank {

    private static final String TAG = "AmeMem";
    private static final String MODEL_NAME = "cheap_model.json";

    private CheapRerank() {
    }

    /** 注入 cheap 重排模型（默认启用）；失败仅降级，不抛异常 */
    public static void ensureLoaded(Context ctx, AmeEngine engine) {
        try (InputStream in = ctx.getAssets().open(MODEL_NAME)) {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) != -1) bos.write(buf, 0, n);
            String json = new String(bos.toByteArray(), StandardCharsets.UTF_8);
            int r = engine.loadCheapModel(json);
            Log.i(TAG, r == 1 ? "cheap 重排已默认启用（池60→特征重排→topN）"
                    : "cheap 模型解析失败，回退引擎原序 rc=" + r);
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "cheap 模型加载失败，回退引擎原序: " + e);
        }
    }
}

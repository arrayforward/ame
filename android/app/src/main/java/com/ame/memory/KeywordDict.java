/*
 * 所属模块：app——关键词词典全局加载器（多 session 共享）。
 * 主体思路：
 *   - 词典文件与 AC 缓存放 filesDir/dict/（全局共享，全 App 只有一份）；
 *   - 首次运行从 APK assets 拷出 merged.txt（67.8 万词，构建期由 Gradle 拷入 assets），
 *     此后各 session 复用，不再重复拷贝；
 *   - 每个 session 的 Engine 创建后调用 ensureLoaded——有 AC 缓存时毫秒级载入，
 *     首次全量构建约百毫秒级并覆写缓存供后续 session 复用；
 *   - 任何失败只记日志（召回退回字符二元组兜底通道，功能不受影响）。
 * 依赖关系：AmeEngine（JNI）、android.content.Context。
 */
package com.ame.memory;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public final class KeywordDict {

    private static final String TAG = "AmeMem";
    private static final String DICT_NAME = "merged.txt";
    private static final String AC_NAME = "merged.ac";
    private static volatile boolean attempted = false;  // 每次进程一次全量尝试标记

    private KeywordDict() {
    }

    /** 确保词典已加载进该引擎实例；可在任意线程调用（内部同步） */
    public static synchronized void ensureLoaded(Context ctx, AmeEngine engine) {
        try {
            File dir = new File(ctx.getFilesDir(), "dict");
            if (!dir.exists()) dir.mkdirs();
            File dict = new File(dir, DICT_NAME);
            if (!dict.exists()) {
                Log.i(TAG, "首次运行：从 assets 拷贝关键词词典 " + DICT_NAME);
                copyAsset(ctx, DICT_NAME, dict);
            }
            File cache = new File(dir, AC_NAME);
            int n = engine.loadKeywordDict(dict.getAbsolutePath(), cache.getAbsolutePath());
            if (n >= 0) {
                if (!attempted) Log.i(TAG, "关键词词典加载完成: " + n + " 条");
            } else {
                Log.e(TAG, "关键词词典加载失败（召回退回二元组兜底）");
            }
        } catch (Exception e) {
            Log.e(TAG, "词典加载异常（召回退回二元组兜底）", e);
        } finally {
            attempted = true;
        }
    }

    private static void copyAsset(Context ctx, String name, File out) throws IOException {
        try (InputStream in = ctx.getAssets().open(name);
             FileOutputStream fos = new FileOutputStream(out)) {
            byte[] buf = new byte[64 * 1024];
            int r;
            while ((r = in.read(buf)) > 0) fos.write(buf, 0, r);
        }
    }
}

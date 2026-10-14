/*
 * 所属模块：app——导游模式的导览方案 PDF 解析。
 * 主体思路：PdfBox（纯 JVM，不联网）抽取全文文本 + 内嵌图（递归表单 XObject）；
 *   无内嵌图或补充版式时用系统 PdfRenderer 把每页渲染成位图——保证"带图片的导览
 *   方案"一定有图可看。产物全部落 session 目录：
 *     guide.txt       方案全文（system prompt 截取注入 + 切块灌入 AME 记忆图）
 *     guide_img/      PDF 内嵌图（≤1024 JPEG）
 *     guide_page/     页渲染图（无内嵌图时的兜底版式图）
 * 依赖关系：pdfbox-android（com.tom-roush）、android.graphics.pdf.PdfRenderer。
 *   （Java 版把 Kotlin use{} 改为 try-with-resources，逻辑一致。）
 */
package com.ame.memory;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.pdf.PdfRenderer;
import android.os.ParcelFileDescriptor;

import com.tom_roush.pdfbox.android.PDFBoxResourceLoader;
import com.tom_roush.pdfbox.pdmodel.PDDocument;
import com.tom_roush.pdfbox.pdmodel.PDResources;
import com.tom_roush.pdfbox.pdmodel.graphics.PDXObject;
import com.tom_roush.pdfbox.pdmodel.graphics.form.PDFormXObject;
import com.tom_roush.pdfbox.pdmodel.graphics.image.PDImageXObject;
import com.tom_roush.pdfbox.text.PDFTextStripper;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public final class GuidePdf {

    /** 解析结果：全文 + 图片目录（内嵌图优先，无内嵌图时为页渲染图） */
    public static final class Plan {
        public final String text;
        public final File imageDir;
        public final List<File> images;

        Plan(String text, File imageDir, List<File> images) {
            this.text = text;
            this.imageDir = imageDir;
            this.images = images;
        }
    }

    /** 页渲染兜底的最大页数（防超大 PDF 撑爆存储/内存） */
    private static final int MAX_RENDER_PAGES = 20;

    private GuidePdf() {
    }

    /** 解析 PDF 到 session 目录；调用方负责切后台线程 */
    public static Plan parse(Context context, File pdf, File sessionDir) throws IOException {
        PDFBoxResourceLoader.init(context.getApplicationContext());
        File imgDir = new File(sessionDir, "guide_img");
        imgDir.mkdirs();
        File pageDir = new File(sessionDir, "guide_page");
        pageDir.mkdirs();

        // use{} → try-with-resources（PDDocument 实现 Closeable）
        try (PDDocument doc = PDDocument.load(pdf)) {
            // 1) 全文文本（跨页拼接；stripper 已处理常见编码/换行）
            String text = new PDFTextStripper().getText(doc).trim();
            Files.write(new File(sessionDir, "guide.txt").toPath(),
                    text.getBytes(StandardCharsets.UTF_8));

            // 2) 内嵌图（递归表单 XObject，可能藏在嵌套图层里）
            List<File> embedded = new ArrayList<>();
            int pi = 0;
            for (Object pageObj : doc.getPages()) {
                com.tom_roush.pdfbox.pdmodel.PDPage page =
                        (com.tom_roush.pdfbox.pdmodel.PDPage) pageObj;
                PDResources res = page.getResources();
                if (res != null) {
                    collectImages(res, imgDir, "p" + pi, embedded);
                }
                pi++;
            }

            // 3) 无内嵌图时渲染页图兜底；有内嵌图也渲染前 3 页作版式概览
            List<File> pageImgs = renderPages(pdf, pageDir,
                    embedded.isEmpty() ? MAX_RENDER_PAGES : 3);
            // distinct() 等价：按插入顺序去重
            Set<File> all = new LinkedHashSet<>(embedded);
            all.addAll(pageImgs);
            List<File> images = new ArrayList<>(all);
            return new Plan(text, embedded.isEmpty() ? pageDir : imgDir, images);
        }
    }

    /** 递归抽取 XObject 中的位图（PDFormXObject 内可能再套图/表单） */
    private static void collectImages(PDResources res, File outDir, String prefix,
                                      List<File> out) throws IOException {
        int idx = 0;
        // COSName 用 var 推断，避免直接 import（pdfbox-android 的打包口径）
        for (var name : res.getXObjectNames()) {
            PDXObject xo = res.getXObject(name);
            if (xo == null) continue;
            if (xo instanceof PDImageXObject) {
                Bitmap bmp = ((PDImageXObject) xo).getImage();
                if (bmp == null) continue;
                File f = new File(outDir, prefix + "_" + (idx++) + ".jpg");
                saveJpeg(bmp, f);
                out.add(f);
            } else if (xo instanceof PDFormXObject) {
                PDResources sub = ((PDFormXObject) xo).getResources();
                if (sub != null) {
                    collectImages(sub, outDir, prefix + "f" + (idx++), out);
                }
            }
            // 其他 XObject 类型忽略（等价 Kotlin when 的 else -> Unit）
        }
    }

    /** 系统 PdfRenderer 渲染页图（seekable 本地文件才可打开） */
    private static List<File> renderPages(File pdf, File outDir, int maxPages)
            throws IOException {
        List<File> out = new ArrayList<>();
        // use{} 嵌套 → try-with-resources 嵌套（PdfRenderer/ParcelFileDescriptor 均可自动关闭）
        try (ParcelFileDescriptor pfd = ParcelFileDescriptor.open(
                pdf, ParcelFileDescriptor.MODE_READ_ONLY);
             PdfRenderer renderer = new PdfRenderer(pfd)) {
            int n = Math.min(renderer.getPageCount(), maxPages);
            for (int i = 0; i < n; i++) {
                PdfRenderer.Page page = renderer.openPage(i);
                float scale = 1024f / page.getWidth();
                Bitmap bmp = Bitmap.createBitmap(
                        Math.max(1, (int) (page.getWidth() * scale)),    // coerceAtLeast(1)
                        Math.max(1, (int) (page.getHeight() * scale)),
                        Bitmap.Config.ARGB_8888);
                bmp.eraseColor(Color.WHITE);
                page.render(bmp, null, null, PdfRenderer.Page.RENDER_MODE_FOR_PRINT);
                page.close();
                File f = new File(outDir, "page_" + i + ".jpg");
                saveJpeg(bmp, f);
                out.add(f);
            }
        }
        return out;
    }

    private static void saveJpeg(Bitmap bmp, File f) throws IOException {
        // 统一最长边 ≤1024，与聊天图同口径，方便 UI 直接解码
        int w = bmp.getWidth();
        int h = bmp.getHeight();
        Bitmap b;
        if (Math.max(w, h) > 1024) {
            float s = 1024f / Math.max(w, h);
            b = Bitmap.createScaledBitmap(bmp, (int) (w * s), (int) (h * s), true);
        } else {
            b = bmp;
        }
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        b.compress(Bitmap.CompressFormat.JPEG, 85, bos);
        Files.write(f.toPath(), bos.toByteArray());
    }
}

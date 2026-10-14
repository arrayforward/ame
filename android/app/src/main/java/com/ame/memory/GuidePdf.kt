/*
 * 所属模块：app——导游模式的导览方案 PDF 解析。
 * 主体思路：PdfBox（纯 JVM，不联网）抽取全文文本 + 内嵌图（递归表单 XObject）；
 *   无内嵌图或补充版式时用系统 PdfRenderer 把每页渲染成位图——保证"带图片的导览
 *   方案"一定有图可看。产物全部落 session 目录：
 *     guide.txt       方案全文（system prompt 截取注入 + 切块灌入 AME 记忆图）
 *     guide_img/      PDF 内嵌图（≤1024 JPEG）
 *     guide_page/     页渲染图（无内嵌图时的兜底版式图）
 * 依赖关系：pdfbox-android（com.tom-roush）、android.graphics.pdf.PdfRenderer。
 */
package com.ame.memory

import android.content.Context
import android.graphics.Bitmap
import android.graphics.pdf.PdfRenderer
import android.os.ParcelFileDescriptor
import com.tom_roush.pdfbox.android.PDFBoxResourceLoader
import com.tom_roush.pdfbox.pdmodel.PDDocument
import com.tom_roush.pdfbox.pdmodel.PDResources
import com.tom_roush.pdfbox.pdmodel.graphics.PDXObject
import com.tom_roush.pdfbox.pdmodel.graphics.form.PDFormXObject
import com.tom_roush.pdfbox.pdmodel.graphics.image.PDImageXObject
import com.tom_roush.pdfbox.text.PDFTextStripper
import java.io.ByteArrayOutputStream
import java.io.File

object GuidePdf {

    /** 解析结果：全文 + 图片目录（内嵌图优先，无内嵌图时为页渲染图） */
    data class Plan(val text: String, val imageDir: File, val images: List<File>)

    /** 页渲染兜底的最大页数（防超大 PDF 撑爆存储/内存） */
    private const val MAX_RENDER_PAGES = 20

    /** 解析 PDF 到 session 目录；调用方负责切后台线程 */
    fun parse(context: Context, pdf: File, sessionDir: File): Plan {
        PDFBoxResourceLoader.init(context.applicationContext)
        val imgDir = File(sessionDir, "guide_img").apply { mkdirs() }
        val pageDir = File(sessionDir, "guide_page").apply { mkdirs() }

        val doc = PDDocument.load(pdf)
        doc.use {
            // 1) 全文文本（跨页拼接；stripper 已处理常见编码/换行）
            val text = PDFTextStripper().getText(doc).trim()
            File(sessionDir, "guide.txt").writeText(text)

            // 2) 内嵌图（递归表单 XObject，可能藏在嵌套图层里）
            val embedded = mutableListOf<File>()
            it.pages.forEachIndexed { pi, page ->
                page.resources?.let { res -> collectImages(res, imgDir, "p${pi}", embedded) }
            }

            // 3) 无内嵌图时渲染页图兜底；有内嵌图也渲染前 3 页作版式概览
            val pageImgs = renderPages(pdf, pageDir,
                maxPages = if (embedded.isEmpty()) MAX_RENDER_PAGES else 3)
            val images = (embedded + pageImgs).distinct()
            return Plan(text, if (embedded.isEmpty()) pageDir else imgDir, images)
        }
    }

    /** 递归抽取 XObject 中的位图（PDFormXObject 内可能再套图/表单） */
    private fun collectImages(res: PDResources, outDir: File, prefix: String,
                              out: MutableList<File>) {
        var idx = 0
        for (name in res.xObjectNames) {
            val xo: PDXObject = res.getXObject(name) ?: continue
            when (xo) {
                is PDImageXObject -> {
                    val bmp = xo.image ?: continue
                    val f = File(outDir, "${prefix}_${idx++}.jpg")
                    saveJpeg(bmp, f)
                    out.add(f)
                }
                is PDFormXObject -> xo.resources?.let {
                    collectImages(it, outDir, "${prefix}f${idx++}", out)
                }
                else -> Unit
            }
        }
    }

    /** 系统 PdfRenderer 渲染页图（seekable 本地文件才可打开） */
    private fun renderPages(pdf: File, outDir: File, maxPages: Int): List<File> {
        val out = mutableListOf<File>()
        val pfd = ParcelFileDescriptor.open(pdf, ParcelFileDescriptor.MODE_READ_ONLY)
        pfd.use {
            val renderer = PdfRenderer(it)
            renderer.use {
                val n = minOf(it.pageCount, maxPages)
                for (i in 0 until n) {
                    val page = it.openPage(i)
                    val scale = 1024f / page.width
                    val bmp = Bitmap.createBitmap(
                        (page.width * scale).toInt().coerceAtLeast(1),
                        (page.height * scale).toInt().coerceAtLeast(1),
                        Bitmap.Config.ARGB_8888)
                    bmp.eraseColor(android.graphics.Color.WHITE)
                    page.render(bmp, null, null, PdfRenderer.Page.RENDER_MODE_FOR_PRINT)
                    page.close()
                    val f = File(outDir, "page_$i.jpg")
                    saveJpeg(bmp, f)
                    out.add(f)
                }
            }
        }
        return out
    }

    private fun saveJpeg(bmp: Bitmap, f: File) {
        // 统一最长边 ≤1024，与聊天图同口径，方便 UI 直接解码
        val w = bmp.width; val h = bmp.height
        val b = if (maxOf(w, h) > 1024) {
            val s = 1024f / maxOf(w, h)
            Bitmap.createScaledBitmap(bmp, (w * s).toInt(), (h * s).toInt(), true)
        } else bmp
        val bos = ByteArrayOutputStream()
        b.compress(Bitmap.CompressFormat.JPEG, 85, bos)
        f.writeBytes(bos.toByteArray())
    }
}

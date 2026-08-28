#!/usr/bin/env python3
"""SVG -> PDF 转换脚本。

把 src_dir 下所有 .svg 转成同名 .pdf 写入 out_dir。
后端自动探测（按优先级）：cairosvg > svglib > rsvg-convert > inkscape。

用法:
    python3 svg2pdf.py [src_dir] [out_dir]
默认 src_dir = 本脚本上级目录（即 exp/pic），out_dir = 本脚本所在目录（exp/pic/pdf）。
"""
import os
import sys
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SRC = os.path.normpath(os.path.join(HERE, ".."))  # exp/pic
DEFAULT_OUT = HERE                                        # exp/pic/pdf


def find_backend():
    try:
        import cairosvg
        return ("cairosvg", cairosvg)
    except ImportError:
        pass
    try:
        import svglib
        import reportlab
        return ("svglib", (svglib, reportlab))
    except ImportError:
        pass
    for exe in ("rsvg-convert", "inkscape"):
        if shutil.which(exe):
            return ("cmd", exe)
    return None


def convert_one(backend, src, dst):
    kind, mod = backend
    if kind == "cairosvg":
        mod.svg2pdf(url=src, write_to=dst)
    elif kind == "svglib":
        from svglib.svglib import svg2rlg
        from reportlab.graphics import renderPDF
        drawing = svg2rlg(src)
        if drawing is None:
            raise RuntimeError("svglib 无法解析 %s" % src)
        renderPDF.drawToFile(drawing, dst)
    elif mod == "rsvg-convert":
        subprocess.run([mod, "-f", "pdf", "-o", dst, src], check=True)
    else:  # inkscape
        subprocess.run([mod, src, "--export-type=pdf",
                        "--export-filename=" + dst], check=True)


def main():
    src_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    out_dir = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT
    os.makedirs(out_dir, exist_ok=True)

    backend = find_backend()
    if backend is None:
        print("没有可用的转换后端，请安装其中之一：")
        print("  pip install cairosvg        (推荐，需系统 libcairo2)")
        print("  pip install svglib reportlab")
        print("或安装系统工具 rsvg-convert / inkscape")
        sys.exit(1)
    print("backend: %s" % backend[0])

    svgs = sorted(f for f in os.listdir(src_dir) if f.lower().endswith(".svg"))
    if not svgs:
        print("src_dir %s 下没有 .svg 文件" % src_dir)
        sys.exit(0)
    for f in svgs:
        src = os.path.join(src_dir, f)
        dst = os.path.join(out_dir, os.path.splitext(f)[0] + ".pdf")
        convert_one(backend, src, dst)
        print("ok: %s -> %s" % (f, os.path.basename(dst)))
    print("done: %d files -> %s" % (len(svgs), out_dir))


if __name__ == "__main__":
    main()

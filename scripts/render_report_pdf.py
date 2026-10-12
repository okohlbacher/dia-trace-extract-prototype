#!/usr/bin/env python3
"""Render a Markdown report (with relative figure links) to PDF via Markdown -> HTML -> WeasyPrint.
usage: render_report_pdf.py REPORT.md OUT.pdf"""
import sys, os, re, markdown
from weasyprint import HTML
src, out = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()
text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
html = markdown.markdown(text, extensions=["tables", "fenced_code", "toc", "sane_lists"])
css = """
@page { size: A4; margin: 18mm 16mm 18mm 16mm; @bottom-right { content: counter(page) " / " counter(pages); font-size: 8pt; color: #666; } }
body { font-family: "DejaVu Sans", "Liberation Sans", Arial, sans-serif; font-size: 9.6pt; line-height: 1.38; color: #111; }
h1 { font-size: 18pt; margin: 0 0 8pt 0; } h2 { font-size: 13pt; margin: 16pt 0 6pt 0; border-bottom: 1px solid #bbb; padding-bottom: 2pt; page-break-after: avoid; }
h3 { font-size: 11pt; margin: 12pt 0 4pt 0; page-break-after: avoid; }
p { margin: 4pt 0; text-align: left; } ul, ol { margin: 3pt 0 3pt 14pt; padding: 0; } li { margin: 1.5pt 0; }
table { border-collapse: collapse; margin: 6pt 0; font-size: 8.6pt; page-break-inside: avoid; }
th, td { border: 1px solid #ccc; padding: 2.5pt 5pt; vertical-align: top; } th { background: #f0f2f5; text-align: left; }
code { font-family: "DejaVu Sans Mono", monospace; font-size: 8.4pt; background: #f4f4f4; padding: 0 2pt; }
pre { background: #f4f4f4; padding: 6pt; font-size: 8pt; white-space: pre-wrap; }
img { max-width: 100%; height: auto; display: block; margin: 6pt auto; page-break-inside: avoid; }
a { color: #1a4f8b; text-decoration: none; }
"""
doc = f"<!doctype html><html><head><meta charset='utf-8'><style>{css}</style></head><body>{html}</body></html>"
HTML(string=doc, base_url=os.path.dirname(os.path.abspath(src)) + "/").write_pdf(out)
print("->", out, os.path.getsize(out) // 1024, "KiB")

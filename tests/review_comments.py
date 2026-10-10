"""审查自有源码中的注释语言与待办标记，不把字符串中的网址误识别为注释。"""

from pathlib import Path
import re
import sys


TOKEN = re.compile(
    r'R"(?P<delimiter>[^()\s\\]{0,16})\([\s\S]*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\])*"'
    r"|'(?:\\.|[^'\\])*'"
    r"|(?P<comment>//[^\n]*|/\*[\s\S]*?\*/)"
)
CHINESE = re.compile(r"[\u3400-\u9fff]")
ENGLISH_SENTENCE = re.compile(r"\b[A-Za-z]{2,}[ -]+[A-Za-z]{2,}[ -]+[A-Za-z]{2,}\b")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    files = sorted(
        path for path in (root / "src").rglob("*")
        if path.suffix in {".cpp", ".h", ".glsl"}
    )
    files.append(root / "tests" / "widget_regression.cpp")
    problems = []
    comments = 0
    for path in files:
        source = path.read_text(encoding="utf-8-sig")
        for match in TOKEN.finditer(source):
            comment = match.group("comment")
            if comment is None:
                continue
            comments += 1
            line = source.count("\n", 0, match.start()) + 1
            reason = ""
            if re.search(r"\b(?:TODO|FIXME|HACK)\b", comment):
                reason = "存在未处理的待办标记"
            elif re.search(r"[A-Za-z]", comment) and not CHINESE.search(comment):
                reason = "注释缺少中文说明"
            elif ENGLISH_SENTENCE.search(comment):
                reason = "注释仍包含英文句子"
            if reason:
                problems.append(f"{path.relative_to(root)}:{line}: {reason}: {comment.strip()}")
    for problem in problems:
        print(problem)
    print(f"已检查 {len(files)} 个源码文件、{comments} 条注释；发现 {len(problems)} 项问题。")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
